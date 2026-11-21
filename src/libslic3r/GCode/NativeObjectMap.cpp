#include "NativeObjectMap.hpp"
#include "NativeObjectWitness.hpp"

#include "libslic3r/Print.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#ifdef __linux__
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Slic3r::NativeObjectMap {
namespace {
#ifdef __linux__
constexpr uint64_t MAX_BYTES = 1073741824, MAX_MAP = 100663296, MAX_RECORD = 1048576;
constexpr uint64_t MAX_WITNESS = 32 * 1024 * 1024;
constexpr uint64_t MAX_GUID = 256;
constexpr uint64_t MAX_INSTANCES = 100000, MAX_VOLUMES = 1000000;
constexpr uint64_t MAX_VERTICES = 10000000, MAX_TRIANGLES = 20000000, MAX_SNAPSHOT = 20000000;
using Json = nlohmann::json;
using Cancel = std::function<void()>;

void require(bool valid, const char* error) {
    if (!valid) throw std::runtime_error(error);
}
void check(const Cancel& cancel) {
    if (cancel) cancel();
}
void charge(uint64_t& used, uint64_t count, uint64_t limit) {
    require(count <= limit - used, "NATIVE_OBJECT_MAP_LIMIT");
    used += count;
}
struct Counts {
    uint64_t instances = 0, volumes = 0, vertices = 0, triangles = 0;
};

class Digest {
public:
    Digest() {
        require(m_ctx && EVP_DigestInit_ex(m_ctx.get(), EVP_sha256(), nullptr) == 1, "NATIVE_OBJECT_MAP_HASH");
    }
    void update(const void* bytes, size_t count) {
        require(EVP_DigestUpdate(m_ctx.get(), bytes, count) == 1, "NATIVE_OBJECT_MAP_HASH");
    }
    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> buffer{};
        unsigned size = 0;
        require(EVP_DigestFinal_ex(m_ctx.get(), buffer.data(), &size) == 1 && size == 32, "NATIVE_OBJECT_MAP_HASH");
        auto ascii = [](const unsigned char n) { return "0123456789abcdef"[n]; };
        std::string digest;
        digest.reserve(size * 2);
        for (unsigned i = 0; i < size; ++i) {
            digest += ascii(buffer[i] >> 4);
            digest += ascii(buffer[i] & 15);
        }
        return digest;
    }
private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> m_ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
};

template<class UInt>
void append_le(unsigned char* out, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
        out[byte] = static_cast<unsigned char>(value & 255);
        value >>= 8;
    }
}

std::string mesh_digest(const indexed_triangle_set& mesh, Counts& counts, const Cancel& cancel) {
    static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
    const uint64_t vertices = mesh.vertices.size(), triangles = mesh.indices.size();
    require(vertices > 0 && vertices <= MAX_VERTICES && triangles > 0 && triangles <= MAX_TRIANGLES,
        "NATIVE_OBJECT_MAP_MESH_LIMIT");
    charge(counts.vertices, vertices, MAX_SNAPSHOT);
    charge(counts.triangles, triangles, MAX_SNAPSHOT);
    Digest digest;
    constexpr char prefix[] = "corvus-native-mesh/1";
    digest.update(prefix, sizeof(prefix)); // Include the NUL.
    std::array<unsigned char, 16> sizes{};
    append_le(sizes.data(), vertices);
    append_le(sizes.data() + 8, triangles);
    digest.update(sizes.data(), sizes.size());

    std::array<unsigned char, 12 * 4096> block{};
    auto elements = [&](const auto& input, const auto& component) {
        size_t used = 0;
        for (const auto& item : input) {
            for (size_t axis = 0; axis < 3; ++axis) {
                append_le(block.data() + used, component(item[axis]));
                used += 4;
            }
            if (used == block.size()) {
                check(cancel);
                digest.update(block.data(), used);
                used = 0;
            }
        }
        if (used) digest.update(block.data(), used);
    };
    elements(mesh.vertices, [](float coordinate) {
        require(std::isfinite(coordinate), "NATIVE_OBJECT_MAP_VERTEX");
        uint32_t bits;
        std::memcpy(&bits, &coordinate, sizeof(bits));
        return bits;
    });
    elements(mesh.indices, [vertices](int index) {
        require(index >= 0 && uint64_t(index) < vertices, "NATIVE_OBJECT_MAP_INDEX");
        return uint32_t(index);
    });
    return "sha256:" + digest.finish();
}

double finite(double value) {
    require(std::isfinite(value), "NATIVE_OBJECT_MAP_TRANSFORM");
    return value;
}
std::array<double, 16> matrix(const Transform3d& transform) {
    std::array<double, 16> row_major{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) row_major[row * 4 + col] = finite(transform.matrix()(row, col));
    return row_major;
}
std::array<double, 3> vector(const Vec3d& v) {
    return {finite(v.x()), finite(v.y()), finite(v.z())};
}
std::string id(uint64_t value) {
    require(value > 0 && value <= INT64_MAX, "NATIVE_OBJECT_MAP_ID");
    return std::to_string(value);
}
int64_t coordinate(coord_t value) {
    require(value > INT64_MIN, "NATIVE_OBJECT_MAP_COORDINATE");
    return value;
}
std::string volume_record(const ModelVolume& volume, Counts& counts, const Cancel& cancel) {
    check(cancel);
    charge(counts.volumes, 1, MAX_VOLUMES);
    const auto type = int(volume.type());
    require(type >= -1 && type <= 4, "NATIVE_OBJECT_MAP_VOLUME_TYPE");
    const auto& guid = volume.part_guid();
    require(guid.size() <= MAX_GUID && std::all_of(guid.begin(), guid.end(), [](unsigned char c) {
        return c >= 32 && c <= 126;
    }), "NATIVE_OBJECT_MAP_GUID");
    const auto& mesh = volume.mesh();
    return Json{{"modelVolumeId", id(volume.id().id)}, {"partGuid", guid}, {"type", type},
        {"volumeMatrix", matrix(volume.get_matrix())}, {"meshInitShift", vector(mesh.get_init_shift())},
        {"vertexCount", mesh.its.vertices.size()}, {"triangleCount", mesh.its.indices.size()},
        {"meshDigest", mesh_digest(mesh.its, counts, cancel)}}.dump();
}

struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) ::close(value); }
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
bool same_inode(const struct stat& a, const struct stat& b) {
    return S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool same_file(const struct stat& a, const struct stat& b) {
    return same_inode(a, b) && a.st_size == b.st_size && a.st_nlink == b.st_nlink
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
class Output {
public:
    explicit Output(const std::string& path, const char* suffix = sidecar_suffix, uint64_t limit = MAX_MAP) : m_limit(limit) {
        require(!path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos, "NATIVE_OBJECT_MAP_PATH");
        const std::filesystem::path name(path);
        m_gcode = name.filename().string();
        require(!m_gcode.empty() && m_gcode != "." && m_gcode != "..", "NATIVE_OBJECT_MAP_PATH");
        const auto parent = name.has_parent_path() ? name.parent_path() : std::filesystem::path(".");
        m_directory.value = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(m_directory.value >= 0, "NATIVE_OBJECT_MAP_PATH");
        if (suffix) {
            m_source.value = ::openat(m_directory.value, m_gcode.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
            require(m_source.value >= 0 && ::fstat(m_source.value, &m_source_identity) == 0 && S_ISREG(m_source_identity.st_mode)
                && m_source_identity.st_nlink == 1 && m_source_identity.st_size > 0
                && uint64_t(m_source_identity.st_size) <= MAX_BYTES, "NATIVE_OBJECT_MAP_GCODE_UNAVAILABLE");
        }
        m_final = suffix ? m_gcode + suffix : m_gcode;
        m_pending = m_final + ".pending";
        m_map.value = ::openat(m_directory.value, m_pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(m_map.value >= 0, "NATIVE_OBJECT_MAP_OUTPUT_UNAVAILABLE");
        if (::fstat(m_map.value, &m_pending_identity) != 0) {
            ::unlinkat(m_directory.value, m_pending.c_str(), 0);
            throw std::runtime_error("NATIVE_OBJECT_MAP_OUTPUT_UNAVAILABLE");
        }
    }
    ~Output() {
        remove_owned(m_pending);
        if (m_linked && !m_retained) remove_owned(m_final);
    }
    void line(std::string record) {
        require(record.size() < MAX_RECORD, "NATIVE_OBJECT_MAP_RECORD_LIMIT");
        record += '\n';
        write(record);
    }
    void write(std::string_view record) {
        charge(m_written, record.size(), m_limit);
        size_t offset = 0;
        while (offset < record.size()) {
            const auto n = ::write(m_map.value, record.data() + offset, record.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "NATIVE_OBJECT_MAP_OUTPUT_UNAVAILABLE");
            offset += size_t(n);
        }
    }
    struct Gcode { std::string sha; uint64_t bytes; };
    Gcode hash_gcode(const Cancel& cancel) {
        Digest digest;
        std::array<unsigned char, 65536> block{};
        uint64_t bytes = 0;
        for (;;) {
            check(cancel);
            const auto n = ::read(m_source.value, block.data(), block.size());
            if (n < 0 && errno == EINTR) continue;
            require(n >= 0, "NATIVE_OBJECT_MAP_GCODE_UNAVAILABLE");
            if (!n) break;
            charge(bytes, size_t(n), MAX_BYTES);
            digest.update(block.data(), size_t(n));
        }
        require(bytes == uint64_t(m_source_identity.st_size) && ::fsync(m_source.value) == 0,
            "NATIVE_OBJECT_MAP_GCODE_UNAVAILABLE");
        return {digest.finish(), bytes};
    }
    void prepare() {
        require(::fsync(m_map.value) == 0 && ::fstat(m_map.value, &m_completed) == 0
            && m_completed.st_size >= 0 && uint64_t(m_completed.st_size) == m_written && m_completed.st_nlink == 1,
            "NATIVE_OBJECT_MAP_OUTPUT_UNAVAILABLE");
        const int fd = m_map.value;
        m_map.value = -1;
        require(::close(fd) == 0, "NATIVE_OBJECT_MAP_OUTPUT_UNAVAILABLE");
        verify();
    }
    void verify() const {
        if (m_source.value < 0) return;
        struct stat gcode{}, current{};
        require(::fstat(m_source.value, &current) == 0
            && ::fstatat(m_directory.value, m_gcode.c_str(), &gcode, AT_SYMLINK_NOFOLLOW) == 0
            && same_file(m_source_identity, current) && same_file(current, gcode), "NATIVE_OBJECT_MAP_GCODE_CHANGED");
    }
    void install() {
        verify();
        struct stat named{};
        require(::fstatat(m_directory.value, m_pending.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && same_file(m_completed, named)
            && ::linkat(m_directory.value, m_pending.c_str(), m_directory.value, m_final.c_str(), 0) == 0,
            "NATIVE_OBJECT_MAP_OUTPUT_UNAVAILABLE");
        m_linked = true;
        require(::unlinkat(m_directory.value, m_pending.c_str(), 0) == 0 && ::fsync(m_directory.value) == 0,
            "NATIVE_OBJECT_MAP_OUTPUT_UNAVAILABLE");
    }
    void retain() { m_retained = true; }
private:
    void remove_owned(const std::string& name) const {
        struct stat named{};
        if (::fstatat(m_directory.value, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && same_inode(m_pending_identity, named))
            ::unlinkat(m_directory.value, name.c_str(), 0);
    }
    Fd m_directory, m_source, m_map;
    std::string m_gcode, m_final, m_pending;
    struct stat m_source_identity{}, m_pending_identity{}, m_completed{};
    uint64_t m_limit;
    uint64_t m_written = 0;
    bool m_linked = false, m_retained = false;
};
#endif
} // namespace

void write_capability(const std::string& output_path) {
#ifdef __linux__
    const Json capability{{"schema", "corvus-native-object-capability/2"}, {"mapSchema", "corvus-native-object-map/2"},
        {"basis", "PRINT_OBJECT_INSTANCE"}, {"maxInstances", std::to_string(MAX_INSTANCES)},
        {"maxVolumes", std::to_string(MAX_VOLUMES)}, {"maxGcodeBytes", std::to_string(MAX_BYTES)},
        {"maxMapBytes", std::to_string(MAX_MAP)}, {"maxRecordBytes", std::to_string(MAX_RECORD)},
        {"maxGuidBytes", std::to_string(MAX_GUID)}, {"maxVerticesPerVolume", std::to_string(MAX_VERTICES)},
        {"maxTrianglesPerVolume", std::to_string(MAX_TRIANGLES)}, {"maxVerticesPerSnapshot", std::to_string(MAX_SNAPSHOT)},
        {"maxTrianglesPerSnapshot", std::to_string(MAX_SNAPSHOT)}, {"plateOrigin", "PRINT_PLATE_ORIGIN_MM_XYZ"}};
    Output output(output_path, nullptr, 4096);
    output.line(capability.dump());
    output.prepare();
    output.install();
    output.retain();
#else
    (void)output_path;
    throw std::runtime_error("NATIVE_OBJECT_MAP_UNSUPPORTED_PLATFORM");
#endif
}

void publish(const std::string& final_gcode, const Print& print, const std::function<void()>& cancel,
             const NativeObjectWitness::Collector* witness) {
#ifdef __linux__
    require(print.get_plate_index() >= 0, "NATIVE_OBJECT_MAP_PLATE");
    Output output(final_gcode);
    output.line(Json{{"record", "header"}, {"schema", "corvus-native-object-map/2"}, {"basis", "PRINT_OBJECT_INSTANCE"},
        {"plate", uint64_t(print.get_plate_index()) + 1}, {"plateOrigin", vector(print.get_plate_origin())}}.dump());
    Counts counts;
    std::set<std::pair<size_t, size_t>> printed;
    std::vector<NativeObjectWitness::Instance> dictionary;
    for (const auto* object : print.objects()) {
        check(cancel);
        require(object && object->model_object(), "NATIVE_OBJECT_MAP_OBJECT");
        const auto* model = object->model_object();
        for (const auto& instance : object->instances()) {
            check(cancel);
            charge(counts.instances, 1, MAX_INSTANCES);
            const auto* item = instance.model_instance;
            require(item && item->get_object() == model
                && printed.emplace(object->id().id, item->id().id).second, "NATIVE_OBJECT_MAP_INSTANCE");
            if (witness) dictionary.push_back({object->id().id, item->id().id, model->id().id});
            const auto& center = object->center_offset();
            auto record = Json{{"record", "instance"}, {"printObjectId", id(object->id().id)},
                {"modelObjectId", id(model->id().id)}, {"modelInstanceId", id(item->id().id)},
                {"labelId", id(item->get_labeled_id())}, {"instanceMatrix", matrix(item->get_matrix())},
                {"printObjectMatrix", matrix(object->trafo())},
                {"shift", {coordinate(instance.shift.x()), coordinate(instance.shift.y())}},
                {"centerOffset", {std::to_string(coordinate(center.x())), std::to_string(coordinate(center.y()))}}}.dump();
            record.pop_back();
            record += ",\"volumes\":[";
            bool first = true;
            for (const auto* volume : model->volumes) {
                require(volume, "NATIVE_OBJECT_MAP_VOLUME");
                auto serialized = volume_record(*volume, counts, cancel);
                require(record.size() + serialized.size() + 3 < MAX_RECORD, "NATIVE_OBJECT_MAP_RECORD_LIMIT");
                if (!first) record += ',';
                first = false;
                record += serialized;
            }
            record += "]}";
            output.line(std::move(record));
        }
    }
    const auto gcode = output.hash_gcode(cancel);
    output.line(Json{{"record", "footer"}, {"instances", counts.instances},
        {"gcodeBytes", std::to_string(gcode.bytes)}, {"gcodeSha256", gcode.sha}}.dump());
    std::vector<std::unique_ptr<Output>> extra;
    if (witness) {
        using namespace NativeObjectWitness;
        using Document = Collector::Document;
        for (const auto& [document, suffix] : {std::pair{Document::Membership, membership_suffix},
            std::pair{Document::Audit, audit_suffix}, std::pair{Document::Operation, operation_suffix},
            std::pair{Document::Writer, writer_suffix}}) {
            if (document == Document::Writer && !witness->has_writer()) continue;
            extra.emplace_back(std::make_unique<Output>(final_gcode, suffix, MAX_WITNESS));
            auto& file = *extra.back();
            witness->write(document, dictionary, gcode.sha, gcode.bytes, [&](std::string_view bytes) { file.write(bytes); });
            file.prepare();
        }
    }
    output.prepare();
    check(cancel);
    output.install();
    for (auto& member : extra) {
        check(cancel);
        member->install();
    }
    output.verify();
    for (const auto& member : extra) member->verify();
    check(cancel);
    output.retain();
    for (auto& member : extra) member->retain();
#else
    (void)final_gcode;
    (void)print;
    (void)cancel;
    (void)witness;
    throw std::runtime_error("NATIVE_OBJECT_MAP_UNSUPPORTED_PLATFORM");
#endif
}

} // namespace Slic3r::NativeObjectMap
