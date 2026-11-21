#include "NativeEmissionMap.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <openssl/evp.h>

namespace Slic3r::NativeEmissionMap {
namespace {
constexpr uint64_t MAX_BYTES = 1073741824;
constexpr uint64_t MAX_LINES = 10000000;
constexpr uint64_t MAX_LINE_BYTES = 1048576;
constexpr uint64_t MAX_SCOPES = 100000;
constexpr uint64_t MAX_SPANS = 262144;

void require(bool valid, const char* error) {
    if (!valid) throw std::runtime_error(error);
}
class Digest {
public:
    Digest() {
        require(m_ctx && EVP_DigestInit_ex(m_ctx.get(), EVP_sha256(), nullptr) == 1, "NATIVE_EMISSION_HASH");
    }
    void update(std::string_view bytes) {
        require(bytes.empty() || EVP_DigestUpdate(m_ctx.get(), bytes.data(), bytes.size()) == 1, "NATIVE_EMISSION_HASH");
    }
    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
        unsigned size = 0;
        require(EVP_DigestFinal_ex(m_ctx.get(), hash.data(), &size) == 1 && size == 32, "NATIVE_EMISSION_HASH");
        const char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(size * 2);
        for (unsigned i = 0; i < size; ++i) {
            result += hex[hash[i] >> 4];
            result += hex[hash[i] & 15];
        }
        return result;
    }
private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> m_ctx{EVP_MD_CTX_new(), EVP_MD_CTX_free};
};
struct Membership {
    Origin origin = Origin::Unattributed;
    uint64_t scope = 0, associated = 0;
    bool operator==(const Membership& o) const { return origin == o.origin && scope == o.scope && associated == o.associated; }
};
struct Range {
    uint64_t end;
    Membership membership;
};
Membership combine(Membership left, Membership right) {
    if (left == right) return left;
    if (left.scope && left.scope == right.scope) return {Origin::Rewritten, left.scope, 0};
    if (left.origin == Origin::NativePostprocessor && right.origin == Origin::NativePostprocessor)
        return {Origin::NativePostprocessor, 0, 0};
    return {};
}

struct Stream {
    uint64_t bytes = 0;
    std::vector<Range> ranges;
    std::string sha;
    std::optional<Digest> digest{std::in_place};

    void append(std::string_view incoming, Membership member) {
        if (incoming.empty()) return;
        require(incoming.size() <= MAX_BYTES - bytes, "NATIVE_EMISSION_BYTE_LIMIT");
        require(incoming.find('\0') == std::string_view::npos, "NATIVE_EMISSION_NUL");
        const bool extend = !ranges.empty() && ranges.back().membership == member;
        require(extend || ranges.size() < MAX_SPANS, "NATIVE_EMISSION_SPAN_LIMIT");
        digest->update(incoming);
        bytes += incoming.size();
        if (extend) ranges.back().end = bytes;
        else ranges.push_back({bytes, member});
    }
    void finish() {
        sha = digest->finish();
        digest.reset();
    }
    template<class Visitor>
    void visit(uint64_t begin, uint64_t end, Visitor visit_range) const {
        auto cursor = std::upper_bound(ranges.begin(), ranges.end(), begin,
            [](uint64_t offset, const Range& range) { return offset < range.end; });
        while (begin < end) {
            require(cursor != ranges.end(), "NATIVE_EMISSION_LINEAGE");
            const uint64_t stop = std::min(end, cursor->end);
            visit_range(cursor->membership, stop - begin);
            begin = stop;
            ++cursor;
        }
    }
    Membership membership(uint64_t begin, uint64_t end) const {
        std::optional<Membership> result;
        visit(begin, end, [&](Membership current, uint64_t) { result = result ? combine(*result, current) : current; });
        return result.value_or(Membership{});
    }
};
} // namespace

struct Collector::Impl {
    explicit Impl(CheckCancel check) : cancel(std::move(check)) {}
    enum class State { Capture, BetweenPasses, Rewrite, Finished, Failed };
    State state = State::Capture;
    struct Step {
        Impl& impl;
        int exceptions = std::uncaught_exceptions();
        ~Step() { if (std::uncaught_exceptions() > exceptions) impl.state = State::Failed; }
    };
    CheckCancel cancel;
    std::vector<Scope> scopes;
    uint64_t invocation = 0, offset = 0, token_begin = 0;
    unsigned passes = 0;
    bool has_token = false;
    std::string token;
    Stream previous, output;
    std::optional<Digest> input_digest;

    void check(State expected) const {
        require(state == expected, "NATIVE_EMISSION_STATE");
        if (cancel) cancel();
    }
    void range(uint64_t relative, uint64_t length) const {
        require(state == State::Rewrite && has_token && relative <= token.size() && length <= token.size() - relative,
            "NATIVE_EMISSION_INPUT_RANGE");
    }
};

Collector::Collector(CheckCancel cancel) : m_impl(std::make_unique<Impl>(std::move(cancel))) {}
Collector::~Collector() = default;

uint64_t Collector::open_scope(uint64_t& invocation, uint64_t local_layer) {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.check(Impl::State::Capture);
    require(m.scopes.size() < MAX_SCOPES && local_layer <= INT64_MAX, "NATIVE_EMISSION_SCOPE_LIMIT");
    if (!invocation) invocation = ++m.invocation;
    require(invocation == m.invocation, "NATIVE_EMISSION_INVOCATION");
    m.scopes.push_back({invocation, local_layer});
    return m.scopes.size();
}
void Collector::capture(std::string_view bytes, uint64_t scope) {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.check(Impl::State::Capture);
    require(scope <= m.scopes.size(), "NATIVE_EMISSION_SCOPE");
    m.output.append(bytes, scope ? Membership{Origin::Direct, scope, 0} : Membership{});
}
void Collector::finish_capture() {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.check(Impl::State::Capture);
    m.output.finish();
    m.state = Impl::State::BetweenPasses;
}
void Collector::begin_rewrite() {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.check(Impl::State::BetweenPasses);
    m.previous = std::move(m.output);
    m.output = Stream{};
    m.input_digest.emplace();
    m.offset = 0;
    m.state = Impl::State::Rewrite;
}
void Collector::consume(uint64_t offset, std::string_view bytes) {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.check(Impl::State::Rewrite);
    require(!m.has_token && offset == m.offset && bytes.size() <= MAX_LINE_BYTES &&
        bytes.size() <= m.previous.bytes - m.offset, "NATIVE_EMISSION_INPUT_RANGE");
    m.token.assign(bytes);
    m.token_begin = m.offset;
    m.offset += bytes.size();
    m.input_digest->update(bytes);
    m.has_token = true;
}
void Collector::emit_copied(uint64_t relative, std::string_view bytes) {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.range(relative, bytes.size());
    require(std::string_view(m.token).substr(relative, bytes.size()) == bytes, "NATIVE_EMISSION_INPUT_MISMATCH");
    uint64_t used = 0;
    m.previous.visit(m.token_begin + relative, m.token_begin + relative + bytes.size(),
        [&](Membership member, uint64_t length) { m.output.append(bytes.substr(used, length), member); used += length; });
}
void Collector::emit_rewritten(uint64_t relative, uint64_t length, std::string_view bytes) {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.range(relative, length);
    Membership member = m.previous.membership(m.token_begin + relative, m.token_begin + relative + length);
    if (member.scope) member.origin = Origin::Rewritten;
    m.output.append(bytes, member);
}
void Collector::emit_associated(uint64_t relative, uint64_t length, std::string_view bytes) {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.range(relative, length);
    const Membership input = m.previous.membership(m.token_begin + relative, m.token_begin + relative + length);
    m.output.append(bytes, {Origin::NativePostprocessor, 0, input.scope});
}
void Collector::emit_unattributed(std::string_view bytes) {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.range(0, 0);
    m.output.append(bytes, {});
}
void Collector::end_input() {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.range(0, 0);
    m.has_token = false;
}
void Collector::finish_rewrite() {
    auto& m = *m_impl;
    Impl::Step step{m};
    m.check(Impl::State::Rewrite);
    require(!m.has_token && m.offset == m.previous.bytes && m.input_digest->finish() == m.previous.sha,
        "NATIVE_EMISSION_INPUT_MISMATCH");
    m.output.finish();
    m.state = ++m.passes == 2 ? Impl::State::Finished : Impl::State::BetweenPasses;
}
const std::vector<Scope>& Collector::scopes() const {
    m_impl->check(Impl::State::Finished);
    return m_impl->scopes;
}
Summary Collector::inspect(const Read& read, const Visit& visit) const {
    const auto& m = *m_impl;
    m.check(Impl::State::Finished);
    Digest digest;
    std::array<char, 65536> buffer{};
    uint64_t bytes = 0, lines = 0, spans = 0, line_begin = 0;
    bool pending_cr = false;
    std::optional<Span> active;
    auto close_line = [&](uint64_t end) {
        require(end > line_begin && end - line_begin <= MAX_LINE_BYTES && lines < MAX_LINES, "NATIVE_EMISSION_LINE_LIMIT");
        const Membership member = m.output.membership(line_begin, end);
        ++lines;
        if (active && Membership{active->origin, active->scope, active->associated_scope} == member) {
            active->end_line = lines + 1;
            active->byte_end = end;
        } else {
            if (active) visit(*active);
            require(++spans <= MAX_SPANS, "NATIVE_EMISSION_SPAN_LIMIT");
            active = Span{lines, lines + 1, line_begin, end, member.origin, member.scope, member.associated};
        }
        line_begin = end;
    };
    for (;;) {
        if (m.cancel) m.cancel();
        const auto count = read(buffer.data(), buffer.size());
        require(count <= buffer.size() && count <= m.output.bytes - bytes, "NATIVE_EMISSION_FINAL_MISMATCH");
        if (!count) break;
        digest.update({buffer.data(), count});
        for (size_t i = 0; i < count; ++i) {
            const char byte = buffer[i];
            const uint64_t offset = bytes + i;
            if (pending_cr) {
                pending_cr = false;
                if (byte == '\n') {
                    close_line(offset + 1);
                    continue;
                }
                close_line(offset);
            }
            if (byte == '\r') pending_cr = true;
            else if (byte == '\n') close_line(offset + 1);
            require(offset + 1 - line_begin <= MAX_LINE_BYTES, "NATIVE_EMISSION_LINE_LIMIT");
        }
        bytes += count;
    }
    if (line_begin < bytes) close_line(bytes);
    auto sha = digest.finish();
    require(bytes == m.output.bytes && sha == m.output.sha, "NATIVE_EMISSION_FINAL_MISMATCH");
    if (active) visit(*active);
    return {std::move(sha), bytes, lines, spans};
}

} // namespace Slic3r::NativeEmissionMap
