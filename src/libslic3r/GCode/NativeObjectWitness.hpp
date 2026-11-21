#ifndef SLIC3R_NATIVE_OBJECT_WITNESS_HPP
#define SLIC3R_NATIVE_OBJECT_WITNESS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::NativeObjectWitness {

struct Options {
    bool witness = false;
    bool writer = false;
};
struct Instance {
    uint64_t print_object, model_instance, model_object;
    bool operator<(const Instance& other) const;
};
enum class Kind { Perimeter, Infill, Ironing };
enum class Writer { LinearXY, LinearXYZ, ArcCW, ArcCCW };

class Collector;
class Operation {
public:
    Operation() = default;
    Operation shifted(uint64_t bytes) const;
    void emitted(uint64_t begin, int role, Writer writer, double delta, bool force_no_extrusion,
                 std::string_view fragment) const;

private:
    friend class Collector;
    Collector* m_collector = nullptr;
    Instance m_instance{};
    Kind m_kind = Kind::Perimeter;
    uint64_t m_parent = 0, m_base = 0;
};

class Collector {
public:
    using Cancel = std::function<void()>;
    using Feed = std::function<void(std::string_view)>;
    explicit Collector(bool writer, Cancel cancel = {});
    enum class Document { Membership, Audit, Operation, Writer };

    Operation operation(Instance instance, Kind kind, uint64_t layer_base);
    // Consume the actual raw LayerResult before editor/cooling. Every native
    // writer return must be found at the exact append coordinates, without searching.
    void seal_layer(std::string_view exact);
    bool has_writer() const { return m_writer; }
    void write(Document document, const std::vector<Instance>& dictionary, std::string_view gcode_sha,
               uint64_t gcode_size, const Feed& feed) const;

private:
    friend class Operation;
    struct Event {
        Instance instance;
        Kind kind;
        Writer writer;
        uint64_t parent, batch, begin, stage_begin;
        int role;
        double delta;
        bool force_no_extrusion;
        std::string bytes;
    };
    void emitted(const Operation& operation, uint64_t begin, int role, Writer writer,
                 double delta, bool force_no_extrusion, std::string_view fragment);
    std::vector<Event> m_events;
    uint64_t m_batch = 0, m_parent = 0, m_stage = 0, m_bytes = 0, m_sealed = 0;
    bool m_writer;
    Cancel m_cancel;
};

inline constexpr char membership_suffix[] = ".object-membership.partial.json";
inline constexpr char audit_suffix[] = ".operation-audit.json";
inline constexpr char operation_suffix[] = ".operation-map.partial.json";
inline constexpr char writer_suffix[] = ".writer-witness.partial.json";

} // namespace Slic3r::NativeObjectWitness
#endif
