#ifndef SLIC3R_NATIVE_PROCESSOR_PROVENANCE_HPP
#define SLIC3R_NATIVE_PROCESSOR_PROVENANCE_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Slic3r::NativeProcessorProvenance {

// Opt-in bookkeeping for the original export's processor callbacks. This is
// neither an interpreter nor a claim that a parser fragment is executable.
// Existing native gcode_id values are deliberately absent from this API.
struct Limits {
    uint64_t source_bytes = 1073741824;
    uint64_t output_bytes = 1073741824;
    uint64_t physical_lines = 10000000;
    uint64_t line_bytes = 1048576;
    uint64_t parser_records = 10000000;
    uint64_t moves = 1000000;
    uint64_t sources = 1000000;
    uint64_t spans = 1000000;       // each intermediate stream, including capture
    uint64_t links = 2000000;       // each intermediate stream
    uint64_t links_per_span = 256;  // checked before materializing a union
    uint64_t token_bytes = 1048576;
    uint64_t work = 64000000;       // build operations/links/relocated elements
    uint64_t query_work = 64000000; // fresh budget for each immutable query
    uint64_t allocation_bytes = 536870912; // retained vector capacities, not RSS
};

enum class MoveKind { Observed, SyntheticEvent, SyntheticPrevious, Dummy };
enum class Relation { Exact, Rewritten, Association, Unknown };

struct Source {
    uint64_t id;                   // one-based, assigned only to used callbacks
    uint64_t parser_record;        // one-based callback ordinal, not a line number
    uint64_t raw_begin, raw_end;   // original bytes, excludes the terminator
    uint64_t consumed_end;         // includes THIS callback's CR/LF/CRLF, if any
};
struct Move {
    uint64_t stable_id;            // zero-based birth ordinal; dummy, if present, is zero
    MoveKind kind;
    std::optional<uint64_t> source_id;
    // A synthetic visual anchor is separate from the callback that caused it.
    std::optional<uint64_t> previous_stable_move;
};
struct Binding {
    uint64_t output_begin, output_end;
    uint64_t original_begin, original_end;
    Relation relation;
};
struct SourceSummary {
    uint64_t bindings = 0;
    bool exact = false, rewritten = false, association = false, unknown = false;
    // Empty raw input is distinct from disappearance through explicit deletion.
    bool empty_raw = false;
    bool no_surviving_bytes = false; // nonempty original raw bytes, no final edges
};

class Collector {
public:
    using CheckCancel = std::function<void()>;
    using VisitBinding = std::function<void(const Binding &)>;
    using VisitAllBindings = std::function<void(uint64_t source_id, const Binding &)>;
    explicit Collector(Limits limits = {}, CheckCancel check_cancel = {});
    ~Collector();
    Collector(const Collector &) = delete;
    Collector &operator=(const Collector &) = delete;

    // The caller keeps exact_written_bytes alive AND UNCHANGED until end_buffer(). Callbacks
    // must exactly partition each buffer using the native parser's raw text and
    // CR-then-LF terminator stepping. Split writes are retained as fragments.
    void begin_buffer(std::string_view exact_written_bytes);
    void begin_parser_record(std::string_view raw_without_terminator);
    void end_parser_record();
    void end_buffer();
    uint64_t append_dummy_move(uint64_t expected_vector_index = 0);
    // Mirror the original export's native reset, which discards initialize()'s
    // dummy before any bytes are captured. Permitted exactly once, with only
    // Dummy0 present, before even an empty begin_buffer(). Does not add a move.
    void reset_initial_moves_to_empty();
    uint64_t append_move(uint64_t expected_vector_index, MoveKind kind,
                         std::optional<uint64_t> previous_stable_move = {});
    // to_index is the destination in the final vector AFTER erase-and-insert.
    // An actual Dummy0 cannot be relocated or displaced; a real first move can.
    // No matching by text or native ID.
    void relocate_move(uint64_t from_index, uint64_t to_index);
    void finish_capture(uint64_t expected_move_count);

    // Exactly two complete sequential passes. consume() supplies actual input
    // bytes, checked by a streaming digest against the previous stream. There
    // is one bounded token at a time. Offsets below are relative to that token.
    void begin_rewrite();
    void consume(uint64_t absolute_input_start, std::string_view exact_input_bytes);
    void emit_copied(uint64_t relative_start, std::string_view exact_bytes);
    void emit_rewritten(uint64_t relative_start, uint64_t source_length, std::string_view bytes);
    void emit_associated(uint64_t relative_start, uint64_t source_length, std::string_view bytes);
    // Unsupported attribution is explicit and sticky, not mistaken for deletion.
    void emit_unknown(uint64_t relative_start, uint64_t source_length, std::string_view bytes);
    void emit_unattributed(std::string_view bytes);
    // No emission for an input token means explicit deletion of those bytes.
    void end_input();
    void finish_rewrite();
    void finish(uint64_t expected_move_count);

    uint64_t move_count() const;
    uint64_t source_count() const;
    Move move_at(uint64_t final_vector_index) const;
    Source source(uint64_t source_id) const;
    uint64_t output_bytes() const;
    std::string output_sha256() const;
    uint64_t original_bytes() const;
    std::string original_sha256() const;
    // Bounded streaming access, available only after finish(). No result vector
    // or Cartesian expansion. Association/Unknown never become membership.
    SourceSummary visit_bindings(uint64_t source_id, const VisitBinding &visitor = {}) const;
    // Canonical export is one pass O(spans + links), not one scan per source.
    // The convenience per-source scan above has a fresh bounded query budget.
    void visit_all_bindings(const VisitAllBindings &visitor) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// All byte ranges are half-open. Exact ranges have equal lengths; transformed
// or associated ranges retain original references without claiming bytewise
// equivalence. Only raw source bytes receive bindings; terminators are retained
// in consumed_end and byte-stream hashes. Empty raw records can consume a newline
// but have empty_raw=true and no_surviving_bytes=false. Every exception
// from a mutating operation poisons the collector; no subsequent finish succeeds.

} // namespace Slic3r::NativeProcessorProvenance
#endif
