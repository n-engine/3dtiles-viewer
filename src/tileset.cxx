// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "tileset.h"
#include "tile_loader.h"

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace v3dt
{

using json = nlohmann::json;

namespace
{

// Bit-pattern finiteness check (exponent all-ones => Inf/NaN). Used instead of
// std::isfinite because the latter can be folded to a constant under
// -ffinite-math-only / -ffast-math; this integer test cannot. The cast from the
// JSON double to float happens first, so a finite-but-huge double that overflows
// to +/-Inf is caught here too. Non-finite values in transforms / bounding
// volumes would poison frustum culling (NaN compares false) and SSE selection.
bool finite_f(float v)
{
    std::uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

// 3D Tiles transform is a column-major 4x4 written as 16 doubles.
glm::mat4 parse_transform(const json& j, const glm::mat4& parent)
{
    if (!j.is_array() || j.size() != 16)
        return parent;
    glm::mat4 m(1.0f);
    for (int i = 0; i < 16; ++i)
    {
        const float v = static_cast<float>(j[i].get<double>());
        if (!finite_f(v))
            return parent;  // malformed transform: ignore it, keep the parent's
        m[i / 4][i % 4] = v;
    }
    glm::mat4 r = parent * m;
    // The product can overflow to non-finite even when both inputs are finite;
    // a non-finite node transform would propagate to every descendant's geometry.
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            if (!finite_f(r[c][row]))
                return parent;
    return r;
}

// Approximate max axis scale of the upper-3x3 of a transform. Used to scale
// local-space bounding sphere radii into world space.
float max_axis_scale(const glm::mat4& t)
{
    glm::vec3 cx(t[0][0], t[0][1], t[0][2]);
    glm::vec3 cy(t[1][0], t[1][1], t[1][2]);
    glm::vec3 cz(t[2][0], t[2][1], t[2][2]);
    return std::max({glm::length(cx), glm::length(cy), glm::length(cz)});
}

// box: [cx, cy, cz, hxx, hxy, hxz, hyx, hyy, hyz, hzx, hzy, hzz] (12 doubles).
// The three half-axes describe an oriented box. We over-approximate it with
// the smallest enclosing sphere assuming orthogonal axes (the common case for
// 3D Tiles): r = sqrt(|hx|^2 + |hy|^2 + |hz|^2).
bool parse_box(const json& j, const glm::mat4& world_t, BoundingSphere& out)
{
    if (!j.is_array() || j.size() != 12)
        return false;
    float v[12];
    for (int i = 0; i < 12; ++i)
    {
        v[i] = static_cast<float>(j[i].get<double>());
        if (!finite_f(v[i]))
            return false;  // non-finite box: reject so the caller falls back
    }
    glm::vec3 c(v[0], v[1], v[2]);
    glm::vec3 hx(v[3], v[4], v[5]);
    glm::vec3 hy(v[6], v[7], v[8]);
    glm::vec3 hz(v[9], v[10], v[11]);
    float     local_r = std::sqrt(glm::dot(hx, hx) + glm::dot(hy, hy) + glm::dot(hz, hz));
    out.center        = glm::vec3(world_t * glm::vec4(c, 1.0f));
    out.radius        = local_r * max_axis_scale(world_t);
    return true;
}

// sphere: [cx, cy, cz, r] (4 doubles).
bool parse_sphere(const json& j, const glm::mat4& world_t, BoundingSphere& out)
{
    if (!j.is_array() || j.size() != 4)
        return false;
    float v[4];
    for (int i = 0; i < 4; ++i)
    {
        v[i] = static_cast<float>(j[i].get<double>());
        if (!finite_f(v[i]))
            return false;  // non-finite sphere: reject so the caller falls back
    }
    glm::vec3 c(v[0], v[1], v[2]);
    float     r = v[3];
    out.center  = glm::vec3(world_t * glm::vec4(c, 1.0f));
    out.radius  = r * max_axis_scale(world_t);
    return true;
}

// region: [west, south, east, north, min_h, max_h]. Proper conversion needs
// WGS84 -> ECEF math; for v0.2 min we fall back to a generous sphere centered
// at the node origin, sized off the geometric error.
void fallback_bv_from_geom_error(const glm::mat4& world_t, float geom_error, BoundingSphere& out)
{
    out.center = glm::vec3(world_t * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    if (!finite_f(out.center.x) || !finite_f(out.center.y) || !finite_f(out.center.z))
        out.center = glm::vec3(0.0f);
    // 4x geometric error is a heuristic: large enough that LOD selection still
    // descends into refined children, small enough that frustum culling can do
    // something useful. A finite-but-huge geometricError (near FLT_MAX) would
    // overflow the multiply to +Inf, so cap the radius well below the float max.
    constexpr float kMaxRadius = 1.0e30f;
    float           r          = geom_error * 4.0f;
    if (!finite_f(r) || r > kMaxRadius)
        r = kMaxRadius;
    out.radius = std::max(r, 1.0f);
}

// A parsed-then-transformed sphere can still come out non-finite (scale
// overflow) or with a negative radius; such a volume would corrupt culling, so
// treat it as unusable and let the caller fall back.
bool bv_usable(const BoundingSphere& s)
{
    return finite_f(s.center.x) && finite_f(s.center.y) && finite_f(s.center.z) &&
           finite_f(s.radius) && s.radius >= 0.0f;
}

// Even a finite bounding volume can be absurdly large (a hostile radius near
// FLT_MAX), and downstream arithmetic multiplies it -- e.g. camera far plane =
// radius * 50, or rebase/rotate of the center in apply_world_anchor -- which then
// overflows to +Inf and poisons projection / culling / SSE. Clamp every bounding
// volume to a generous-but-safe extent so those products stay finite. The cap is
// ~12 orders of magnitude above any real planetary-scale ECEF value, so it never
// touches legitimate data; it only tames hostile inputs. A non-finite component
// (NaN) is snapped to a safe value too.
constexpr float kMaxBvExtent = 1.0e18f;

float clamp_extent(float v)
{
    if (!finite_f(v))
        return 0.0f;
    return std::min(std::max(v, -kMaxBvExtent), kMaxBvExtent);
}

void clamp_bv(BoundingSphere& s)
{
    s.center.x = clamp_extent(s.center.x);
    s.center.y = clamp_extent(s.center.y);
    s.center.z = clamp_extent(s.center.z);
    s.radius = finite_f(s.radius) ? std::min(std::max(s.radius, 0.0f), kMaxBvExtent) : kMaxBvExtent;
}

BoundingSphere parse_bv(const json& node, const glm::mat4& world_t, float geom_error)
{
    BoundingSphere bv{};
    if (node.contains("boundingVolume"))
    {
        const auto& bvj = node["boundingVolume"];
        if (bvj.contains("box") && parse_box(bvj["box"], world_t, bv) && bv_usable(bv))
        {
            clamp_bv(bv);
            return bv;
        }
        if (bvj.contains("sphere") && parse_sphere(bvj["sphere"], world_t, bv) && bv_usable(bv))
        {
            clamp_bv(bv);
            return bv;
        }
        // region or anything else: fall through to fallback.
    }
    fallback_bv_from_geom_error(world_t, geom_error, bv);
    clamp_bv(bv);
    return bv;
}

TileRefine parse_refine(const json& node, TileRefine inherited)
{
    if (!node.contains("refine"))
        return inherited;
    std::string s = node["refine"].get<std::string>();
    // Spec allows lowercase or uppercase.
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (s == "ADD")
        return TileRefine::Add;
    return TileRefine::Replace;
}

// Hard caps so a hostile tileset.json (deeply nested or a huge children graph)
// cannot stack-overflow the recursion or exhaust memory before rendering starts.
constexpr int         kMaxTileDepth = 256;
constexpr std::size_t kMaxTileNodes = 2000000;

std::unique_ptr<TileNode> walk(const json& j, const glm::mat4& parent_transform,
                               float parent_geo_err, TileRefine parent_refine,
                               std::size_t& leaf_counter, std::size_t& node_counter,
                               bool& truncated, int depth)
{
    if (depth > kMaxTileDepth || node_counter >= kMaxTileNodes)
    {
        truncated = true;
        return nullptr;
    }
    ++node_counter;
    auto node       = std::make_unique<TileNode>();
    node->transform = parent_transform;
    if (j.contains("transform"))
        node->transform = parse_transform(j["transform"], parent_transform);

    node->geometric_error = parent_geo_err;
    if (j.contains("geometricError"))
    {
        const float ge = j["geometricError"].get<float>();
        // Finite, non-negative, and capped: the SSE computation multiplies it by
        // viewport height, so a finite-but-huge value (near FLT_MAX) would
        // overflow to +Inf and poison LOD selection. The cap is far above any
        // real planetary-scale error.
        if (finite_f(ge) && ge >= 0.0f)
            node->geometric_error = std::min(ge, kMaxBvExtent);  // else inherit the parent's
    }

    node->refine = parse_refine(j, parent_refine);
    node->bv     = parse_bv(j, node->transform, node->geometric_error);

    if (j.contains("content") && j["content"].contains("uri"))
    {
        node->content_uri = j["content"]["uri"].get<std::string>();
        ++leaf_counter;
    }

    if (j.contains("children") && j["children"].is_array())
    {
        for (const auto& child : j["children"])
        {
            auto c = walk(child, node->transform, node->geometric_error, node->refine, leaf_counter,
                          node_counter, truncated, depth + 1);
            if (c)
                node->children.push_back(std::move(c));
        }
    }
    return node;
}

void apply_anchor_recursive(TileNode& n, const glm::vec3& anchor, const glm::mat4& align)
{
    n.transform[3].x -= anchor.x;
    n.transform[3].y -= anchor.y;
    n.transform[3].z -= anchor.z;
    n.transform = align * n.transform;
    // Re-apply the same rebase + align to the bounding sphere's center. Radius
    // is unchanged: pure translation + pure rotation preserve it.
    n.bv.center -= anchor;
    n.bv.center = glm::vec3(align * glm::vec4(n.bv.center, 1.0f));
    // The subtract + rotate can push a finite-but-large center past the safe
    // extent; re-clamp so the renderer never sees a non-finite/overflowing center.
    clamp_bv(n.bv);
    for (auto& c : n.children)
        apply_anchor_recursive(*c, anchor, align);
}

}  // namespace

void Tileset::apply_world_anchor(const glm::vec3& anchor, const glm::mat4& align)
{
    if (root)
        apply_anchor_recursive(*root, anchor, align);
}

bool parse_tileset(ITileLoader& loader, Tileset& out, std::string* err)
{
    std::string fetch_err;
    Bytes       raw = loader.fetch(loader.root(), &fetch_err);
    if (raw.empty())
    {
        if (err)
            *err = "fetch tileset: " + fetch_err;
        return false;
    }
    json j;
    try
    {
        j = json::parse(raw.begin(), raw.end());
    }
    catch (const std::exception& e)
    {
        if (err)
            *err = std::string("parse tileset: ") + e.what();
        return false;
    }
    if (!j.contains("root"))
    {
        if (err)
            *err = "missing root";
        return false;
    }
    // The semantic parse below reads typed fields from untrusted JSON; a
    // wrong-typed value makes nlohmann's get<T>() throw. Catch here and fail
    // cleanly instead of letting it terminate the process.
    try
    {
        const auto& root = j["root"];
        if (root.contains("geometricError"))
        {
            const float ge = root["geometricError"].get<float>();
            if (finite_f(ge) && ge >= 0.0f)
                out.root_geometric_error = std::min(ge, kMaxBvExtent);
        }
        if (root.contains("transform"))
            out.root_transform = parse_transform(root["transform"], glm::mat4(1.0f));
        out.total_leaves       = 0;
        std::size_t node_count = 0;
        bool        truncated  = false;
        out.root = walk(root, out.root_transform, out.root_geometric_error, TileRefine::Replace,
                        out.total_leaves, node_count, truncated, 0);
        if (truncated)
            std::fprintf(stderr,
                         "[tileset] tile tree hit caps (depth %d / %zu nodes); rendering the "
                         "retained subtree\n",
                         kMaxTileDepth, kMaxTileNodes);
    }
    catch (const std::exception& e)
    {
        if (err)
            *err = std::string("tileset semantics: ") + e.what();
        return false;
    }
    return true;
}

}  // namespace v3dt
