#ifndef SLIC3R_NATIVE_OBJECT_MAP_HPP
#define SLIC3R_NATIVE_OBJECT_MAP_HPP

#include <functional>
#include <string>

namespace Slic3r {
class Print;
namespace NativeObjectWitness { class Collector; }

namespace NativeObjectMap {

inline constexpr char sidecar_suffix[] = ".object-map.jsonl";
void write_capability(const std::string& output_path);

// Publish the actual Print/Model snapshot from the same native export. This
// reports loaded geometry, never membership of G-code operations.
void publish(const std::string& final_gcode, const Print& print, const std::function<void()>& cancel,
             const NativeObjectWitness::Collector* witness = nullptr);

} // namespace NativeObjectMap
} // namespace Slic3r
#endif
