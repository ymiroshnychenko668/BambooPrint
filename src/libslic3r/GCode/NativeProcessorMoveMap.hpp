#ifndef SLIC3R_NATIVE_PROCESSOR_MOVE_MAP_HPP
#define SLIC3R_NATIVE_PROCESSOR_MOVE_MAP_HPP

#include "NativeProcessorProvenance.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r {
struct GCodeProcessorResult;
namespace NativeProcessorMoveMap {

struct Limits {
    uint64_t moves = 1000000;
    uint64_t sources = 1000000;
    uint64_t bindings = 2000000;
    uint64_t arc_points = 4000000;
    uint64_t map_bytes = 268435456;
    uint64_t record_bytes = 2048; // includes LF
};
using CheckCancel = NativeProcessorProvenance::Collector::CheckCancel;

// Trusted native-export call only. Both arguments are borrowed, finalized and
// immutable for the whole call. The directory belongs to the isolated exporter;
// the caller keeps this pinned parent directory at its pathname for publication.
// Publishes <final_gcode_path>.processor-map.jsonl without replacing a file.
void publish(const std::string &final_gcode_path, const GCodeProcessorResult &result,
             const NativeProcessorProvenance::Collector &provenance, CheckCancel cancel = {});
void publish(const std::string &final_gcode_path, const GCodeProcessorResult &result,
             const NativeProcessorProvenance::Collector &provenance, Limits limits, CheckCancel cancel = {});

namespace detail {
// Internal adapter/test seam, not an ingestion API or source of native authority.
// Only one observation is copied at a time; arc vectors stay in the native result.
struct Observation {
    uint8_t type = 0, role = 0, path = 0, extruder_field = 0, color = 0;
    uint32_t gcode_id = 0;
    int32_t object_label = -1;
    bool pa_calibration = false;
    float delta_extruder = 0, feedrate = 0, width = 0, height = 0, mm3_per_mm = 0;
    float fan = 0, additional_fan = 0, temperature = 0, layer_duration = 0;
    float thermal_min = 0, thermal_max = 0, thermal_mean = 0, print_z = 0;
    std::array<float, 2> time{};
    std::array<float, 3> position{}, arc_center{};
    uint64_t interpolation_points = 0;
};
struct ResultView {
    uint64_t count;
    std::function<Observation(uint64_t)> at;
    std::function<std::array<float, 3>(uint64_t, uint64_t)> arc_point;
};
void write(const std::string &final_gcode_path, const ResultView &result,
           const NativeProcessorProvenance::Collector &provenance, Limits limits = {}, CheckCancel cancel = {});
} // namespace detail
} // namespace NativeProcessorMoveMap
} // namespace Slic3r
#endif
