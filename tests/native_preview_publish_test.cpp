#include "../src/libslic3r/GCode/NativePreviewPublish.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <unistd.h>
using namespace Slic3r::NativePreview;
namespace fs = std::filesystem;
constexpr const char *abc_sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr const char *map_sha = "60be9861750facbfad8758254a2f76c0cfe78d54459a3bc187d49b1401fcd8e8";
const std::string exec_sha(64, 'a');
void put(const fs::path &p, const std::string &s) { std::ofstream out(p, std::ios::binary); out << s; assert(out.good()); }
std::string get(const fs::path &p) { std::ifstream in(p, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
void throws(const std::function<void()> &f) { bool failed = false; try { f(); } catch (const std::exception &) { failed = true; } assert(failed); }
int main() {
    char tmp[] = "/dev/shm/native-preview-publish-XXXXXX";
    const char *created = ::mkdtemp(tmp); assert(created);
    fs::path dir(created), path = dir / "print.gcode";
    auto reset = [&] { fs::remove_all(dir); fs::create_directory(dir); put(path, "abc"); put(path.string() + ".processor-map.jsonl", "map"); };
    auto builder = [&](const Identity &id) {
        assert(id.source_sha256 == abc_sha && id.source_bytes == 3 && id.map_sha256 == map_sha && id.map_bytes == 3 && id.execution_sha256 == exec_sha);
        return Package{std::string("CPV\0x", 5), "sidecar"};
    };
    auto publish = [&] { detail::publish_built(path.string(), 3, abc_sha, exec_sha, builder); };
    reset(); publish();
    assert(get(path.string() + ".preview.cpv") == std::string("CPV\0x", 5));
    assert(get(path.string() + ".preview-map.json") == "sidecar");
    assert(!fs::exists(path.string() + ".preview.cpv.pending"));
    throws(publish); assert(get(path.string() + ".preview.cpv") == std::string("CPV\0x", 5));
    reset(); put(path.string() + ".preview-map.json", "existing"); throws(publish);
    assert(!fs::exists(path.string() + ".preview.cpv") && get(path.string() + ".preview-map.json") == "existing");
    assert(!fs::exists(path.string() + ".preview-map.json.pending"));
    reset(); fs::remove(path.string() + ".processor-map.jsonl"); fs::create_symlink(path, path.string() + ".processor-map.jsonl"); throws(publish);
    reset(); throws([&] { detail::publish_built(path.string(), 3, abc_sha, exec_sha, [&](const Identity &id) { auto p = builder(id); put(path.string() + ".processor-map.jsonl", "mop"); return p; }); });
    assert(!fs::exists(path.string() + ".preview.cpv") && !fs::exists(path.string() + ".preview-map.json"));
    reset(); throws([&] { detail::publish_built(path.string(), 3, std::string(64, '0'), exec_sha, builder); });
    throws([&] { detail::publish_built(path.string(), 3, abc_sha, "bad", builder); });
    reset(); Limits limits; limits.cpv_bytes = 4; throws([&] { detail::publish_built(path.string(), 3, abc_sha, exec_sha, builder, limits); });
    reset(); int checks = 0; throws([&] { detail::publish_built(path.string(), 3, abc_sha, exec_sha, builder, {}, [&] { if (++checks == 3) throw std::runtime_error("cancel"); }); });
    assert(!fs::exists(path.string() + ".preview.cpv") && !fs::exists(path.string() + ".preview.cpv.pending"));
    reset(); throws([&] { detail::publish_built(path.string(), 3, abc_sha, exec_sha, builder, {}, [&] {
        if (fs::exists(path.string() + ".preview.cpv")) throw std::runtime_error("cancel after first link");
    }); });
    assert(!fs::exists(path.string() + ".preview.cpv") && !fs::exists(path.string() + ".preview-map.json"));
    reset(); fs::create_symlink(path, path.string() + ".preview.cpv.pending"); throws(publish);
    assert(fs::is_symlink(path.string() + ".preview.cpv.pending") && get(path) == "abc");
    auto reset_producer = [&] { reset(); fs::remove(path.string() + ".processor-map.jsonl"); };
    auto producer = [&](const std::string &private_path) {
        assert(get(private_path) == "abc");
        put(private_path + ".processor-map.jsonl", "map");
    };
    auto run_private = [&](const detail::Builder &b, CheckCancel cancel = {}) {
        detail::publish_with_map_built(path.string(), 3, abc_sha, exec_sha, producer, b, {}, cancel);
    };
    reset_producer(); run_private(builder);
    assert(get(path.string() + ".processor-map.jsonl") == "map");
    assert(get(path.string() + ".preview-map.json") == "sidecar");
    for (const auto &entry : fs::directory_iterator(dir)) assert(entry.path().filename().string().find(".native-preview-") != 0);
    reset_producer(); throws([&] { run_private([&](const Identity &id) {
        auto result = builder(id);
        fs::remove(path.string() + ".processor-map.jsonl");
        put(path.string() + ".processor-map.jsonl", "map"); // same bytes, foreign inode
        return result;
    }); });
    assert(!fs::exists(path.string() + ".preview.cpv") && !fs::exists(path.string() + ".preview-map.json"));
    reset_producer(); throws([&] { run_private([&](const Identity &id) {
        auto result = builder(id);
        put(path.string() + ".processor-map.jsonl", "bad"); // mutate the linked inode
        return result;
    }); });
    assert(!fs::exists(path.string() + ".preview.cpv") && !fs::exists(path.string() + ".preview-map.json"));
    reset_producer(); throws([&] { run_private(builder, [&] {
        const auto public_map = path.string() + ".processor-map.jsonl";
        if (fs::exists(public_map)) { fs::remove(public_map); put(public_map, "bad"); }
    }); });
    assert(!fs::exists(path.string() + ".preview.cpv") && !fs::exists(path.string() + ".preview-map.json"));
    reset(); throws([&] { run_private(builder); });
    assert(get(path.string() + ".processor-map.jsonl") == "map");
    fs::remove_all(dir);
}
