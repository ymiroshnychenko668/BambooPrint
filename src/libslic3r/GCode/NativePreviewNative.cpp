#include "NativePreview.hpp"
#include "GCodeProcessor.hpp"
#include "NativeProcessorProvenance.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r::NativePreview {
namespace {
static_assert(int(EMoveType::Noop) == 0 && int(EMoveType::Retract) == 1 && int(EMoveType::Unretract) == 2 &&
              int(EMoveType::Seam) == 3 && int(EMoveType::Tool_change) == 4 && int(EMoveType::Color_change) == 5 &&
              int(EMoveType::Pause_Print) == 6 && int(EMoveType::Custom_GCode) == 7 && int(EMoveType::Travel) == 8 &&
              int(EMoveType::Wipe) == 9 && int(EMoveType::Extrude) == 10 && int(EMoveType::Count) == 11);
static_assert(int(EMovePathType::Noop_move) == 0 && int(EMovePathType::Linear_move) == 1 &&
              int(EMovePathType::Arc_move_cw) == 2 && int(EMovePathType::Arc_move_ccw) == 3 && int(EMovePathType::Count) == 4);

std::array<float, 3> point(const Vec3f &p) { return {p.x(), p.y(), p.z()}; }

uint8_t kind(NativeProcessorProvenance::MoveKind value) {
    using K = NativeProcessorProvenance::MoveKind;
    switch (value) {
    case K::Observed: return 0;
    case K::SyntheticEvent: return 1;
    case K::SyntheticPrevious: return 2;
    case K::Dummy: return 3;
    }
    throw std::runtime_error("NATIVE_PREVIEW_INVALID_MOVE_KIND");
}
}

Package build_native(const GCodeProcessorResult &result,
                     const NativeProcessorProvenance::Collector &provenance,
                     const Identity &identity, Limits limits, CheckCancel cancel) {
    if (limits.moves == 0 || limits.moves > Limits{}.moves ||
        result.moves.size() != provenance.move_count() || result.moves.size() > limits.moves ||
        result.moves.size() > UINT32_MAX ||
        result.spiral_vase_layers.size() > UINT32_MAX)
        throw std::runtime_error("NATIVE_PREVIEW_INVALID_RESULT");
    // These accessors require a finished collector. Bind to its finalized byte
    // stream, not to a caller-provided source identity alone.
    if (provenance.output_bytes() != identity.source_bytes ||
        provenance.output_sha256() != identity.source_sha256)
        throw std::runtime_error("NATIVE_PREVIEW_SOURCE_MISMATCH");

    // The existing CPV exporter counts all moves except synthetic seams when
    // assigning vase ranges. The non-vase case explicitly requests inference.
    std::vector<uint32_t> layers(result.moves.size(), UINT32_MAX);
    if (!result.spiral_vase_layers.empty()) {
        size_t sequential = 0, layer = 0;
        for (size_t i = 0; i < result.moves.size(); ++i) {
            if (cancel) cancel();
            if (i != 0 && result.moves[i].type != EMoveType::Seam) ++sequential;
            while (layer + 1 < result.spiral_vase_layers.size() &&
                   sequential > result.spiral_vase_layers[layer].second.second) ++layer;
            layers[i] = uint32_t(layer);
        }
    }

    ResultView view{result.moves.size(), [&result, &provenance, &layers](uint64_t index) {
        const auto &m = result.moves.at(size_t(index));
        const auto source = provenance.move_at(index);
        Move v{};
        v.position = point(m.position); v.width = m.width; v.height = m.height; v.feedrate = m.feedrate;
        v.mm3_per_mm = m.mm3_per_mm; v.temperature = m.temperature; v.fan = m.fan_speed;
        v.layer_duration = m.layer_duration; v.time_normal = m.time[0]; v.time_stealth = m.time[1];
        v.print_z = m.print_z; v.delta_extruder = m.delta_extruder; v.gcode_id = m.gcode_id;
        v.layer_id = layers.at(size_t(index)); v.object_label = m.object_label_id;
        v.type = uint8_t(m.type); v.role = uint8_t(m.extrusion_role); v.extruder = m.extruder_id;
        v.path = uint8_t(m.move_path_type); v.pa = m.is_pa_line_calibration;
        v.point_count = m.is_arc_move_with_interpolation_points() ? m.interpolation_points.size() : 0;
        v.stable_id = source.stable_id; v.kind = kind(source.kind);
        return v;
    }, [&result](uint64_t index, uint64_t sample) {
        const auto &m = result.moves.at(size_t(index));
        if (!m.is_arc_move_with_interpolation_points())
            throw std::runtime_error("NATIVE_PREVIEW_INVALID_ARC");
        return point(m.interpolation_points.at(size_t(sample)));
    }};
    return build(view, identity, limits, std::move(cancel));
}
} // namespace Slic3r::NativePreview
