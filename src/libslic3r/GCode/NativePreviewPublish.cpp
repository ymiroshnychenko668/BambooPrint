#include "NativePreviewPublish.hpp"
#include "NativeProcessorProvenance.hpp"
#include "NativeProcessorMoveMap.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#ifdef __linux__
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>
#include <openssl/evp.h>
#endif

namespace Slic3r::NativePreview {
bool valid_sha256(std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

namespace {
[[noreturn]] void fail(const char *code) { throw std::runtime_error(code); }
void require(bool b, const char *code) { if (!b) fail(code); }
#ifdef __linux__
constexpr uint64_t MAX_SOURCE = 1073741824, MAX_MAP = 268435456;
struct Fd {
    int fd = -1;
    explicit Fd(int value = -1) : fd(value) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
bool same(const struct stat &a, const struct stat &b) {
    return S_ISREG(a.st_mode) && S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
        a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
class Input {
    int dir;
    std::string name;
    Fd file;
    struct stat initial{};
public:
    uint64_t size = 0;
    std::string sha;
    Input(int d, std::string n, uint64_t cap, const CheckCancel &cancel) : dir(d), name(std::move(n)) {
        file.fd = ::openat(dir, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        require(file.fd >= 0 && ::fstat(file.fd, &initial) == 0 && S_ISREG(initial.st_mode) &&
                initial.st_size >= 0 && uint64_t(initial.st_size) <= cap, "NATIVE_PREVIEW_INPUT_UNAVAILABLE");
        size = uint64_t(initial.st_size); sha = hash(cancel);
    }
    void verify(const CheckCancel &cancel) const {
        verify_stat();
        require(hash(cancel) == sha, "NATIVE_PREVIEW_INPUT_CHANGED");
        verify_stat();
    }
    void verify_alias(int other_dir, const std::string &other_name) const {
        struct stat named{};
        require(::fstatat(other_dir, other_name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                same(initial, named), "NATIVE_PREVIEW_INPUT_CHANGED");
    }
    // linkat changes ctime. Preserve and rehash the already-open producer fd;
    // only the link-induced metadata change may be adopted.
    void accept_new_link(int other_dir, const std::string &other_name, const CheckCancel &cancel) {
        struct stat opened{}, private_name{}, public_name{};
        require(::fstat(file.fd, &opened) == 0 && ::fstatat(dir, name.c_str(), &private_name, AT_SYMLINK_NOFOLLOW) == 0 &&
                ::fstatat(other_dir, other_name.c_str(), &public_name, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(opened.st_mode) && opened.st_dev == initial.st_dev && opened.st_ino == initial.st_ino &&
                opened.st_size == initial.st_size && opened.st_mtim.tv_sec == initial.st_mtim.tv_sec &&
                opened.st_mtim.tv_nsec == initial.st_mtim.tv_nsec && same(opened, private_name) && same(opened, public_name),
                "NATIVE_PREVIEW_INPUT_CHANGED");
        initial = opened;
        require(hash(cancel) == sha, "NATIVE_PREVIEW_INPUT_CHANGED");
        verify_alias(other_dir, other_name);
    }
private:
    void verify_stat() const {
        struct stat opened{}, named{};
        require(::fstat(file.fd, &opened) == 0 && ::fstatat(dir, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                same(initial, opened) && same(initial, named), "NATIVE_PREVIEW_INPUT_CHANGED");
    }
    std::string hash(const CheckCancel &cancel) const {
        verify_stat();
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        require(ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1, "NATIVE_PREVIEW_HASH_FAILED");
        std::array<char, 65536> buffer{};
        uint64_t offset = 0;
        while (offset < size) {
            if (cancel) cancel();
            const size_t amount = size_t(std::min<uint64_t>(buffer.size(), size - offset));
            const ssize_t n = ::pread(file.fd, buffer.data(), amount, off_t(offset));
            if (n < 0 && errno == EINTR) continue;
            require(n > 0 && uint64_t(n) <= size - offset, "NATIVE_PREVIEW_INPUT_CHANGED");
            require(EVP_DigestUpdate(ctx.get(), buffer.data(), size_t(n)) == 1, "NATIVE_PREVIEW_HASH_FAILED");
            offset += uint64_t(n);
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned length = 0;
        require(EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) == 1 && length == 32, "NATIVE_PREVIEW_HASH_FAILED");
        std::string out; out.reserve(64); constexpr char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < length; ++i) { out += hex[digest[i] >> 4]; out += hex[digest[i] & 15]; }
        verify_stat(); return out;
    }
};
class PrivateDir {
    int parent;
    std::string dirname;
public:
    Fd fd;
    PrivateDir(int parent_fd) : parent(parent_fd) {
        std::array<unsigned char, 16> random{};
        size_t offset = 0;
        while (offset < random.size()) {
            const ssize_t n = ::getrandom(random.data() + offset, random.size() - offset, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "NATIVE_PREVIEW_PRIVATE_UNAVAILABLE"); offset += size_t(n);
        }
        constexpr char hex[] = "0123456789abcdef";
        dirname = ".native-preview-";
        for (auto b : random) { dirname += hex[b >> 4]; dirname += hex[b & 15]; }
        require(::mkdirat(parent, dirname.c_str(), 0700) == 0, "NATIVE_PREVIEW_PRIVATE_UNAVAILABLE");
        fd.fd = ::openat(parent, dirname.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd.fd < 0) { ::unlinkat(parent, dirname.c_str(), AT_REMOVEDIR); fail("NATIVE_PREVIEW_PRIVATE_UNAVAILABLE"); }
        struct stat st{};
        if (::fstat(fd.fd, &st) != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 0777) != 0700) {
            ::unlinkat(parent, dirname.c_str(), AT_REMOVEDIR); fail("NATIVE_PREVIEW_PRIVATE_UNAVAILABLE");
        }
    }
    ~PrivateDir() {
        if (fd.fd >= 0) {
            ::unlinkat(fd.fd, "source.gcode.processor-map.jsonl.pending", 0);
            ::unlinkat(fd.fd, "source.gcode.processor-map.jsonl", 0);
            ::unlinkat(fd.fd, "source.gcode", 0);
        }
        ::unlinkat(parent, dirname.c_str(), AT_REMOVEDIR);
    }
    const std::string &name() const { return dirname; }
};
class Output {
    int dir;
    std::string name, pending;
    Fd file;
    struct stat owned{};
    bool linked = false, keep = false;
public:
    Output(int d, std::string n) : dir(d), name(std::move(n)), pending(name + ".pending") {
        file.fd = ::openat(dir, pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(file.fd >= 0, "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
        if (::fstat(file.fd, &owned) != 0 || !S_ISREG(owned.st_mode)) {
            ::unlinkat(dir, pending.c_str(), 0); fail("NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
        }
    }
    ~Output() { remove_if_owned(pending); if (linked && !keep) remove_if_owned(name); }
    void write(const std::string &bytes, uint64_t cap, const CheckCancel &cancel) {
        require(bytes.size() <= cap, "NATIVE_PREVIEW_OUTPUT_LIMIT");
        size_t offset = 0;
        while (offset < bytes.size()) {
            if (cancel) cancel();
            const size_t amount = std::min<size_t>(65536, bytes.size() - offset);
            const ssize_t n = ::write(file.fd, bytes.data() + offset, amount);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE"); offset += size_t(n);
        }
        require(::fsync(file.fd) == 0 && ::fstat(file.fd, &owned) == 0 &&
                S_ISREG(owned.st_mode) && owned.st_size >= 0 && uint64_t(owned.st_size) == bytes.size(),
                "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
    }
    void install() {
        struct stat now{}, named{};
        require(::fstat(file.fd, &now) == 0 && ::fstatat(dir, pending.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                same(owned, now) && same(owned, named), "NATIVE_PREVIEW_OUTPUT_CHANGED");
        require(::linkat(dir, pending.c_str(), dir, name.c_str(), 0) == 0, "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
        linked = true;
    }
    void retain() { keep = true; }
private:
    void remove_if_owned(const std::string &n) noexcept {
        struct stat named{};
        if (::fstatat(dir, n.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(named.st_mode) &&
            named.st_dev == owned.st_dev && named.st_ino == owned.st_ino) ::unlinkat(dir, n.c_str(), 0);
    }
};
void validate_request(const std::string &path, uint64_t source_bytes, const std::string &source_sha256,
                      const std::string &execution_sha256, const detail::Builder &builder, const Limits &limits) {
    const Limits hard{};
    require(!path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos &&
            valid_sha256(source_sha256) && valid_sha256(execution_sha256) && source_bytes <= MAX_SOURCE && builder &&
            limits.moves && limits.moves <= hard.moves && limits.arc_points && limits.arc_points <= hard.arc_points &&
            limits.cpv_bytes && limits.cpv_bytes <= hard.cpv_bytes && limits.sidecar_bytes && limits.sidecar_bytes <= hard.sidecar_bytes,
            "NATIVE_PREVIEW_INVALID_ARGUMENT");
}
void emit_package(int dir, const std::string &name, Input &source, Input &map,
                  const std::string &execution_sha256, const detail::Builder &builder,
                  Limits limits, const CheckCancel &cancel) {
    const std::string public_map = name + ".processor-map.jsonl";
    map.verify_alias(dir, public_map);
    Identity identity{execution_sha256, source.sha, map.sha, source.size, map.size};
    Package package = builder(identity);
    require(package.cpv.size() <= limits.cpv_bytes && package.sidecar.size() <= limits.sidecar_bytes &&
            !package.cpv.empty() && !package.sidecar.empty(), "NATIVE_PREVIEW_OUTPUT_LIMIT");
    Output cpv(dir, name + ".preview.cpv"), sidecar(dir, name + ".preview-map.json");
    cpv.write(package.cpv, limits.cpv_bytes, cancel);
    sidecar.write(package.sidecar, limits.sidecar_bytes, cancel);
    source.verify(cancel); map.verify(cancel); map.verify_alias(dir, public_map);
    if (cancel) cancel();
    cpv.install();
    source.verify(cancel); map.verify(cancel); map.verify_alias(dir, public_map);
    sidecar.install();
    cpv.retain(); sidecar.retain();
    require(::fsync(dir) == 0, "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
}
#endif
}

void write_capability(const std::string &path)
{
#ifndef __linux__
    (void)path;
    fail("NATIVE_PREVIEW_PLATFORM_UNSUPPORTED");
#else
    require(!path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos,
            "NATIVE_PREVIEW_INVALID_PATH");
    const std::filesystem::path parsed(path);
    const std::string name = parsed.filename().string();
    require(!name.empty() && name != "." && name != "..", "NATIVE_PREVIEW_INVALID_PATH");
    const auto parent = parsed.parent_path().empty() ? std::filesystem::path(".") : parsed.parent_path();
    Fd dir(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    require(dir.fd >= 0, "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
    const std::string capability =
        R"({"schema":"corvus-native-preview-capability/1","mapSchema":"corvus-native-preview-map/1",)"
        R"("cpvSchema":"CPV1/1","basis":"FINAL_PROCESSOR_RESULT_AND_COLLECTOR","relation":"ONE_RECORD_PER_FINAL_MOVE",)"
        R"("optInEnvironment":"CORVUS_NATIVE_PREVIEW_MAP","optInValue":"1",)"
        R"("executionIdentityEnvironment":"CORVUS_NATIVE_EXECUTION_SHA256","cpvSuffix":".preview.cpv",)"
        R"("mapSuffix":".preview-map.json","verification":"NOT_EVALUATED"})" "\n";
    Output output(dir.fd, name);
    output.write(capability, 4096, {});
    output.install();
    require(::fsync(dir.fd) == 0, "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
    output.retain();
#endif
}

void detail::publish_built(const std::string &path, uint64_t source_bytes, const std::string &source_sha256,
                           const std::string &execution_sha256, const Builder &builder, Limits limits, CheckCancel cancel) {
#ifndef __linux__
    (void)path; (void)source_bytes; (void)source_sha256; (void)execution_sha256; (void)builder; (void)limits; (void)cancel;
    fail("NATIVE_PREVIEW_PLATFORM_UNSUPPORTED");
#else
    validate_request(path, source_bytes, source_sha256, execution_sha256, builder, limits);
    const std::filesystem::path parsed(path);
    const std::string name = parsed.filename().string();
    require(!name.empty() && name != "." && name != "..", "NATIVE_PREVIEW_INVALID_PATH");
    const auto parent = parsed.parent_path().empty() ? std::filesystem::path(".") : parsed.parent_path();
    Fd dir(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    require(dir.fd >= 0, "NATIVE_PREVIEW_INPUT_UNAVAILABLE");
    Input source(dir.fd, name, MAX_SOURCE, cancel);
    require(source.size == source_bytes && source.sha == source_sha256, "NATIVE_PREVIEW_SOURCE_MISMATCH");
    Input map(dir.fd, name + ".processor-map.jsonl", MAX_MAP, cancel);
    emit_package(dir.fd, name, source, map, execution_sha256, builder, limits, cancel);
#endif
}
void detail::publish_with_map_built(const std::string &path, uint64_t source_bytes,
                                    const std::string &source_sha256, const std::string &execution_sha256,
                                    const MapProducer &producer, const Builder &builder, Limits limits, CheckCancel cancel) {
#ifndef __linux__
    (void)path; (void)source_bytes; (void)source_sha256; (void)execution_sha256; (void)producer; (void)builder; (void)limits; (void)cancel;
    fail("NATIVE_PREVIEW_PLATFORM_UNSUPPORTED");
#else
    validate_request(path, source_bytes, source_sha256, execution_sha256, builder, limits);
    require(bool(producer), "NATIVE_PREVIEW_INVALID_ARGUMENT");
    const std::filesystem::path parsed(path);
    const std::string name = parsed.filename().string();
    require(!name.empty() && name != "." && name != "..", "NATIVE_PREVIEW_INVALID_PATH");
    const auto parent = parsed.parent_path().empty() ? std::filesystem::path(".") : parsed.parent_path();
    Fd dir(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    require(dir.fd >= 0, "NATIVE_PREVIEW_INPUT_UNAVAILABLE");
    Fd original(::openat(dir.fd, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    struct stat before{}, after{};
    require(original.fd >= 0 && ::fstat(original.fd, &before) == 0 && S_ISREG(before.st_mode) &&
            before.st_size >= 0 && uint64_t(before.st_size) == source_bytes, "NATIVE_PREVIEW_SOURCE_MISMATCH");
    PrivateDir staging(dir.fd);
    require(::linkat(dir.fd, name.c_str(), staging.fd.fd, "source.gcode", 0) == 0 &&
            ::fstatat(staging.fd.fd, "source.gcode", &after, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(after.st_mode) && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
            before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            before.st_mtim.tv_nsec == after.st_mtim.tv_nsec, "NATIVE_PREVIEW_SOURCE_MISMATCH");
    Input source(dir.fd, name, MAX_SOURCE, cancel);
    require(source.size == source_bytes && source.sha == source_sha256, "NATIVE_PREVIEW_SOURCE_MISMATCH");
    source.verify_alias(staging.fd.fd, "source.gcode");
    const auto private_path = parent / staging.name() / "source.gcode";
    producer(private_path.string());
    source.verify(cancel); source.verify_alias(staging.fd.fd, "source.gcode");
    Input map(staging.fd.fd, "source.gcode.processor-map.jsonl", MAX_MAP, cancel);
    require(::linkat(staging.fd.fd, "source.gcode.processor-map.jsonl", dir.fd,
                     (name + ".processor-map.jsonl").c_str(), 0) == 0, "NATIVE_PREVIEW_OUTPUT_UNAVAILABLE");
    map.accept_new_link(dir.fd, name + ".processor-map.jsonl", cancel);
    emit_package(dir.fd, name, source, map, execution_sha256, builder, limits, cancel);
#endif
}
void publish_with_processor_map(const std::string &path, const GCodeProcessorResult &result,
                                const NativeProcessorProvenance::Collector &collector,
                                const std::string &execution_sha256, Limits limits, CheckCancel cancel) {
    detail::publish_with_map_built(path, collector.output_bytes(), collector.output_sha256(), execution_sha256,
        [&](const std::string &private_path) { NativeProcessorMoveMap::publish(private_path, result, collector, cancel); },
        [&](const Identity &identity) { return build_native(result, collector, identity, limits, cancel); }, limits, cancel);
}
} // namespace Slic3r::NativePreview
