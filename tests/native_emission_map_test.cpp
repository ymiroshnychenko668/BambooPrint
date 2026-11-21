#include "libslic3r/GCode/NativeEmissionMap.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

using namespace Slic3r::NativeEmissionMap;

namespace {
void write(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    assert(out.good());
}
std::string read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
template<class Run>
void expect_error(const char* expected, Run run) {
    try {
        run();
        assert(false);
    } catch (const std::runtime_error& error) {
        assert(error.what() == std::string(expected));
    }
}
void copy_lines(Collector& map, const std::vector<std::string>& tokens) {
    uint64_t offset = 0;
    for (const auto& token : tokens) {
        map.consume(offset, token);
        map.emit_copied(0, token);
        map.end_input();
        offset += token.size();
    }
}
} // namespace

int main(int argc, const char* argv[]) {
    assert(argc == 1 || argc == 2);
    struct Scratch {
        std::string path;
        ~Scratch() {
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        }
    } scratch;
    if (argc == 1) {
        auto path = (std::filesystem::temp_directory_path() / "bamboo-emission-XXXXXX").string();
        assert(::mkdtemp(path.data()));
        scratch.path = path;
    }
    const std::string prefix = (argc == 2 ? argv[1] : scratch.path) + std::string("/");
    uint64_t invocation = 0;
    Collector map;
    map.capture(";header\n");
    const auto first = map.open_scope(invocation, 5);
    map.capture("G1 X1\r", first);
    map.capture("\n;replace\njoin", first);
    const auto second = map.open_scope(invocation, 5);
    map.capture("ed\n", second);
    map.open_scope(invocation, 5);
    invocation = 0;
    const auto fourth = map.open_scope(invocation, 0);
    map.capture("G1 X2\rG1 X3", fourth);
    map.finish_capture();

    map.begin_rewrite();
    const std::string original = ";header\nG1 X1\r\n;replace\njoined\nG1 X2\rG1 X3";
    const std::vector<std::string> tokens{";header\n", "G1 X1\r\n", ";replace\n", "joined\n", "G1 X2\r", "G1 X3"};
    uint64_t offset = 0;
    for (size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        assert(original.substr(offset, token.size()) == token);
        map.consume(offset, token);
        if (index == 1) {
            map.emit_associated(0, 5, "M73 R1\n");
            map.emit_copied(0, "G1 X1");
            map.emit_rewritten(5, 2, "\n");
        } else if (index == 2) {
            map.emit_rewritten(0, 8, ";expanded\n;lines\n");
        } else {
            map.emit_copied(0, token);
        }
        offset += token.size();
        map.end_input();
    }
    map.finish_rewrite();
    map.begin_rewrite();
    copy_lines(map, {";header\n", "M73 R1\n", "G1 X1\n", ";expanded\n", ";lines\n", "joined\n", "G1 X2\r", "G1 X3"});
    map.finish_rewrite();
    const std::string exact = ";header\nM73 R1\nG1 X1\n;expanded\n;lines\njoined\nG1 X2\rG1 X3";
    std::vector<Span> spans;
    size_t consumed = 0;
    const auto summary = map.inspect([&](char* bytes, size_t) {
        const auto n = std::min(size_t(1), exact.size() - consumed);
        return exact.copy(bytes, n, std::exchange(consumed, consumed + n));
    }, [&](const Span& span) { spans.push_back(span); });
    assert(summary.bytes == exact.size() && summary.lines == 8 && summary.spans == 5 && spans.size() == 5);
    assert(spans[0].origin == Origin::Unattributed && spans[0].first_line == 1);
    assert(spans[1].origin == Origin::NativePostprocessor && spans[1].associated_scope == first);
    assert(spans[2].origin == Origin::Rewritten && spans[2].scope == first && spans[2].first_line == 3 && spans[2].end_line == 6);
    assert(spans[3].origin == Origin::Unattributed && spans[3].first_line == 6);
    assert(spans[4].origin == Origin::Direct && spans[4].scope == fourth && spans[4].end_line == 9);
    assert(map.scopes().size() == 4 && map.scopes()[1].local_layer == 5 && map.scopes()[3].invocation == 2);

    const auto path = prefix + "final.gcode", destination = path + sidecar_suffix, pending = destination + ".pending";
    write(path, exact);
    write_capability(prefix + "probe.json");
    expect_error("NATIVE_EMISSION_OUTPUT_UNAVAILABLE", [&] { write_capability(prefix + "probe.json"); });
    write(pending, "belongs to another producer");
    expect_error("NATIVE_EMISSION_OUTPUT_UNAVAILABLE", [&] { publish(path, map); });
    assert(read(pending) == "belongs to another producer");
    const auto other = pending + ".other";
    std::filesystem::rename(pending, other);
    write(path, "!" + exact.substr(1));
    expect_error("NATIVE_EMISSION_FINAL_MISMATCH", [&] { publish(path, map); });
    assert(!std::filesystem::exists(destination) && !std::filesystem::exists(pending));
    write(path, exact);
    expect_error("cancel", [&] { publish(path, map, [] { throw std::runtime_error("cancel"); }); });
    assert(!std::filesystem::exists(destination) && !std::filesystem::exists(pending));
    publish(path, map);
    const auto correct = read(destination);
    expect_error("NATIVE_EMISSION_OUTPUT_UNAVAILABLE", [&] { publish(path, map); });
    assert(!std::filesystem::exists(pending) && read(destination) == correct && read(path) == exact);

    Collector changed;
    changed.capture("a\n");
    changed.finish_capture();
    changed.begin_rewrite();
    copy_lines(changed, {"b\n"});
    expect_error("NATIVE_EMISSION_INPUT_MISMATCH", [&] { changed.finish_rewrite(); });
    expect_error("NATIVE_EMISSION_STATE", [&] { changed.begin_rewrite(); });
}
