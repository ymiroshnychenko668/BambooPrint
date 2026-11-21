#include "NativeProcessorMoveMap.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#ifdef __linux__
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>
#endif

namespace Slic3r::NativeProcessorMoveMap {
namespace {
[[noreturn]] void fail(const char *code) { throw std::runtime_error(code); }
void require(bool valid, const char *code) { if (!valid) fail(code); }
#ifdef __linux__
constexpr uint64_t MAX_GCODE_BYTES = 1073741824, MAX_LINES = 10000000, MAX_LINE_BYTES = 1048576;
constexpr size_t CHUNK = 65536;
std::string integer(uint64_t value) { return "\"" + std::to_string(value) + "\""; }
std::string signed_integer(int32_t value) { return "\"" + std::to_string(value) + "\""; }
std::string nullable(std::optional<uint64_t> value) { return value ? integer(*value) : "null"; }
std::string real(float value) {
    require(std::isfinite(value), "NATIVE_PROCESSOR_MAP_NONFINITE");
    if (value == 0 && std::signbit(value)) return "-0.0";
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                    std::chars_format::general, std::numeric_limits<float>::max_digits10);
    require(result.ec == std::errc{}, "NATIVE_PROCESSOR_MAP_NUMBER");
    return {buffer.data(), result.ptr};
}
std::string point(const std::array<float, 3> &p) { return "[" + real(p[0]) + "," + real(p[1]) + "," + real(p[2]) + "]"; }
const char *kind(NativeProcessorProvenance::MoveKind value) {
    using K = NativeProcessorProvenance::MoveKind;
    switch (value) {
    case K::Observed: return "OBSERVED";
    case K::SyntheticEvent: return "SYNTHETIC_EVENT";
    case K::SyntheticPrevious: return "SYNTHETIC_PREVIOUS";
    case K::Dummy: return "DUMMY";
    }
    fail("NATIVE_PROCESSOR_MAP_ENUM");
}
const char *relation(NativeProcessorProvenance::Relation value) {
    using R = NativeProcessorProvenance::Relation;
    switch (value) {
    case R::Exact: return "EXACT";
    case R::Rewritten: return "REWRITTEN";
    case R::Association: return "ASSOCIATION";
    case R::Unknown: return "UNKNOWN";
    }
    fail("NATIVE_PROCESSOR_MAP_ENUM");
}
void validate(const Limits &limits) {
    const Limits hard{};
    for (const auto field : {&Limits::moves, &Limits::sources, &Limits::bindings, &Limits::arc_points,
                            &Limits::map_bytes, &Limits::record_bytes})
        require(limits.*field > 0 && limits.*field <= hard.*field, "NATIVE_PROCESSOR_MAP_LIMIT_CONFIG");
}
bool active_arc(const detail::Observation &m) {
    require(m.type < 11 && m.role < 22 && m.path < 4, "NATIVE_PROCESSOR_MAP_ENUM");
    return m.path == 2 || m.path == 3;
}
void add_count(uint64_t &total, uint64_t value, uint64_t cap) {
    require(total <= cap && value <= cap - total, "NATIVE_PROCESSOR_MAP_COUNT_LIMIT"); total += value;
}
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
bool same(const struct stat &a, const struct stat &b) {
    return S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
class FinalFile {
public:
    Fd directory, file;
    std::string name, sha;
    struct stat identity{};
    uint64_t bytes = 0, lines = 0;
    FinalFile(const std::string &path, uint64_t expected_bytes, const std::string &expected_sha, const CheckCancel &cancel) {
        require(!path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos && expected_bytes <= MAX_GCODE_BYTES,
                "NATIVE_PROCESSOR_MAP_INVALID_PATH");
        const std::filesystem::path parsed(path);
        name = parsed.filename().string();
        require(!name.empty() && name != "." && name != "..", "NATIVE_PROCESSOR_MAP_INVALID_PATH");
        const auto parent = parsed.parent_path().empty() ? std::filesystem::path(".") : parsed.parent_path();
        directory.value = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(directory.value >= 0, "NATIVE_PROCESSOR_MAP_FINAL_UNAVAILABLE");
        file.value = ::openat(directory.value, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        require(file.value >= 0 && ::fstat(file.value, &identity) == 0 && S_ISREG(identity.st_mode) &&
                identity.st_size >= 0 && uint64_t(identity.st_size) == expected_bytes, "NATIVE_PROCESSOR_MAP_FINAL_MISMATCH");
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        require(hash && EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) == 1, "NATIVE_PROCESSOR_MAP_HASH");
        std::array<char, CHUNK> block{};
        uint64_t line_bytes = 0;
        bool cr = false;
        const auto close_line = [&] { require(lines < MAX_LINES, "NATIVE_PROCESSOR_MAP_LINE_LIMIT"); ++lines; line_bytes = 0; };
        for (;;) {
            if (cancel) cancel();
            const ssize_t read = ::read(file.value, block.data(), block.size());
            if (read < 0 && errno == EINTR) continue;
            require(read >= 0, "NATIVE_PROCESSOR_MAP_FINAL_UNAVAILABLE");
            if (!read) break;
            require(uint64_t(read) <= expected_bytes - bytes, "NATIVE_PROCESSOR_MAP_FINAL_MISMATCH");
            bytes += uint64_t(read);
            require(EVP_DigestUpdate(hash.get(), block.data(), size_t(read)) == 1, "NATIVE_PROCESSOR_MAP_HASH");
            for (ssize_t i = 0; i < read; ++i) {
                const char ch = block[size_t(i)];
                require(ch != '\0', "NATIVE_PROCESSOR_MAP_NUL");
                if (cr) {
                    cr = false;
                    if (ch == '\n') {
                        require(line_bytes < MAX_LINE_BYTES, "NATIVE_PROCESSOR_MAP_LINE_LIMIT");
                        ++line_bytes; close_line(); continue;
                    }
                    close_line();
                }
                require(line_bytes < MAX_LINE_BYTES, "NATIVE_PROCESSOR_MAP_LINE_LIMIT");
                ++line_bytes;
                if (ch == '\r') cr = true;
                else if (ch == '\n') close_line();
            }
        }
        if (line_bytes) close_line();
        require(bytes == expected_bytes, "NATIVE_PROCESSOR_MAP_FINAL_MISMATCH");
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned length = 0;
        require(EVP_DigestFinal_ex(hash.get(), digest.data(), &length) == 1 && length == 32, "NATIVE_PROCESSOR_MAP_HASH");
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < length; ++i) { sha += hex[digest[i] >> 4]; sha += hex[digest[i] & 15]; }
        require(sha == expected_sha, "NATIVE_PROCESSOR_MAP_FINAL_MISMATCH");
        require(::fsync(file.value) == 0, "NATIVE_PROCESSOR_MAP_FINAL_UNAVAILABLE");
        verify();
    }
    void verify() const {
        struct stat current{}, named{};
        require(::fstat(file.value, &current) == 0 &&
                ::fstatat(directory.value, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                same(identity, current) && same(current, named), "NATIVE_PROCESSOR_MAP_FINAL_MISMATCH");
    }
};
class Output {
    int directory;
    Fd file;
    std::string final_name, pending_name;
    Limits limits;
    struct stat owned{};
    uint64_t bytes = 0;
public:
    Output(const FinalFile &source, Limits value) : directory(source.directory.value),
        final_name(source.name + sidecar_suffix), pending_name(final_name + ".pending"), limits(value) {
        file.value = ::openat(directory, pending_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(file.value >= 0, "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE");
        if (::fstat(file.value, &owned) != 0) { ::unlinkat(directory, pending_name.c_str(), 0); fail("NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE"); }
    }
    ~Output() {
        struct stat pending{};
        if (::fstatat(directory, pending_name.c_str(), &pending, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(pending.st_mode) && pending.st_dev == owned.st_dev && pending.st_ino == owned.st_ino)
            ::unlinkat(directory, pending_name.c_str(), 0);
    }
    void line(std::string_view text) {
        require(text.size() < limits.record_bytes, "NATIVE_PROCESSOR_MAP_RECORD_LIMIT");
        const uint64_t length = text.size() + 1;
        require(length <= limits.map_bytes - bytes, "NATIVE_PROCESSOR_MAP_BYTE_LIMIT");
        for (const auto part : {text, std::string_view("\n")}) {
            size_t offset = 0;
            while (offset < part.size()) {
                const ssize_t n = ::write(file.value, part.data() + offset, part.size() - offset);
                if (n < 0 && errno == EINTR) continue;
                require(n > 0, "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE"); offset += size_t(n);
            }
        }
        bytes += length;
    }
    void commit(const FinalFile &source, const CheckCancel &cancel) {
        require(::fsync(file.value) == 0, "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE");
        struct stat completed{};
        require(::fstat(file.value, &completed) == 0 && completed.st_size >= 0 && uint64_t(completed.st_size) == bytes,
                "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE");
        const int fd = file.value; file.value = -1;
        require(::close(fd) == 0, "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE");
        if (cancel) cancel();
        source.verify();
        struct stat named{};
        require(::fstatat(directory, pending_name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && same(completed, named),
                "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE");
        require(::linkat(directory, pending_name.c_str(), directory, final_name.c_str(), 0) == 0,
                "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE");
        // A late failure can leave a complete visible map: do not remove or replace it.
        require(::unlinkat(directory, pending_name.c_str(), 0) == 0 && ::fsync(directory) == 0,
                "NATIVE_PROCESSOR_MAP_OUTPUT_UNAVAILABLE");
    }
};
class Record {
    std::string value;
    uint64_t cap;
public:
    Record(const char *type, uint64_t limit) : cap(limit) { add("{\"record\":\""); add(type); add("\""); }
    void add(std::string_view part) {
        require(value.size() < cap && part.size() < cap - value.size(), "NATIVE_PROCESSOR_MAP_RECORD_LIMIT");
        value.append(part);
    }
    void field(const char *key, std::string_view encoded) { add(",\""); add(key); add("\":"); add(encoded); }
    void emit(Output &output) { add("}"); output.line(value); }
};
#endif
} // namespace

void write_capability(const std::string &output_path)
{
#ifndef __linux__
    (void)output_path;
    fail("NATIVE_PROCESSOR_MAP_UNSUPPORTED_PLATFORM");
#else
    const auto provenance = NativeProcessorProvenance::Limits{};
    const auto map = Limits{};
    std::string payload = R"({
  "schema": "corvus-native-processor-capability/1",
  "mapSchema": "corvus-native-processor-map/1",
  "generator": "corvusprint-gcodeprocessor/1",
  "enumNamespace": "corvusprint-processor-enums/1",
  "basis": "NATIVE_PROCESSOR_OBSERVATION",
  "lineage": "ORIGINAL_PROCESSOR_CALLBACK_BYTES",
  "originalOffsetDomain": "ORIGINAL_EXPORT_BYTES",
  "outputOffsetDomain": "FINAL_GCODE_BYTES",
  "platform": "LINUX",
  "pipeline": "FRESH_BY_LAYER",
  "optInEnvironment": "CORVUS_NATIVE_PROCESSOR_MAP",
  "optInValue": "1",
  "rewritePasses": "2",
  "unsupportedFeatures": ["CACHED_EXPORT", "BY_OBJECT", "SPIRAL", "Z_SMOOTHING", "EXTERNAL_POSTPROCESS"],
  "nativeGcodeIdMeaning": "LEGACY_PROCESSOR_COUNTER",
  "coordinateBasis": "NATIVE_PREVIEW_TRANSFORMED",
  "timeBasis": "NATIVE_PREFIX_ESTIMATE",
  "timeModeOrder": ["NORMAL", "STEALTH"],
  "objectLabelMeaning": "UNVERIFIED_NATIVE_LABEL",
  "extruderFieldMeaning": "NATIVE_FILAMENT_SLOT",
  "arcSamples": "NATIVE_PREVIEW_INTERPOLATION",
  "verification": "NOT_EVALUATED",
  "sidecarSuffix": ")";
    payload += sidecar_suffix;
    payload += "\"";
    const auto field = [&](const char *name, uint64_t value) {
        payload += ",\n  \"";
        payload += name;
        payload += "\": \"";
        payload += std::to_string(value);
        payload += "\"";
    };
    field("maxOriginalBytes", provenance.source_bytes);
    field("maxFinalGcodeBytes", provenance.output_bytes);
    field("maxPhysicalLinesPerStream", provenance.physical_lines);
    field("maxLineBytes", provenance.line_bytes);
    field("maxParserRecords", provenance.parser_records);
    field("maxSources", std::min(provenance.sources, map.sources));
    field("maxMoves", std::min(provenance.moves, map.moves));
    field("maxSpansPerStream", provenance.spans);
    field("maxLinksPerStream", provenance.links);
    field("maxLinksPerSpan", provenance.links_per_span);
    field("maxTokenBytes", provenance.token_bytes);
    field("maxBuildWork", provenance.work);
    field("maxQueryWork", provenance.query_work);
    field("maxTrackedVectorAllocationBytes", provenance.allocation_bytes);
    field("maxBindings", map.bindings);
    field("maxArcPoints", map.arc_points);
    field("maxMapBytes", map.map_bytes);
    field("maxRecordBytes", map.record_bytes);
    payload += "\n}\n";

    Fd file(::open(output_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    require(file.value >= 0, "NATIVE_PROCESSOR_MAP_PROBE_OUTPUT_UNAVAILABLE");
    size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t n = ::write(file.value, payload.data() + offset, payload.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "NATIVE_PROCESSOR_MAP_PROBE_OUTPUT_UNAVAILABLE");
        offset += size_t(n);
    }
    const int fd = file.value;
    file.value = -1;
    require(::close(fd) == 0, "NATIVE_PROCESSOR_MAP_PROBE_OUTPUT_UNAVAILABLE");
#endif
}

void detail::write(const std::string &path, const ResultView &result,
                   const NativeProcessorProvenance::Collector &provenance, Limits limits, CheckCancel cancel) {
#ifndef __linux__
    (void)path; (void)result; (void)provenance; (void)limits; (void)cancel;
    fail("NATIVE_PROCESSOR_MAP_PLATFORM_UNSUPPORTED");
#else
    validate(limits);
    require(result.at && result.arc_point && result.count <= limits.moves &&
            result.count == provenance.move_count() && provenance.source_count() <= limits.sources,
            "NATIVE_PROCESSOR_MAP_INVALID_RESULT");
    const auto original_bytes = provenance.original_bytes();
    const auto original_sha = provenance.original_sha256();
    FinalFile final_file(path, provenance.output_bytes(), provenance.output_sha256(), cancel);
    Output output(final_file, limits);
    output.line("{\"record\":\"header\",\"schema\":\"corvus-native-processor-map/1\",\"generator\":\"corvusprint-gcodeprocessor/1\",\"basis\":\"NATIVE_PROCESSOR_OBSERVATION\",\"lineage\":\"ORIGINAL_PROCESSOR_CALLBACK_BYTES\",\"originalOffsetDomain\":\"ORIGINAL_EXPORT_BYTES\",\"outputOffsetDomain\":\"FINAL_GCODE_BYTES\",\"enumNamespace\":\"corvusprint-processor-enums/1\",\"coordinateBasis\":\"NATIVE_PREVIEW_TRANSFORMED\",\"timeBasis\":\"NATIVE_PREFIX_ESTIMATE\",\"timeModeOrder\":[\"NORMAL\",\"STEALTH\"],\"nativeGcodeIdMeaning\":\"LEGACY_PROCESSOR_COUNTER\",\"objectLabelMeaning\":\"UNVERIFIED_NATIVE_LABEL\",\"extruderFieldMeaning\":\"NATIVE_FILAMENT_SLOT\",\"arcSamples\":\"NATIVE_PREVIEW_INTERPOLATION\",\"rewritePasses\":\"2\",\"verification\":\"NOT_EVALUATED\"}");
    for (uint64_t id = 1; id <= provenance.source_count(); ++id) {
        if (cancel) cancel();
        const auto s = provenance.source(id);
        require(s.id == id && s.raw_begin <= s.raw_end && s.raw_end <= s.consumed_end && s.consumed_end <= original_bytes,
                "NATIVE_PROCESSOR_MAP_INVALID_SOURCE");
        Record record("source", limits.record_bytes);
        record.field("sourceId", integer(s.id)); record.field("parserRecord", integer(s.parser_record));
        record.field("rawBegin", integer(s.raw_begin)); record.field("rawEnd", integer(s.raw_end));
        record.field("consumedEnd", integer(s.consumed_end)); record.field("emptyRaw", s.raw_begin == s.raw_end ? "true" : "false");
        record.emit(output);
    }
    uint64_t active_points = 0, ignored_points = 0;
    for (uint64_t index = 0; index < result.count; ++index) {
        if (cancel) cancel();
        const auto observed = result.at(index); const auto m = provenance.move_at(index); const bool arc = active_arc(observed);
        require(m.stable_id < result.count && (!m.source_id || (*m.source_id > 0 && *m.source_id <= provenance.source_count())) &&
                (!m.previous_stable_move || *m.previous_stable_move < result.count), "NATIVE_PROCESSOR_MAP_INVALID_MOVE");
        using MoveKind = NativeProcessorProvenance::MoveKind;
        if (m.kind == MoveKind::Dummy) {
            require(index == 0 && m.stable_id == 0 && observed.type == 0 && !m.source_id && !m.previous_stable_move,
                    "NATIVE_PROCESSOR_MAP_INVALID_MOVE");
        } else {
            // Native export may reset away initialize()'s dummy. Preserve the
            // actual finalized array; its first real move may have stable ID 0.
            const auto expected_kind = observed.type == 3 ? MoveKind::SyntheticPrevious :
                (observed.type == 5 || observed.type == 6 || observed.type == 7) ? MoveKind::SyntheticEvent : MoveKind::Observed;
            require(m.kind == expected_kind && m.source_id &&
                    ((m.kind == MoveKind::SyntheticPrevious) == m.previous_stable_move.has_value()),
                    "NATIVE_PROCESSOR_MAP_INVALID_MOVE");
        }
        if (arc) add_count(active_points, observed.interpolation_points, limits.arc_points);
        else add_count(ignored_points, observed.interpolation_points, std::numeric_limits<uint64_t>::max());
        Record record("move", limits.record_bytes);
        record.field("finalIndex", integer(index)); record.field("stableId", integer(m.stable_id));
        record.field("kind", std::string("\"") + kind(m.kind) + "\""); record.field("sourceId", nullable(m.source_id));
        record.field("previousStableId", nullable(m.previous_stable_move)); record.field("nativeGcodeId", integer(observed.gcode_id));
        record.field("nativeMoveType", integer(observed.type)); record.field("nativeExtrusionRole", integer(observed.role));
        record.field("nativePathType", integer(observed.path)); record.field("nativeExtruderField", integer(observed.extruder_field));
        record.field("nativeColorId", integer(observed.color)); record.field("nativeObjectLabelId", signed_integer(observed.object_label));
        record.field("nativePositionMm", point(observed.position)); record.field("nativeArcCenterMm", arc ? point(observed.arc_center) : "null");
        record.field("deltaExtruderMm", real(observed.delta_extruder)); record.field("feedrateMmPerSecond", real(observed.feedrate));
        record.field("widthMm", real(observed.width)); record.field("heightMm", real(observed.height));
        record.field("mm3PerMm", real(observed.mm3_per_mm)); record.field("fanPercent", real(observed.fan));
        record.field("additionalFanPercent", real(observed.additional_fan)); record.field("temperatureC", real(observed.temperature));
        record.field("nativeLayerDurationSeconds", real(observed.layer_duration));
        record.field("nativeElapsedSeconds", "[" + real(observed.time[0]) + "," + real(observed.time[1]) + "]");
        record.field("nativeThermalIndex", "[" + real(observed.thermal_min) + "," + real(observed.thermal_max) + "," + real(observed.thermal_mean) + "]");
        record.field("nativePrintZMm", real(observed.print_z)); record.field("paLineCalibration", observed.pa_calibration ? "true" : "false");
        record.field("activeArcPointCount", integer(arc ? observed.interpolation_points : 0));
        record.field("ignoredInterpolationPointCount", integer(arc ? 0 : observed.interpolation_points)); record.emit(output);
    }
    uint64_t bindings = 0;
    provenance.visit_all_bindings([&](uint64_t source_id, const NativeProcessorProvenance::Binding &b) {
        if (cancel) cancel();
        add_count(bindings, 1, limits.bindings);
        const auto source = provenance.source(source_id);
        require(b.output_begin < b.output_end && b.output_end <= final_file.bytes && b.original_begin < b.original_end &&
                b.original_begin >= source.raw_begin && b.original_end <= source.raw_end,
                "NATIVE_PROCESSOR_MAP_INVALID_BINDING");
        require(b.relation != NativeProcessorProvenance::Relation::Exact ||
                b.output_end - b.output_begin == b.original_end - b.original_begin, "NATIVE_PROCESSOR_MAP_INVALID_BINDING");
        Record record("binding", limits.record_bytes);
        record.field("bindingId", integer(bindings)); record.field("sourceId", integer(source_id));
        record.field("finalBegin", integer(b.output_begin)); record.field("finalEnd", integer(b.output_end));
        record.field("originalBegin", integer(b.original_begin)); record.field("originalEnd", integer(b.original_end));
        record.field("relation", std::string("\"") + relation(b.relation) + "\""); record.emit(output);
    });
    uint64_t emitted_points = 0;
    for (uint64_t index = 0; index < result.count; ++index) {
        if (cancel) cancel();
        const auto m = result.at(index);
        if (!active_arc(m)) continue;
        const auto stable_id = provenance.move_at(index).stable_id;
        for (uint64_t point_index = 0; point_index < m.interpolation_points; ++point_index) {
            if (cancel) cancel();
            add_count(emitted_points, 1, limits.arc_points);
            Record record("arcPoint", limits.record_bytes);
            record.field("stableId", integer(stable_id)); record.field("pointIndex", integer(point_index));
            record.field("nativePositionMm", point(result.arc_point(index, point_index))); record.emit(output);
        }
    }
    require(emitted_points == active_points, "NATIVE_PROCESSOR_MAP_INVALID_RESULT");
    Record summary("summary", limits.record_bytes);
    summary.field("originalSha256", "\"" + original_sha + "\""); summary.field("originalBytes", integer(original_bytes));
    summary.field("gcodeSha256", "\"" + final_file.sha + "\""); summary.field("gcodeBytes", integer(final_file.bytes));
    summary.field("physicalLineCount", integer(final_file.lines)); summary.field("sourceCount", integer(provenance.source_count()));
    summary.field("moveCount", integer(result.count)); summary.field("bindingCount", integer(bindings));
    summary.field("activeArcPointCount", integer(active_points)); summary.field("ignoredInterpolationPointCount", integer(ignored_points));
    summary.emit(output); output.commit(final_file, cancel);
#endif
}
} // namespace Slic3r::NativeProcessorMoveMap
