#include "NativeObjectWitness.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Slic3r::NativeObjectWitness {
namespace {
constexpr uint64_t MAX_SOURCE = 1073741824, MAX_EVENTS = 100000, MAX_EVENT_BYTES = 32 * 1024 * 1024;
using Json = nlohmann::json;

void require(bool valid, const char* error) {
    if (!valid) throw std::runtime_error(error);
}
bool valid_id(uint64_t value) {
    return value > 0 && value <= INT64_MAX;
}
Json identity(Instance value) {
    return {{"printObjectId", std::to_string(value.print_object)}, {"modelObjectId", std::to_string(value.model_object)},
            {"modelInstanceId", std::to_string(value.model_instance)}};
}
const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Perimeter: return "PERIMETER";
    case Kind::Infill: return "INFILL";
    case Kind::Ironing: return "IRONING";
    }
    throw std::runtime_error("NATIVE_OBJECT_WITNESS_KIND");
}
const char* generator(Kind kind) {
    return kind == Kind::Perimeter ? "EXTRUDE_PERIMETERS" : "EXTRUDE_INFILL";
}
const char* writer_name(Writer writer) {
    switch (writer) {
    case Writer::LinearXY: return "LINEAR_XY";
    case Writer::LinearXYZ: return "LINEAR_XYZ";
    case Writer::ArcCW: return "ARC_CW";
    case Writer::ArcCCW: return "ARC_CCW";
    }
    throw std::runtime_error("NATIVE_OBJECT_WITNESS_WRITER");
}
std::string hexadecimal(std::string_view bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}
std::string decimal(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(std::numeric_limits<double>::max_digits10);
    text << value;
    require(static_cast<bool>(text), "NATIVE_OBJECT_WITNESS_DELTA");
    return text.str();
}

} // namespace

bool Instance::operator<(const Instance& other) const {
    return std::tie(print_object, model_instance, model_object) <
        std::tie(other.print_object, other.model_instance, other.model_object);
}
Operation Operation::shifted(uint64_t bytes) const {
    if (!m_collector) return {};
    require(bytes <= MAX_SOURCE - m_base, "NATIVE_OBJECT_WITNESS_RANGE");
    Operation shifted = *this;
    shifted.m_base += bytes;
    return shifted;
}
void Operation::emitted(uint64_t begin, int role, Writer writer, double delta, bool force_no_extrusion,
                        std::string_view fragment) const {
    if (m_collector) m_collector->emitted(*this, begin, role, writer, delta, force_no_extrusion, fragment);
}
Collector::Collector(bool writer, Cancel cancel) : m_writer(writer), m_cancel(std::move(cancel)) {}

Operation Collector::operation(Instance instance, Kind kind, uint64_t layer_base) {
    require(valid_id(instance.print_object) && valid_id(instance.model_instance) && valid_id(instance.model_object)
        && layer_base <= MAX_SOURCE && m_parent < MAX_SOURCE, "NATIVE_OBJECT_WITNESS_ID");
    Operation result;
    result.m_collector = this;
    result.m_instance = instance;
    result.m_kind = kind;
    result.m_parent = ++m_parent;
    result.m_base = layer_base;
    return result;
}
void Collector::emitted(const Operation& operation, uint64_t begin, int role, Writer writer,
                        double delta, bool force_no_extrusion, std::string_view fragment) {
    if (fragment.empty()) return;
    if (m_cancel) m_cancel();
    require(begin <= MAX_SOURCE - operation.m_base && fragment.size() <= MAX_SOURCE - (operation.m_base + begin)
        && role >= 0 && std::isfinite(delta) && fragment.find('\0') == std::string_view::npos,
        "NATIVE_OBJECT_WITNESS_RANGE");
    require(m_events.size() < MAX_EVENTS && fragment.size() <= MAX_EVENT_BYTES - m_bytes
        && fragment.size() <= 65536, "NATIVE_OBJECT_WITNESS_LIMIT");
    const uint64_t absolute = operation.m_base + begin;
    if (m_events.size() != m_sealed) {
        const auto& previous = m_events.back();
        require(absolute >= previous.begin + previous.bytes.size(), "NATIVE_OBJECT_WITNESS_ORDER");
    }
    m_events.push_back({operation.m_instance, operation.m_kind, writer, operation.m_parent, m_batch + 1,
                        absolute, 0, role, delta, force_no_extrusion, std::string(fragment)});
    m_bytes += fragment.size();
}
void Collector::seal_layer(std::string_view exact) {
    require(exact.size() <= MAX_SOURCE - m_stage, "NATIVE_OBJECT_WITNESS_LIMIT");
    for (uint64_t i = m_sealed; i < m_events.size(); ++i) {
        if (m_cancel) m_cancel();
        auto& event = m_events[i];
        require(event.begin <= exact.size() && event.bytes.size() <= exact.size() - event.begin
            && exact.substr(event.begin, event.bytes.size()) == event.bytes, "NATIVE_OBJECT_WITNESS_BYTES");
        event.stage_begin = m_stage + event.begin;
    }
    m_stage += exact.size();
    m_sealed = m_events.size();
    ++m_batch;
}
void Collector::write(Document document, const std::vector<Instance>& dictionary, std::string_view gcode_sha,
                      uint64_t gcode_size, const Feed& feed) const {
    require(m_sealed == m_events.size() && m_sealed > 0 && m_stage > 0 && gcode_size > 0 && gcode_size <= MAX_SOURCE,
        "NATIVE_OBJECT_WITNESS_EMPTY");
    require(gcode_sha.size() == 64 && std::all_of(gcode_sha.begin(), gcode_sha.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }), "NATIVE_OBJECT_WITNESS_GCODE_SHA");
    const std::set<Instance> known(dictionary.begin(), dictionary.end());
    for (const auto& event : m_events)
        require(known.count(event.instance) == 1, "NATIVE_OBJECT_WITNESS_DICTIONARY");

    Json header{{"coverage", "PARTIAL"}, {"finalSha256", gcode_sha}};
    switch (document) {
    case Document::Membership: header["schema"] = "corvus-native-object-membership-witness/1"; break;
    case Document::Audit: header["schema"] = "corvus-native-generator-audit/1"; break;
    case Document::Operation:
        header["schema"] = "corvus-native-operation-map/1";
        header["byteLength"] = std::to_string(gcode_size);
        header["stageLengths"] = {std::to_string(m_stage), std::to_string(gcode_size)};
        header["spans"] = Json::array({Json{{"begin", "0"}, {"end", std::to_string(gcode_size)},
            {"relation", "UNATTRIBUTED"}, {"origins", Json::array()}}});
        header["edges"] = Json::array();
        header["deletions"] = Json::array();
        break;
    case Document::Writer:
        require(m_writer, "NATIVE_OBJECT_WITNESS_WRITER_DISABLED");
        header["schema"] = "corvus-native-writer-witness/1";
        header["observation"] = "OBSERVED_WRITER_CALL";
        break;
    }
    auto prefix = header.dump();
    prefix.pop_back();
    feed(prefix);
    if (document == Document::Membership) {
        feed(",\"instances\":[");
        for (size_t i = 0; i < dictionary.size(); ++i) {
            if (m_cancel) m_cancel();
            if (i) feed(",");
            feed(identity(dictionary[i]).dump());
        }
        feed("]");
    }
    if (document == Document::Operation) {
        feed(",\"origins\":[");
        for (size_t i = 0; i < m_events.size(); ++i) {
            if (m_cancel) m_cancel();
            if (i) feed(",");
            const auto& event = m_events[i];
            auto row = identity(event.instance);
            row.update(Json{{"id", std::to_string(i + 1)}, {"auditEventId", std::to_string(i + 1)},
                {"generator", "EXTRUDE"}, {"nativeRole", std::to_string(event.role)}, {"kind", kind_name(event.kind)},
                {"parentOperation", std::to_string(event.parent)}, {"parentGenerator", generator(event.kind)},
                {"sourceHex", hexadecimal(event.bytes)}});
            feed(row.dump());
        }
        feed("],\"parents\":[");
        std::set<uint64_t> parents;
        for (const auto& event : m_events) if (parents.insert(event.parent).second) {
            if (m_cancel) m_cancel();
            if (parents.size() > 1) feed(",");
            auto row = identity(event.instance);
            row.update(Json{{"id", std::to_string(event.parent)}, {"kind", kind_name(event.kind)}, {"generator", generator(event.kind)}});
            feed(row.dump());
        }
        feed("],\"roots\":[");
        for (size_t i = 0; i < m_events.size(); ++i) {
            if (m_cancel) m_cancel();
            if (i) feed(",");
            const auto& event = m_events[i];
            feed(Json{{"stage", "0"}, {"begin", std::to_string(event.stage_begin)},
                {"end", std::to_string(event.stage_begin + event.bytes.size())}, {"origin", std::to_string(i + 1)}}.dump());
        }
    } else {
        feed(",\"events\":[");
        for (size_t i = 0; i < m_events.size(); ++i) {
            if (m_cancel) m_cancel();
            if (i) feed(",");
            const auto& event = m_events[i];
            const auto id = std::to_string(i + 1);
            auto row = identity(event.instance);
            row.update(Json{{"eventId", id}, {"batchId", std::to_string(event.batch)}, {"begin", std::to_string(event.begin)},
                {"end", std::to_string(event.begin + event.bytes.size())}, {"nativeRole", std::to_string(event.role)},
                {"bytesHex", hexadecimal(event.bytes)}});
            if (document == Document::Audit)
                row.update(Json{{"primitive", true}, {"generator", "EXTRUDE"}, {"kind", kind_name(event.kind)},
                                {"parentOperation", std::to_string(event.parent)}});
            else row["auditEventId"] = id;
            if (document == Document::Writer)
                row.update(Json{{"writerKind", writer_name(event.writer)}, {"requestedDelta", decimal(event.delta)},
                                {"forceNoExtrusion", event.force_no_extrusion}});
            feed(row.dump());
        }
    }
    feed("]}\n");
}
} // namespace Slic3r::NativeObjectWitness
