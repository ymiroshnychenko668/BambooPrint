#include "NativePreview.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <openssl/evp.h>

namespace Slic3r::NativePreview {
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool digest_format(const std::string &s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string digest(std::string_view bytes) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    unsigned char out[EVP_MAX_MD_SIZE]; unsigned n = 0;
    require(ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1 &&
            EVP_DigestUpdate(ctx.get(), bytes.data(), bytes.size()) == 1 &&
            EVP_DigestFinal_ex(ctx.get(), out, &n) == 1 && n == 32, "NATIVE_PREVIEW_HASH_FAILED");
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned i = 0; i < n; ++i) { result += hex[out[i] >> 4]; result += hex[out[i] & 15]; }
    return result;
}
void u32(std::string &out, uint32_t n) {
    for (unsigned shift = 0; shift != 32; shift += 8) out.push_back(char(uint8_t(n >> shift)));
}
void f32(std::string &out, float f) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "CPV1 requires IEEE float32");
    uint32_t n; std::memcpy(&n, &f, 4); u32(out, n);
}
void position(std::string &out, const std::array<float, 3> &p) {
    for (float f : p) { require(std::isfinite(f) && std::abs(f) <= 1e8f, "NATIVE_PREVIEW_INVALID_POSITION"); f32(out, f); }
}
void scalar(std::string &out, float f) {
    require(std::isfinite(f), "NATIVE_PREVIEW_NONFINITE"); f32(out, f);
}
const char *kind(uint8_t k) {
    switch (k) {
    case 0: return "OBSERVED";
    case 1: return "SYNTHETIC_EVENT";
    case 2: return "SYNTHETIC_PREVIOUS";
    case 3: return "DUMMY";
    default: throw std::runtime_error("NATIVE_PREVIEW_INVALID_KIND");
    }
}
void validate(const ResultView &view, const Identity &id, const Limits &l) {
    const Limits hard{};
    require(l.moves && l.moves <= hard.moves && l.arc_points && l.arc_points <= hard.arc_points &&
            l.cpv_bytes >= 32 && l.cpv_bytes <= hard.cpv_bytes && l.sidecar_bytes &&
            l.sidecar_bytes <= hard.sidecar_bytes, "NATIVE_PREVIEW_INVALID_LIMIT");
    require(view.at && view.arc_point && view.count <= l.moves && view.count <= UINT32_MAX,
            "NATIVE_PREVIEW_INVALID_RESULT");
    require(digest_format(id.execution_sha256) && digest_format(id.source_sha256) && digest_format(id.map_sha256) &&
            id.source_bytes <= 1073741824 && id.map_bytes > 0 && id.map_bytes <= 268435456,
            "NATIVE_PREVIEW_INVALID_IDENTITY");
}
}

Package build(const ResultView &view, const Identity &id, Limits limits, CheckCancel cancel) {
    validate(view, id, limits);
    std::vector<Move> moves; moves.reserve(size_t(view.count));
    std::vector<bool> seen(size_t(view.count), false);
    uint64_t points = 0;
    for (uint64_t i = 0; i < view.count; ++i) {
        if (cancel) cancel();
        Move m = view.at(i);
        require(m.type < 11 && m.role < 22 && m.path < 4 && m.stable_id < view.count &&
                !seen[size_t(m.stable_id)], "NATIVE_PREVIEW_INVALID_MOVE");
        seen[size_t(m.stable_id)] = true;
        require((m.kind == 3 && i == 0 && m.stable_id == 0 && m.type == 0) ||
                (m.kind == 2 && m.type == 3) ||
                (m.kind == 1 && (m.type == 5 || m.type == 6 || m.type == 7)) ||
                (m.kind == 0 && m.type != 3 && m.type != 5 && m.type != 6 && m.type != 7),
                "NATIVE_PREVIEW_INVALID_KIND");
        require((m.path == 2 || m.path == 3) || m.point_count == 0, "NATIVE_PREVIEW_INVALID_ARC");
        require(m.width >= 0 && m.height >= 0 && m.width <= 1e6f && m.height <= 1e6f,
                "NATIVE_PREVIEW_INVALID_DIMENSION");
        require(m.point_count <= limits.arc_points - points, "NATIVE_PREVIEW_ARC_LIMIT");
        points += m.point_count; moves.push_back(m);
    }
    require(view.count <= (limits.cpv_bytes - 32) / 81, "NATIVE_PREVIEW_BYTE_LIMIT");
    uint64_t cpv_size = 32 + 81 * view.count;
    require(points <= (limits.cpv_bytes - cpv_size) / 12 && points <= UINT32_MAX, "NATIVE_PREVIEW_BYTE_LIMIT");
    cpv_size += 12 * points;
    Package package; auto &out = package.cpv; out.reserve(size_t(cpv_size));
    out += "CPV1"; u32(out, 1); u32(out, uint32_t(view.count)); u32(out, uint32_t(points));
    u32(out, 80); u32(out, 1); u32(out, 0); u32(out, 0);
    std::string records; uint32_t arc_offset = 0;
    for (uint64_t i = 0; i < view.count; ++i) {
        if (cancel) cancel();
        const Move &m = moves[size_t(i)];
        position(out, m.position); scalar(out,m.width); scalar(out,m.height); scalar(out,m.feedrate);
        scalar(out,m.mm3_per_mm); scalar(out,m.temperature); scalar(out,m.fan); scalar(out,m.layer_duration);
        scalar(out,m.time_normal); scalar(out,m.time_stealth); scalar(out,m.print_z);
        u32(out,m.gcode_id); u32(out,m.layer_id); u32(out,arc_offset); u32(out,uint32_t(m.point_count));
        out.push_back(char(m.type)); out.push_back(char(m.role)); out.push_back(char(m.extruder)); out.push_back(char(m.path));
        scalar(out,m.delta_extruder); u32(out,uint32_t(m.object_label));
        // This witness is appended only at the actual CPV record emission.
        std::string row = (i ? "," : "") + std::string("{\"stableId\":\"") + std::to_string(m.stable_id) +
            "\",\"finalIndex\":\"" + std::to_string(i) + "\",\"ordinal\":\"" + std::to_string(i) +
            "\",\"kind\":\"" + kind(m.kind) + "\",\"primitivePotential\":\"" +
            ((i == 0 || m.type < 8) ? "SEED_OR_NON_SEGMENT" : "SEGMENT_MAY_BE_FILTERED") +
            "\",\"seed\":" + (i == 0 ? "true" : "false") +
            ",\"synthetic\":" + ((m.kind == 1 || m.kind == 2) ? "true" : "false") +
            ",\"dummy\":" + (m.kind == 3 ? "true" : "false") +
            ",\"segmentCandidate\":" + ((m.type >= 8 && i != 0) ? "true" : "false") +
            ",\"noGeometry\":" + ((m.type == 0 || (i == 0 && m.type >= 8)) ? "true" : "false") +
            ",\"arcPoints\":\"" + std::to_string(m.point_count) + "\"}";
        require(row.size() <= limits.sidecar_bytes - std::min<uint64_t>(records.size(), limits.sidecar_bytes), "NATIVE_PREVIEW_SIDECAR_LIMIT");
        records += row; arc_offset += uint32_t(m.point_count);
    }
    for (uint64_t i = 0; i < view.count; ++i) {
        for (uint64_t j = 0; j < moves[size_t(i)].point_count; ++j) {
            if (cancel) cancel();
            position(out, view.arc_point(i, j));
        }
    }
    for (const Move &m : moves) out.push_back(char(m.pa ? 3 : 1));
    require(out.size() == cpv_size, "NATIVE_PREVIEW_INTERNAL_SIZE");
    package.sidecar = "{\"schema\":\"corvus-native-preview-map/1\",\"executionSha256\":\"" + id.execution_sha256 +
        "\",\"sourceSha256\":\"" + id.source_sha256 + "\",\"sourceBytes\":\"" + std::to_string(id.source_bytes) +
        "\",\"processorMapSha256\":\"" + id.map_sha256 + "\",\"processorMapBytes\":\"" + std::to_string(id.map_bytes) +
        "\",\"cpvSha256\":\"" + digest(out) + "\",\"cpvBytes\":\"" + std::to_string(out.size()) +
        "\",\"records\":[" + records + "]}\n";
    require(package.sidecar.size() <= limits.sidecar_bytes, "NATIVE_PREVIEW_SIDECAR_LIMIT");
    return package;
}
} // namespace Slic3r::NativePreview
