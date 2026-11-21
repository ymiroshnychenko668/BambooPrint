#ifndef SLIC3R_NATIVE_PREVIEW_PUBLISH_HPP
#define SLIC3R_NATIVE_PREVIEW_PUBLISH_HPP

#include "NativePreview.hpp"

#include <string_view>

namespace Slic3r::NativePreview {
bool valid_sha256(std::string_view value);
// Emits the immutable capability declaration for the dedicated CLI probe.
void write_capability(const std::string &path);

// Trusted export hook: replaces processor-map publish and uses the same
// finished collector and final result. execution_sha256 must come from
// the authenticated execution, not from a request-controlled value.
// Publishes <path>.preview.cpv then <path>.preview-map.json, without replacement.
// These two links are not a transaction: admission must verify both identities.
// The caller keeps the isolated parent directory pinned at its pathname and
// does not mutate its source, map, result or collector during this call.
// Produces the processor map and preview from the same borrowed pair. Use only
// for authenticated preview exports.
void publish_with_processor_map(const std::string &path, const GCodeProcessorResult &result,
                                const NativeProcessorProvenance::Collector &collector,
                                const std::string &execution_sha256, Limits limits = {}, CheckCancel cancel = {});

namespace detail {
// Internal filesystem test seam. An arbitrary builder is not native authority.
using Builder = std::function<Package(const Identity &)>;
using MapProducer = std::function<void(const std::string &)>;
void publish_built(const std::string &path, uint64_t source_bytes,
                   const std::string &source_sha256, const std::string &execution_sha256,
                   const Builder &builder, Limits limits = {}, CheckCancel cancel = {});
// Filesystem test seam; the callback stands in for the native map producer.
void publish_with_map_built(const std::string &path, uint64_t source_bytes,
                            const std::string &source_sha256, const std::string &execution_sha256,
                            const MapProducer &producer, const Builder &builder,
                            Limits limits = {}, CheckCancel cancel = {});
}
}
#endif
