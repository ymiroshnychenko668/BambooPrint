#include "NativeProcessorMoveMap.hpp"
#include "GCodeProcessor.hpp"
#include <utility>

namespace Slic3r::NativeProcessorMoveMap {
namespace {
// Freeze the numeric namespace in the JSONL schema. Adding/reordering a native
// enum needs an explicit wire-contract review, never a silent numeric change.
static_assert(int(EMoveType::Noop) == 0 && int(EMoveType::Retract) == 1 && int(EMoveType::Unretract) == 2 &&
              int(EMoveType::Seam) == 3 && int(EMoveType::Tool_change) == 4 && int(EMoveType::Color_change) == 5 &&
              int(EMoveType::Pause_Print) == 6 && int(EMoveType::Custom_GCode) == 7 && int(EMoveType::Travel) == 8 &&
              int(EMoveType::Wipe) == 9 && int(EMoveType::Extrude) == 10 && int(EMoveType::Count) == 11);
static_assert(int(EMovePathType::Noop_move) == 0 && int(EMovePathType::Linear_move) == 1 &&
              int(EMovePathType::Arc_move_cw) == 2 && int(EMovePathType::Arc_move_ccw) == 3 && int(EMovePathType::Count) == 4);
static_assert(int(erNone) == 0 && int(erPerimeter) == 1 && int(erExternalPerimeter) == 2 && int(erOverhangPerimeter) == 3 &&
              int(erInternalInfill) == 4 && int(erSolidInfill) == 5 && int(erFloatingVerticalShell) == 6 &&
              int(erTopSolidInfill) == 7 && int(erBottomSurface) == 8 && int(erIroning) == 9 && int(erBridgeInfill) == 10 &&
              int(erGapFill) == 11 && int(erSkirt) == 12 && int(erBrim) == 13 && int(erSupportMaterial) == 14 &&
              int(erSupportMaterialInterface) == 15 && int(erSupportTransition) == 16 && int(erSupportIroning) == 17 &&
              int(erWipeTower) == 18 && int(erCustom) == 19 && int(erFlush) == 20 && int(erMixed) == 21 && int(erCount) == 22);
static_assert(int(PrintEstimatedStatistics::ETimeMode::Normal) == 0 &&
              int(PrintEstimatedStatistics::ETimeMode::Stealth) == 1 && int(PrintEstimatedStatistics::ETimeMode::Count) == 2);
std::array<float, 3> point(const Vec3f &p) { return {p.x(), p.y(), p.z()}; }
}

void publish(const std::string &path, const GCodeProcessorResult &result,
             const NativeProcessorProvenance::Collector &provenance, CheckCancel cancel) {
    publish(path, result, provenance, Limits{}, std::move(cancel));
}
void publish(const std::string &path, const GCodeProcessorResult &result,
             const NativeProcessorProvenance::Collector &provenance, Limits limits, CheckCancel cancel) {
    detail::ResultView view{result.moves.size(), [&result](uint64_t index) {
        const auto &m = result.moves.at(size_t(index));
        detail::Observation value;
        value.type = uint8_t(m.type); value.role = uint8_t(m.extrusion_role); value.path = uint8_t(m.move_path_type);
        value.extruder_field = m.extruder_id; value.color = m.cp_color_id; value.gcode_id = m.gcode_id;
        value.object_label = m.object_label_id; value.pa_calibration = m.is_pa_line_calibration;
        value.delta_extruder = m.delta_extruder; value.feedrate = m.feedrate; value.width = m.width; value.height = m.height;
        value.mm3_per_mm = m.mm3_per_mm; value.fan = m.fan_speed; value.additional_fan = m.additional_fan_speed;
        value.temperature = m.temperature; value.layer_duration = m.layer_duration;
        value.thermal_min = m.thermal_index_min; value.thermal_max = m.thermal_index_max; value.thermal_mean = m.thermal_index_mean;
        value.print_z = m.print_z; value.time = m.time; value.position = point(m.position);
        value.arc_center = point(m.arc_center_position); value.interpolation_points = m.interpolation_points.size();
        return value;
    }, [&result](uint64_t index, uint64_t sample) {
        return point(result.moves.at(size_t(index)).interpolation_points.at(size_t(sample)));
    }};
    detail::write(path, view, provenance, limits, std::move(cancel));
}
} // namespace Slic3r::NativeProcessorMoveMap
