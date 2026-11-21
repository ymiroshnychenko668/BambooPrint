#include "NativeEmissionMap.hpp"

#include <stdexcept>
#ifdef __linux__
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Slic3r::NativeEmissionMap {
namespace {
#ifdef __linux__
constexpr uint64_t MAX_MAP_BYTES = 100663296, MAX_RECORD_BYTES = 1024;
constexpr char CAPABILITY[] =
    R"({"schema":"corvus-native-emission-capability/1","mapSchema":"corvus-native-emission-map/1","generator":"corvusprint-layer-result/1","basis":"NATIVE_EMISSION_SCOPE","pipeline":"BY_LAYER","kind":"LAYER_BATCH","contentOrigin":"MIXED","rewritePasses":"2","maxGcodeBytes":"1073741824","maxLines":"10000000","maxScopes":"100000","maxSpans":"262144","maxLineBytes":"1048576","maxMapBytes":"100663296","maxRecordBytes":"1024"})";
constexpr char HEADER[] =
    R"({"record":"header","schema":"corvus-native-emission-map/1","basis":"NATIVE_EMISSION_SCOPE","kind":"LAYER_BATCH","pipeline":"BY_LAYER","contentOrigin":"MIXED","framing":"PHYSICAL_LINES_LF_CRLF_CR_EOF","generator":"corvusprint-layer-result/1","rewritePasses":"2"})";

void require(bool valid, const char* error) {
    if (!valid) throw std::runtime_error(error);
}
struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) ::close(fd); }
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
struct Path {
    Fd directory;
    std::string name;
    explicit Path(const std::string& path) {
        require(!path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos,
            "NATIVE_EMISSION_INVALID_PATH");
        const std::filesystem::path parsed(path);
        name = parsed.filename().string();
        require(!name.empty() && name != "." && name != "..", "NATIVE_EMISSION_INVALID_PATH");
        const auto parent = parsed.has_parent_path() ? parsed.parent_path() : std::filesystem::path(".");
        directory.fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(directory.fd >= 0, "NATIVE_EMISSION_INVALID_PATH");
    }
};

class Output {
public:
    Output(int directory, const std::string& name) :
        m_directory(directory), m_name(name), m_pending(name + ".pending") {
        m_file.fd = ::openat(m_directory, m_pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(m_file.fd >= 0, "NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
        if (::fstat(m_file.fd, &m_identity) != 0) {
            ::unlinkat(m_directory, m_pending.c_str(), 0);
            throw std::runtime_error("NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
        }
    }
    ~Output() {
        struct stat named{};
        if (::fstatat(m_directory, m_pending.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && same_inode(m_identity, named))
            ::unlinkat(m_directory, m_pending.c_str(), 0);
    }
    void line(std::string_view text) {
        require(text.size() < MAX_RECORD_BYTES, "NATIVE_EMISSION_RECORD_LIMIT");
        require(text.size() + 1 <= MAX_MAP_BYTES - m_bytes, "NATIVE_EMISSION_BYTE_LIMIT");
        for (const auto part : {text, std::string_view("\n")}) {
            size_t offset = 0;
            while (offset < part.size()) {
                const ssize_t n = ::write(m_file.fd, part.data() + offset, part.size() - offset);
                if (n < 0 && errno == EINTR) continue;
                require(n > 0, "NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
                offset += size_t(n);
            }
        }
        m_bytes += text.size() + 1;
    }
    void commit(const CheckCancel& before_publish = {}) {
        struct stat completed{}, named{};
        require(::fsync(m_file.fd) == 0 && ::fstat(m_file.fd, &completed) == 0
            && completed.st_size >= 0 && uint64_t(completed.st_size) == m_bytes && completed.st_nlink == 1,
            "NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
        const int fd = m_file.fd;
        m_file.fd = -1;
        require(::close(fd) == 0, "NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
        if (before_publish) before_publish();
        require(::fstatat(m_directory, m_pending.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && same_file(completed, named),
            "NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
        require(::linkat(m_directory, m_pending.c_str(), m_directory, m_name.c_str(), 0) == 0,
            "NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
        require(::unlinkat(m_directory, m_pending.c_str(), 0) == 0 && ::fsync(m_directory) == 0,
            "NATIVE_EMISSION_OUTPUT_UNAVAILABLE");
    }
private:
    int m_directory;
    std::string m_name, m_pending;
    Fd m_file;
    struct stat m_identity{};
    uint64_t m_bytes = 0;
};

std::string number(uint64_t value) { return "\"" + std::to_string(value) + "\""; }
std::string nullable(uint64_t id) { return id ? number(id) : "null"; }
const char* origin_name(Origin origin) {
    switch (origin) {
    case Origin::Direct: return "DIRECT";
    case Origin::Rewritten: return "REWRITTEN";
    case Origin::NativePostprocessor: return "NATIVE_POSTPROCESSOR";
    case Origin::Unattributed: return "UNATTRIBUTED";
    }
    throw std::runtime_error("NATIVE_EMISSION_ORIGIN");
}
#endif
} // namespace

void write_capability(const std::string& path) {
#ifdef __linux__
    const Path target(path);
    Output output(target.directory.fd, target.name);
    output.line(CAPABILITY);
    output.commit();
#else
    (void)path;
    throw std::runtime_error("NATIVE_EMISSION_UNSUPPORTED_PLATFORM");
#endif
}

void publish(const std::string& final_gcode, const Collector& collector, CheckCancel cancel) {
#ifdef __linux__
    const Path target(final_gcode);
    Fd source;
    source.fd = ::openat(target.directory.fd, target.name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat identity{};
    require(source.fd >= 0 && ::fstat(source.fd, &identity) == 0 && S_ISREG(identity.st_mode) && identity.st_nlink == 1,
        "NATIVE_EMISSION_FINAL_UNAVAILABLE");
    Output output(target.directory.fd, target.name + sidecar_suffix);
    output.line(HEADER);

    uint64_t count = 0;
    for (const Scope& scope : collector.scopes()) {
        if (cancel) cancel();
        const auto id = number(++count);
        output.line("{\"record\":\"scope\",\"scopeId\":" + id + ",\"emissionOrdinal\":" + id
            + ",\"pipelineInvocation\":" + number(scope.invocation) + ",\"localLayerId\":" + number(scope.local_layer) + "}");
    }
    const auto summary = collector.inspect([&](char* bytes, size_t size) {
        for (;;) {
            const ssize_t n = ::read(source.fd, bytes, size);
            if (n < 0 && errno == EINTR) continue;
            require(n >= 0, "NATIVE_EMISSION_FINAL_UNAVAILABLE");
            return size_t(n);
        }
    }, [&](const Span& span) {
        output.line("{\"record\":\"span\",\"firstLine\":" + number(span.first_line)
            + ",\"endLineExclusive\":" + number(span.end_line) + ",\"byteStart\":" + number(span.byte_begin)
            + ",\"byteEnd\":" + number(span.byte_end) + ",\"origin\":\"" + origin_name(span.origin)
            + "\",\"scopeId\":" + nullable(span.scope) + ",\"associatedScopeId\":" + nullable(span.associated_scope) + "}");
    });
    output.line("{\"record\":\"summary\",\"gcodeSha256\":\"" + summary.sha256 + "\",\"gcodeBytes\":" + number(summary.bytes)
        + ",\"lineCount\":" + number(summary.lines) + ",\"scopeCount\":" + number(count) + ",\"spanCount\":" + number(summary.spans) + "}");
    require(::fsync(source.fd) == 0, "NATIVE_EMISSION_FINAL_UNAVAILABLE");
    output.commit([&] {
        if (cancel) cancel();
        struct stat final_file{}, named{};
        require(::fstat(source.fd, &final_file) == 0
            && ::fstatat(target.directory.fd, target.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0
            && same_file(identity, final_file) && same_file(final_file, named), "NATIVE_EMISSION_FINAL_MISMATCH");
    });
#else
    (void)final_gcode;
    (void)collector;
    (void)cancel;
    throw std::runtime_error("NATIVE_EMISSION_UNSUPPORTED_PLATFORM");
#endif
}

} // namespace Slic3r::NativeEmissionMap
