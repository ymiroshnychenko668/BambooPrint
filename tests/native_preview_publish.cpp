// Exercise the real file publisher's source and map identity fence. The
// arbitrary builder is a test seam, never an authority for native mapping.
#include "NativePreviewPublish.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace V = Slic3r::NativePreview;
namespace fs = std::filesystem;
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static void write(const fs::path &p, const std::string &v) { std::ofstream f(p, std::ios::binary); f << v; f.close(); check(bool(f), "write failed"); }
static bool absent(const fs::path &p) { return !fs::exists(p.string()+".preview.cpv") && !fs::exists(p.string()+".preview-map.json"); }
template<class F> static void rejects(F f) { try { f(); } catch (const std::exception &) { return; } throw std::runtime_error("unexpected success"); }

int main(int argc, char **argv) {
    if (argc != 2) { std::cerr << "usage: native_preview_publish <fresh-output-directory>\n"; return 64; }
    fs::path dir = argv[1]; check(fs::create_directory(dir), "fresh directory required");
    fs::path src = dir / "sample.gcode"; fs::path map = src.string()+".processor-map.jsonl";
    const std::string source = "I\nA\nP1\nP2\nB\n";
    const std::string digest = "28ee975e2aaf287a9658745baa2d00818fac21f84aa6c346f4c6337a65b9bec2";
    const std::string map_digest = "9817b74faa734cd7118808908760312a62ffec8d294babe95a2ce23869daa1cb";
    const std::string execution(64, 'a');
    write(src, source); write(map, "map\n");
    auto builder = [&](const V::Identity &id) {
        check(id.source_sha256 == digest && id.source_bytes == 12, "wrong source identity");
        check(id.map_sha256 == map_digest && id.map_bytes == 4, "wrong map identity");
        check(id.execution_sha256 == execution, "wrong execution identity");
        return V::Package{"CPV", "MAP"};
    };
    rejects([&] { V::detail::publish_built(src.string(), 12, std::string(64, 'd'), execution, builder); });
    check(absent(src), "source identity failure published output");
    rejects([&] { V::detail::publish_built(src.string(), 12, digest, execution, [&](const V::Identity &id) {
        auto result = builder(id); write(map, "tampered\n"); return result;
    }); });
    check(absent(src), "map tamper published output");
    write(map, "map\n");
    rejects([&] { V::detail::publish_built(src.string(), 12, digest, execution, [&](const V::Identity &id) {
        auto result = builder(id); write(src, "T\nA\nP1\nP2\nB\n"); return result;
    }); });
    check(absent(src), "source tamper published output");
    write(src, source);
    V::detail::publish_built(src.string(), 12, digest, execution, builder);
    check(fs::file_size(src.string()+".preview.cpv") == 3 && fs::file_size(src.string()+".preview-map.json") == 3, "published files missing");
    rejects([&] { V::detail::publish_built(src.string(), 12, digest, execution, builder); });
    check(fs::file_size(src.string()+".preview.cpv") == 3 && fs::file_size(src.string()+".preview-map.json") == 3, "existing outputs changed");
    std::cout << "native preview publication identity checks: OK\n";
}
