// Producer seam test: explicit native MoveVertex values, explicit collector
// relocation, and a companion independent literal-byte Python oracle.
#include "native_preview_fixture.hpp"
#include "NativePreview.hpp"
#include "NativePreviewPublish.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace V = Slic3r::NativePreview;
namespace P = Slic3r::NativeProcessorProvenance;

template<class F> static void rejects(F f, const char *message)
{
    try { f(); } catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}

static void save(const std::string &path, const std::string &bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    out.close();
    if (!out) throw std::runtime_error("fixture output failed");
}

int main(int argc, char **argv)
{
    if (argc != 2) { std::cerr << "usage: native_preview <output-prefix>\n"; return 64; }
    Slic3r::GCodeProcessorResult native;
    NativePreviewFixture::populate_result(native);
    P::Collector collector;
    NativePreviewFixture::populate_collector(collector);
    V::ResultView view{
        native.moves.size(),
        [&](uint64_t index) {
            const auto &m = native.moves.at(size_t(index));
            const auto observed = collector.move_at(index);
            V::Move out{};
            out.position = {m.position.x(), m.position.y(), m.position.z()};
            out.width = m.width; out.height = m.height; out.feedrate = m.feedrate;
            out.mm3_per_mm = m.mm3_per_mm; out.temperature = m.temperature; out.fan = m.fan_speed;
            out.layer_duration = m.layer_duration; out.time_normal = m.time[0]; out.time_stealth = m.time[1];
            out.print_z = m.print_z; out.delta_extruder = m.delta_extruder;
            out.gcode_id = m.gcode_id; out.layer_id = UINT32_MAX; out.object_label = m.object_label_id;
            out.type = uint8_t(m.type); out.role = uint8_t(m.extrusion_role);
            out.extruder = m.extruder_id; out.path = uint8_t(m.move_path_type);
            out.pa = m.is_pa_line_calibration;
            out.point_count = m.is_arc_move_with_interpolation_points() ? m.interpolation_points.size() : 0;
            out.stable_id = observed.stable_id; out.kind = uint8_t(observed.kind);
            return out;
        },
        [&](uint64_t index, uint64_t point) {
            const auto &p = native.moves.at(size_t(index)).interpolation_points.at(size_t(point));
            return std::array<float, 3>{p.x(), p.y(), p.z()};
        }
    };
    V::Identity identity{std::string(64, 'a'), collector.output_sha256(), std::string(64, 'c'), 12, 34};
    const auto package = V::build(view, identity);
    const auto native_package = V::build_native(native, collector, identity);
    if (native_package.cpv != package.cpv || native_package.sidecar != package.sidecar)
        throw std::runtime_error("native result adapter differs from fixture view");
    auto duplicates = view;
    duplicates.at = [&](uint64_t i) { auto m = view.at(i); if (i == 2) m.stable_id = 1; return m; };
    rejects([&] { V::build(duplicates, identity); }, "duplicate stable ID accepted");
    auto wrong_identity = identity; wrong_identity.source_sha256 = std::string(64, 'd');
    rejects([&] { V::build_native(native, collector, wrong_identity); }, "source mismatch accepted");
    auto too_small = V::Limits{}; too_small.cpv_bytes = 460;
    rejects([&] { V::build(view, identity, too_small); }, "CPV byte limit ignored");
    too_small = V::Limits{}; too_small.arc_points = 1;
    rejects([&] { V::build(view, identity, too_small); }, "arc point limit ignored");
    too_small = V::Limits{}; too_small.sidecar_bytes = 64;
    rejects([&] { V::build(view, identity, too_small); }, "sidecar byte limit ignored");
    rejects([&] { V::build(view, identity, {}, [] { throw std::runtime_error("cancelled"); }); }, "cancellation ignored");
    // Non-arc points are stale native state and must not become arc records.
    native.moves[1].interpolation_points.push_back(Slic3r::Vec3f(99, 98, 97));
    const auto stale = V::build_native(native, collector, identity);
    if (stale.cpv != package.cpv || stale.sidecar != package.sidecar)
        throw std::runtime_error("non-arc stale interpolation points serialized");
    save(std::string(argv[1]) + ".cpv", package.cpv);
    save(std::string(argv[1]) + ".json", package.sidecar);
    native.moves[1].interpolation_points.clear();
    const std::string path = std::string(argv[1]) + ".gcode";
    save(path, "I\nA\nP1\nP2\nB\n");
    V::publish_with_processor_map(path, native, collector, std::string(64, 'a'));
    std::cout << "native preview fixture emitted\n";
}
