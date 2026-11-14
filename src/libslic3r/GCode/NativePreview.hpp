#ifndef SLIC3R_NATIVE_PREVIEW_HPP
#define SLIC3R_NATIVE_PREVIEW_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r {
struct GCodeProcessorResult;
namespace NativeProcessorProvenance { class Collector; }
namespace NativePreview {

// Identity values are supplied by the trusted export/retention invocation.
// The publishing adapter verifies source and map hashes against actual files;
// the execution hash must be provided by the authenticated orchestrator.
struct Identity {
    std::string execution_sha256, source_sha256, map_sha256;
    uint64_t source_bytes = 0, map_bytes = 0;
};
struct Limits {
    uint64_t moves = 1000000, arc_points = 4000000;
    uint64_t cpv_bytes = 134217728, sidecar_bytes = 268435456;
};
using CheckCancel = std::function<void()>;
struct Move {
    std::array<float, 3> position{};
    float width = 0, height = 0, feedrate = 0, mm3_per_mm = 0, temperature = 0, fan = 0;
    float layer_duration = 0, time_normal = 0, time_stealth = 0, print_z = 0, delta_extruder = 0;
    uint32_t gcode_id = 0, layer_id = UINT32_MAX;
    int32_t object_label = -1;
    uint8_t type = 0, role = 0, extruder = 0, path = 0;
    bool pa = false;
    uint64_t point_count = 0, stable_id = 0;
    // 0 observed; 1 synthetic event; 2 synthetic previous; 3 dummy.
    uint8_t kind = 0;
};
struct ResultView {
    uint64_t count = 0;
    std::function<Move(uint64_t)> at;
    std::function<std::array<float, 3>(uint64_t, uint64_t)> arc_point;
};
struct Package { std::string cpv, sidecar; };

// Test seam only: callers must not treat an arbitrary view as native authority.
Package build(const ResultView &, const Identity &, Limits = {}, CheckCancel = {});
// Both arguments must be the same finalized pair used by processor-map publish.
Package build_native(const GCodeProcessorResult &, const NativeProcessorProvenance::Collector &,
                     const Identity &, Limits = {}, CheckCancel = {});
} // namespace NativePreview
} // namespace Slic3r
#endif
