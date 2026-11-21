#ifndef SLIC3R_NATIVE_EMISSION_MAP_HPP
#define SLIC3R_NATIVE_EMISSION_MAP_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::NativeEmissionMap {

inline constexpr char sidecar_suffix[] = ".emission-map.jsonl";
using CheckCancel = std::function<void()>;
enum class Origin { Direct, Rewritten, NativePostprocessor, Unattributed };

struct Scope {
    uint64_t invocation;
    uint64_t local_layer;
};
struct Span {
    uint64_t first_line, end_line, byte_begin, byte_end;
    Origin origin;
    uint64_t scope, associated_scope;
};
struct Summary {
    std::string sha256;
    uint64_t bytes, lines, spans;
};

class Collector {
public:
    using Read = std::function<size_t(char*, size_t)>;
    using Visit = std::function<void(const Span&)>;

    explicit Collector(CheckCancel cancel = {});
    ~Collector();
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    // Only the serial native LayerResult sink can open a scope. Reusing a
    // local layer number does not reuse a scope. Zero denotes an ordinary write.
    uint64_t open_scope(uint64_t& invocation, uint64_t local_layer);
    void capture(std::string_view bytes, uint64_t scope = 0);
    void finish_capture();

    // Match the exact TimeProcessor writer events. Offsets are relative to
    // the active input token, except for the checked consume() file offset.
    void begin_rewrite();
    void consume(uint64_t offset, std::string_view bytes);
    void emit_copied(uint64_t relative, std::string_view bytes);
    void emit_rewritten(uint64_t relative, uint64_t length, std::string_view bytes);
    void emit_associated(uint64_t relative, uint64_t length, std::string_view bytes);
    void emit_unattributed(std::string_view bytes);
    void end_input();
    void finish_rewrite();

    const std::vector<Scope>& scopes() const;
    // Read the exact final file to verify digest and frame physical lines.
    // Membership arrives solely from the native callbacks above.
    Summary inspect(const Read& read, const Visit& visit) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

void write_capability(const std::string& path);
void publish(const std::string& final_gcode, const Collector& collector, CheckCancel cancel = {});

} // namespace Slic3r::NativeEmissionMap
#endif
