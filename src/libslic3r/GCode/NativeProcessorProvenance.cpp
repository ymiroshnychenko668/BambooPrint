#include "NativeProcessorProvenance.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include <openssl/evp.h>

namespace Slic3r::NativeProcessorProvenance {
namespace {
[[noreturn]] void fail(const char *code) { throw std::runtime_error(code); }
void require(bool valid, const char *code = "NATIVE_PROCESSOR_PROVENANCE_STATE") {
    if (!valid) fail(code);
}
constexpr size_t CHUNK = 65536;
constexpr size_t MAX_UNION = 256;

class Digest {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> m_ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
public:
    Digest() { require(m_ctx && EVP_DigestInit_ex(m_ctx.get(), EVP_sha256(), nullptr) == 1,
                       "NATIVE_PROCESSOR_PROVENANCE_HASH"); }
    void update(std::string_view bytes, const Collector::CheckCancel &cancel) {
        for (size_t pos = 0; pos < bytes.size();) {
            if (cancel) cancel();
            const size_t n = std::min(CHUNK, bytes.size() - pos);
            require(EVP_DigestUpdate(m_ctx.get(), bytes.data() + pos, n) == 1,
                    "NATIVE_PROCESSOR_PROVENANCE_HASH");
            pos += n;
        }
    }
    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
        unsigned n = 0;
        require(EVP_DigestFinal_ex(m_ctx.get(), bytes.data(), &n) == 1 && n == 32,
                "NATIVE_PROCESSOR_PROVENANCE_HASH");
        constexpr char hex[] = "0123456789abcdef";
        std::string result; result.reserve(64);
        for (unsigned i = 0; i < n; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
        return result;
    }
};

// Physical framing is independent of callback boundaries and native counters.
struct Framing {
    uint64_t lines = 0, line_bytes = 0;
    bool cr = false;
    void close(const Limits &limits) {
        require(lines < limits.physical_lines, "NATIVE_PROCESSOR_PROVENANCE_LINE_LIMIT");
        ++lines; line_bytes = 0;
    }
    void append(std::string_view bytes, const Limits &limits, const Collector::CheckCancel &cancel) {
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (i % CHUNK == 0 && cancel) cancel();
            const char ch = bytes[i];
            require(ch != '\0', "NATIVE_PROCESSOR_PROVENANCE_NUL");
            if (cr) {
                cr = false;
                if (ch == '\n') {
                    require(line_bytes < limits.line_bytes, "NATIVE_PROCESSOR_PROVENANCE_LINE_LIMIT");
                    ++line_bytes; close(limits); continue;
                }
                close(limits);
            }
            require(line_bytes < limits.line_bytes, "NATIVE_PROCESSOR_PROVENANCE_LINE_LIMIT");
            ++line_bytes;
            if (ch == '\r') cr = true;
            else if (ch == '\n') close(limits);
        }
    }
    void finish(const Limits &limits) { if (line_bytes) close(limits); cr = false; }
};
} // namespace

struct Collector::Impl {
    enum class State { Capture, Ready, Rewrite, Finished, Failed } state = State::Capture;
    struct Link { uint64_t source, begin, end; Relation relation; };
    struct Span { uint64_t begin, end, first, count; };
    struct Stream {
        uint64_t bytes = 0;
        std::string sha;
        std::vector<Span> spans;
        std::vector<Link> links;
    } input, output;

    Limits limits;
    CheckCancel cancel;
    uint64_t work = 0, allocation = 0, records = 0, read_offset = 0;
    uint64_t original_bytes = 0;
    std::string original_sha;
    unsigned passes = 0;
    std::vector<Source> sources;
    std::vector<Move> moves;
    std::vector<uint64_t> order;
    std::unique_ptr<Digest> capture_digest = std::make_unique<Digest>(), read_digest, write_digest;
    Framing capture_frame, output_frame;
    std::string_view buffer;
    uint64_t buffer_start = 0, buffer_cursor = 0;
    bool in_buffer = false, in_record = false, in_token = false, capture_started = false;
    bool initial_reset_done = false;
    Source current{};
    std::optional<uint64_t> current_source;
    std::vector<char> token;
    uint64_t token_start = 0;

    explicit Impl(Limits value, CheckCancel callback) : limits(value), cancel(std::move(callback)) {
        const Limits hard{};
        const uint64_t Limits::* fields[] = {
            &Limits::source_bytes, &Limits::output_bytes, &Limits::physical_lines, &Limits::line_bytes,
            &Limits::parser_records, &Limits::moves, &Limits::sources, &Limits::spans, &Limits::links,
            &Limits::links_per_span, &Limits::token_bytes, &Limits::work, &Limits::query_work,
            &Limits::allocation_bytes};
        for (const auto field : fields)
            require(value.*field > 0 && value.*field <= hard.*field, "NATIVE_PROCESSOR_PROVENANCE_LIMIT_CONFIG");
        require(limits.links_per_span <= MAX_UNION, "NATIVE_PROCESSOR_PROVENANCE_LIMIT_CONFIG");
    }

    void charge(uint64_t count = 1) {
        require(count <= limits.work - work, "NATIVE_PROCESSOR_PROVENANCE_WORK_LIMIT");
        work += count;
        if (cancel) cancel();
    }
    template<class F> auto mutate(F &&fn) -> decltype(fn()) {
        require(state != State::Failed, "NATIVE_PROCESSOR_PROVENANCE_FAILED");
        try { charge(); return fn(); }
        catch (...) { state = State::Failed; throw; }
    }
    template<class T> void capacity(std::vector<T> &items, uint64_t need, uint64_t max) {
        require(need <= max && need <= uint64_t(std::numeric_limits<size_t>::max()) / sizeof(T),
                "NATIVE_PROCESSOR_PROVENANCE_LIMIT");
        if (need <= items.capacity()) return;
        const uint64_t grown = std::max<uint64_t>(need, std::min<uint64_t>(max, std::max<uint64_t>(16, items.capacity() * 2)));
        const uint64_t old_capacity = items.capacity();
        // vector::reserve may temporarily retain its old allocation as well.
        const uint64_t temporary = grown * sizeof(T);
        require(temporary <= limits.allocation_bytes - allocation,
                "NATIVE_PROCESSOR_PROVENANCE_ALLOCATION_LIMIT");
        items.reserve(size_t(grown));
        // The supported allocator reserves exactly the requested capacity. Do
        // not silently undercount an implementation with different growth.
        require(items.capacity() == grown, "NATIVE_PROCESSOR_PROVENANCE_ALLOCATION_LIMIT");
        allocation += (items.capacity() - old_capacity) * sizeof(T);
    }
    template<class T> void release(std::vector<T> &items) {
        allocation -= items.capacity() * sizeof(T);
        std::vector<T>().swap(items);
    }
    void clear_stream(Stream &stream) {
        release(stream.spans); release(stream.links); stream.bytes = 0; stream.sha.clear();
    }
    void append_span(Stream &stream, uint64_t begin, uint64_t end, const Link *links, size_t count) {
        if (begin == end || count == 0) return;
        require(begin < end && (stream.spans.empty() || stream.spans.back().end <= begin));
        require(count <= limits.links_per_span && count <= limits.links - stream.links.size(),
                "NATIVE_PROCESSOR_PROVENANCE_LINK_LIMIT");
        charge(count);
        capacity(stream.spans, stream.spans.size() + 1, limits.spans);
        capacity(stream.links, stream.links.size() + count, limits.links);
        const auto first = stream.links.size();
        stream.links.insert(stream.links.end(), links, links + count);
        stream.spans.push_back({begin, end, first, count});
    }
    void check_count(uint64_t expected) const {
        require(moves.size() == order.size() && order.size() == expected &&
                (moves.empty() || moves.front().kind != MoveKind::Dummy || order.front() == 0),
                "NATIVE_PROCESSOR_PROVENANCE_MOVE_COUNT");
    }
    void check_range(uint64_t relative, uint64_t length) const {
        require(state == State::Rewrite && in_token && relative <= token.size() && length <= token.size() - relative,
                "NATIVE_PROCESSOR_PROVENANCE_RANGE");
    }
    uint64_t output_start(std::string_view bytes) {
        require(state == State::Rewrite);
        require(bytes.size() <= limits.output_bytes - output.bytes,
                "NATIVE_PROCESSOR_PROVENANCE_BYTE_LIMIT");
        const auto start = output.bytes;
        output_frame.append(bytes, limits, cancel);
        write_digest->update(bytes, cancel);
        output.bytes += bytes.size();
        return start;
    }
    static Link clipped(const Link &link, const Span &span, uint64_t begin, uint64_t end) {
        auto result = link;
        if (result.relation == Relation::Exact) {
            result.begin += begin - span.begin;
            result.end = result.begin + end - begin;
        }
        return result;
    }
    template<class F> void overlapping(uint64_t begin, uint64_t end, F &&fn) {
        if (begin == end) return;
        auto it = std::lower_bound(input.spans.begin(), input.spans.end(), begin,
            [&](const Span &span, uint64_t position) { charge(); return span.end <= position; });
        for (; it != input.spans.end() && it->begin < end; ++it) {
            charge();
            const auto a = std::max(begin, it->begin), b = std::min(end, it->end);
            fn(*it, a, b);
        }
    }
    static Relation transformed(Relation prior, Relation operation) {
        if (prior == Relation::Unknown || operation == Relation::Unknown) return Relation::Unknown;
        if (prior == Relation::Association || operation == Relation::Association) return Relation::Association;
        return Relation::Rewritten;
    }
    void emit_transform(uint64_t relative, uint64_t length, std::string_view bytes, Relation relation) {
        check_range(relative, length);
        require(length > 0, "NATIVE_PROCESSOR_PROVENANCE_RANGE");
        const uint64_t begin = token_start + relative, end = begin + length;
        std::array<Link, MAX_UNION> union_links{};
        size_t count = 0;
        overlapping(begin, end, [&](const Span &span, uint64_t a, uint64_t b) {
            for (uint64_t i = 0; i < span.count; ++i) {
                charge();
                auto link = clipped(input.links[size_t(span.first + i)], span, a, b);
                link.relation = transformed(link.relation, relation);
                // Preserve relation-distinct paths. Merge only equivalent causes
                // with overlapping/touching original intervals, with bounded work.
                for (size_t j = 0; j < count;) {
                    charge();
                    const auto &old = union_links[j];
                    if (old.source == link.source && old.relation == link.relation && old.begin <= link.end && link.begin <= old.end) {
                        link.begin = std::min(link.begin, old.begin); link.end = std::max(link.end, old.end);
                        union_links[j] = union_links[--count]; j = 0;
                    } else ++j;
                }
                require(count < limits.links_per_span, "NATIVE_PROCESSOR_PROVENANCE_UNION_LIMIT");
                union_links[count++] = link;
            }
        });
        std::sort(union_links.begin(), union_links.begin() + count, [&](const Link &a, const Link &b) {
            charge();
            if (a.source != b.source) return a.source < b.source;
            if (a.relation != b.relation) return int(a.relation) < int(b.relation);
            if (a.begin != b.begin) return a.begin < b.begin;
            return a.end < b.end;
        });
        const auto start = output_start(bytes);
        append_span(output, start, output.bytes, union_links.data(), count);
    }
    template<class F> void query(F &&visitor) const {
        require(state == State::Finished);
        uint64_t query_work = 0;
        const auto tick = [&]() {
            require(query_work < limits.query_work, "NATIVE_PROCESSOR_PROVENANCE_QUERY_LIMIT");
            if ((query_work++ % 4096) == 0 && cancel) cancel();
        };
        tick();
        for (const auto &span : input.spans) {
            tick();
            for (uint64_t j = 0; j < span.count; ++j) {
                tick();
                const auto &link = input.links[size_t(span.first + j)];
                visitor(link.source, Binding{span.begin, span.end, link.begin, link.end, link.relation});
            }
        }
    }
};

Collector::Collector(Limits limits, CheckCancel cancel) : m_impl(std::make_unique<Impl>(limits, std::move(cancel))) {}
Collector::~Collector() = default;

void Collector::begin_buffer(std::string_view bytes) {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Capture && !p.in_buffer && !p.in_record);
        require(bytes.size() <= p.limits.source_bytes - p.input.bytes, "NATIVE_PROCESSOR_PROVENANCE_BYTE_LIMIT");
        p.buffer_start = p.input.bytes; p.buffer_cursor = 0; p.buffer = bytes; p.in_buffer = true;
        p.capture_started = true;
        p.capture_frame.append(bytes, p.limits, p.cancel); p.capture_digest->update(bytes, p.cancel);
        p.input.bytes += bytes.size();
    });
}
void Collector::begin_parser_record(std::string_view raw) {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Capture && p.in_buffer && !p.in_record && p.buffer_cursor < p.buffer.size());
        require(p.records < p.limits.parser_records, "NATIVE_PROCESSOR_PROVENANCE_RECORD_LIMIT");
        const uint64_t start = p.buffer_cursor;
        require(raw.size() <= p.buffer.size() - start && raw.size() <= p.limits.line_bytes,
                "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH");
        for (size_t i = 0; i < raw.size(); i += CHUNK) {
            if (p.cancel) p.cancel();
            const auto part = raw.substr(i, std::min(CHUNK, raw.size() - i));
            require(part.find_first_of("\r\n") == std::string_view::npos &&
                    std::memcmp(part.data(), p.buffer.data() + start + i, part.size()) == 0,
                    "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH");
        }
        uint64_t end = start + raw.size();
        require(end == p.buffer.size() || p.buffer[size_t(end)] == '\r' || p.buffer[size_t(end)] == '\n',
                "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH");
        if (end < p.buffer.size() && p.buffer[size_t(end)] == '\r') ++end;
        if (end < p.buffer.size() && p.buffer[size_t(end)] == '\n') ++end;
        p.current = {0, ++p.records, p.buffer_start + start, p.buffer_start + start + raw.size(), p.buffer_start + end};
        p.current_source.reset(); p.buffer_cursor = end; p.in_record = true;
    });
}
void Collector::end_parser_record() {
    auto &p = *m_impl; p.mutate([&] { require(p.in_record && p.state == Impl::State::Capture); p.in_record = false; });
}
void Collector::end_buffer() {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Capture && p.in_buffer && !p.in_record && p.buffer_cursor == p.buffer.size(),
                "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH");
        p.buffer = {}; p.in_buffer = false;
    });
}
uint64_t Collector::append_dummy_move(uint64_t index) {
    auto &p = *m_impl; return p.mutate([&]() -> uint64_t {
        require(p.state == Impl::State::Capture && p.moves.empty() && index == 0 && !p.in_buffer);
        p.capacity(p.moves, 1, p.limits.moves); p.capacity(p.order, 1, p.limits.moves);
        p.moves.push_back({0, MoveKind::Dummy, {}, {}}); p.order.push_back(0); return 0;
    });
}
void Collector::reset_initial_moves_to_empty() {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Capture && !p.initial_reset_done && !p.capture_started && !p.in_buffer && !p.in_record &&
                p.records == 0 && p.sources.empty() && p.moves.size() == 1 && p.order.size() == 1 &&
                p.moves.front().stable_id == 0 && p.moves.front().kind == MoveKind::Dummy &&
                !p.moves.front().source_id && !p.moves.front().previous_stable_move && p.order.front() == 0);
        // Capacity remains allocated and charged. Birth numbering now mirrors
        // the empty native vector; no synthetic vertex survives the reset.
        p.moves.clear(); p.order.clear(); p.initial_reset_done = true;
    });
}
uint64_t Collector::append_move(uint64_t index, MoveKind kind, std::optional<uint64_t> previous) {
    auto &p = *m_impl; return p.mutate([&]() -> uint64_t {
        require(p.state == Impl::State::Capture && p.in_record && index == p.order.size());
        require(kind == MoveKind::Observed || kind == MoveKind::SyntheticEvent || kind == MoveKind::SyntheticPrevious);
        require((kind == MoveKind::SyntheticPrevious) == previous.has_value() && (!previous || *previous < p.moves.size()));
        p.capacity(p.moves, p.moves.size() + 1, p.limits.moves);
        p.capacity(p.order, p.order.size() + 1, p.limits.moves);
        if (!p.current_source) {
            p.capacity(p.sources, p.sources.size() + 1, p.limits.sources);
            p.current.id = p.sources.size() + 1;
            p.sources.push_back(p.current); p.current_source = p.current.id;
            const Impl::Link link{p.current.id, p.current.raw_begin, p.current.raw_end, Relation::Exact};
            p.append_span(p.input, p.current.raw_begin, p.current.raw_end, &link, 1);
        }
        const auto id = p.moves.size();
        p.moves.push_back({id, kind, p.current_source, previous}); p.order.push_back(id); return id;
    });
}
void Collector::relocate_move(uint64_t from, uint64_t to) {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Capture && from < p.order.size() && to < p.order.size() &&
                (p.moves.front().kind != MoveKind::Dummy || (from > 0 && to > 0)));
        p.charge(from > to ? from - to : to - from);
        if (from < to) std::rotate(p.order.begin() + from, p.order.begin() + from + 1, p.order.begin() + to + 1);
        else if (to < from) std::rotate(p.order.begin() + to, p.order.begin() + from, p.order.begin() + from + 1);
    });
}
void Collector::finish_capture(uint64_t expected) {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Capture && !p.in_buffer && !p.in_record); p.check_count(expected);
        p.capture_frame.finish(p.limits); p.input.sha = p.capture_digest->finish(); p.capture_digest.reset();
        p.original_bytes = p.input.bytes; p.original_sha = p.input.sha;
        p.state = Impl::State::Ready;
    });
}
void Collector::begin_rewrite() {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Ready && p.passes < 2);
        p.read_digest = std::make_unique<Digest>(); p.write_digest = std::make_unique<Digest>();
        p.read_offset = 0; p.output_frame = {}; p.state = Impl::State::Rewrite;
    });
}
void Collector::consume(uint64_t start, std::string_view bytes) {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Rewrite && !p.in_token && !bytes.empty());
        require(start == p.read_offset && bytes.size() <= p.input.bytes - p.read_offset,
                "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH");
        p.capacity(p.token, bytes.size(), p.limits.token_bytes);
        p.token.assign(bytes.begin(), bytes.end()); p.token_start = start; p.in_token = true;
        p.read_digest->update(bytes, p.cancel); p.read_offset += bytes.size();
    });
}
void Collector::emit_copied(uint64_t relative, std::string_view bytes) {
    auto &p = *m_impl; p.mutate([&] {
        p.check_range(relative, bytes.size());
        for (size_t i = 0; i < bytes.size(); i += CHUNK) {
            if (p.cancel) p.cancel();
            require(std::memcmp(bytes.data() + i, p.token.data() + relative + i, std::min(CHUNK, bytes.size() - i)) == 0,
                    "NATIVE_PROCESSOR_PROVENANCE_COPY_MISMATCH");
        }
        const auto begin = p.token_start + relative, end = begin + bytes.size();
        const auto start = p.output_start(bytes);
        p.overlapping(begin, end, [&](const Impl::Span &span, uint64_t a, uint64_t b) {
            std::array<Impl::Link, MAX_UNION> links{};
            for (uint64_t j = 0; j < span.count; ++j) {
                p.charge(); links[size_t(j)] = Impl::clipped(p.input.links[size_t(span.first + j)], span, a, b);
            }
            p.append_span(p.output, start + a - begin, start + b - begin, links.data(), size_t(span.count));
        });
    });
}
void Collector::emit_rewritten(uint64_t start, uint64_t length, std::string_view bytes) {
    auto &p = *m_impl; p.mutate([&] { p.emit_transform(start, length, bytes, Relation::Rewritten); });
}
void Collector::emit_associated(uint64_t start, uint64_t length, std::string_view bytes) {
    auto &p = *m_impl; p.mutate([&] { p.emit_transform(start, length, bytes, Relation::Association); });
}
void Collector::emit_unknown(uint64_t start, uint64_t length, std::string_view bytes) {
    auto &p = *m_impl; p.mutate([&] { p.emit_transform(start, length, bytes, Relation::Unknown); });
}
void Collector::emit_unattributed(std::string_view bytes) {
    auto &p = *m_impl; p.mutate([&] { p.output_start(bytes); });
}
void Collector::end_input() {
    auto &p = *m_impl; p.mutate([&] { require(p.state == Impl::State::Rewrite && p.in_token); p.in_token = false; p.token.clear(); });
}
void Collector::finish_rewrite() {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Rewrite && !p.in_token && p.read_offset == p.input.bytes,
                "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH");
        require(p.read_digest->finish() == p.input.sha, "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH");
        p.output_frame.finish(p.limits); p.output.sha = p.write_digest->finish();
        p.read_digest.reset(); p.write_digest.reset();
        p.clear_stream(p.input);
        std::swap(p.input, p.output); ++p.passes; p.state = Impl::State::Ready;
    });
}
void Collector::finish(uint64_t expected) {
    auto &p = *m_impl; p.mutate([&] {
        require(p.state == Impl::State::Ready && p.passes == 2); p.check_count(expected); p.state = Impl::State::Finished;
    });
}

uint64_t Collector::move_count() const { return m_impl->order.size(); }
uint64_t Collector::source_count() const { return m_impl->sources.size(); }
Move Collector::move_at(uint64_t index) const {
    require(m_impl->state != Impl::State::Failed && index < m_impl->order.size(), "NATIVE_PROCESSOR_PROVENANCE_RANGE");
    return m_impl->moves[size_t(m_impl->order[size_t(index)])];
}
Source Collector::source(uint64_t id) const {
    require(m_impl->state != Impl::State::Failed && id > 0 && id <= m_impl->sources.size(), "NATIVE_PROCESSOR_PROVENANCE_RANGE");
    return m_impl->sources[size_t(id - 1)];
}
uint64_t Collector::output_bytes() const { require(m_impl->state == Impl::State::Finished); return m_impl->input.bytes; }
std::string Collector::output_sha256() const { require(m_impl->state == Impl::State::Finished); return m_impl->input.sha; }
uint64_t Collector::original_bytes() const { require(m_impl->state == Impl::State::Finished); return m_impl->original_bytes; }
std::string Collector::original_sha256() const { require(m_impl->state == Impl::State::Finished); return m_impl->original_sha; }
SourceSummary Collector::visit_bindings(uint64_t id, const VisitBinding &visitor) const {
    const auto origin = source(id);
    SourceSummary result; result.empty_raw = origin.raw_begin == origin.raw_end;
    m_impl->query([&](uint64_t source_id, const Binding &binding) {
        if (source_id != id) return;
        ++result.bindings;
        switch (binding.relation) {
        case Relation::Exact: result.exact = true; break;
        case Relation::Rewritten: result.rewritten = true; break;
        case Relation::Association: result.association = true; break;
        case Relation::Unknown: result.unknown = true; break;
        }
        if (visitor) visitor(binding);
    });
    result.no_surviving_bytes = !result.empty_raw && result.bindings == 0;
    return result;
}
void Collector::visit_all_bindings(const VisitAllBindings &visitor) const {
    require(bool(visitor), "NATIVE_PROCESSOR_PROVENANCE_VISITOR");
    m_impl->query(visitor);
}

} // namespace Slic3r::NativeProcessorProvenance
