// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "gltf_scene.h"

#include "gpu_caps.h"

#include <epoxy/gl.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#if defined(VIEWER_HAS_WEBP)
#include <webp/decode.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace v3dt
{

namespace
{

// Bit-pattern finiteness check (exponent all-ones => Inf/NaN). Integer test so
// it survives -ffinite-math-only / -ffast-math (which can fold std::isfinite to
// a constant). Non-finite vertex data poisons the AABB (NaN compares false),
// normal synthesis, and downstream culling, so we drop such primitives.
bool finite_f(float v)
{
    std::uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

bool all_finite(const std::vector<float>& a)
{
    for (float v : a)
        if (!finite_f(v))
            return false;
    return true;
}

// A finite-but-absurd vertex coordinate (near FLT_MAX) overflows in the vertex
// shader's transform/normalize math. Bound it to a generous extent -- far above
// any real geometry but well below the float ceiling -- and skip a primitive that
// exceeds it. (call after all_finite, which rejects NaN/Inf this can't see.)
constexpr float kMaxVertexCoord = 1.0e18f;

bool within_extent(const std::vector<float>& a, float cap)
{
    for (float v : a)
        if (v < -cap || v > cap)
            return false;
    return true;
}

// Defense-in-depth cap: a hostile .glb can claim an astronomical element count.
// Real photogrammetry tiles are far below this; reject rather than resize() into
// an OOM or a size_t-overflowing multiply.
constexpr cgltf_size kMaxAccessorElements = 32u * 1024u * 1024u;

// Read accessor into a tightly-packed float buffer (3-component pos/normal,
// 2-component uv). Handles strided source buffer views. Returns empty if the
// accessor is missing or its count is implausibly large.
std::vector<float> read_accessor_float(const cgltf_accessor* a, int components)
{
    std::vector<float> out;
    if (!a || components <= 0)
        return out;
    if (a->count > kMaxAccessorElements)
    {
        std::fprintf(stderr, "[gltf] accessor count %zu over cap, skipping\n",
                     static_cast<std::size_t>(a->count));
        return out;
    }
    out.resize(static_cast<std::size_t>(a->count) * static_cast<std::size_t>(components));
    for (cgltf_size i = 0; i < a->count; ++i)
    {
        cgltf_accessor_read_float(a, i, &out[i * components], components);
    }
    return out;
}

std::uint32_t upload_buffer(GLenum target, const void* data, std::size_t bytes, GLenum usage)
{
    std::uint32_t id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(target, id);
    glBufferData(target, static_cast<GLsizeiptr>(bytes), data, usage);
    return id;
}

constexpr int       kMaxTextureDim    = 16384;
constexpr long long kMaxTexturePixels = 64ll * 1024 * 1024;  // 64M px (~256 MB RGBA)

bool texture_dims_ok(int w, int h)
{
    return w > 0 && h > 0 && w <= kMaxTextureDim && h <= kMaxTextureDim &&
           static_cast<long long>(w) * h <= kMaxTexturePixels;
}

std::uint32_t upload_rgba8(const std::uint8_t* px, int w, int h)
{
    // Decoded dimensions are attacker-controlled; reject absurd sizes before a
    // multi-gigabyte GPU allocation or a driver crash.
    if (!px || !texture_dims_ok(w, h))
    {
        std::fprintf(stderr, "[gltf] rejecting texture %dx%d\n", w, h);
        return 0;
    }
    std::uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);

    // Anisotropy is an optional capability (absent on some software / ES stacks);
    // the caps layer probed it once at init instead of querying per-texture.
    if (gpu_caps().has_anisotropy)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                        std::min(gpu_caps().max_anisotropy, 16.0f));
    return tex;
}

bool is_webp(const std::uint8_t* d, std::size_t n)
{
    // RIFF....WEBP magic at bytes 0..3 and 8..11.
    return n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' && d[8] == 'W' &&
           d[9] == 'E' && d[10] == 'B' && d[11] == 'P';
}

std::uint32_t upload_texture_from_memory(const std::uint8_t* data, std::size_t size,
                                         const char* mime_hint)
{
    // stb takes an int length; reject blobs that would narrow to a wrong/negative
    // size, and obviously-empty inputs.
    if (!data || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        std::fprintf(stderr, "[gltf] rejecting texture blob (%zu bytes)\n", size);
        return 0;
    }
    const bool prefer_webp =
        is_webp(data, size) || (mime_hint && std::string(mime_hint) == "image/webp");
#if defined(VIEWER_HAS_WEBP)
    if (prefer_webp)
    {
        int w = 0;
        int h = 0;
        // Probe dimensions BEFORE decoding: a tiny compressed image can claim a
        // huge canvas and OOM during decode otherwise.
        if (!WebPGetInfo(data, size, &w, &h) || !texture_dims_ok(w, h))
        {
            std::fprintf(stderr, "[gltf] WebP rejected (%dx%d)\n", w, h);
            return 0;
        }
        std::uint8_t* px = WebPDecodeRGBA(data, size, &w, &h);
        if (!px)
        {
            std::fprintf(stderr, "[gltf] WebPDecodeRGBA failed\n");
            return 0;
        }
        std::uint32_t tex = upload_rgba8(px, w, h);
        WebPFree(px);
        return tex;
    }
#else
    if (prefer_webp)
    {
        std::fprintf(stderr,
                     "[gltf] WebP texture encountered but viewer was built without "
                     "ENABLE_WEBP_TEXTURE\n");
        return 0;
    }
#endif

    int w  = 0;
    int h  = 0;
    int ch = 0;
    // Probe dimensions before the full decode (decompression-bomb guard).
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &ch) || !texture_dims_ok(w, h))
    {
        std::fprintf(stderr, "[gltf] image rejected (%dx%d)\n", w, h);
        return 0;
    }
    stbi_set_flip_vertically_on_load(false);
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &ch, 4);
    if (!px)
    {
        std::fprintf(stderr, "[gltf] stb_image failed: %s\n", stbi_failure_reason());
        return 0;
    }
    std::uint32_t tex = upload_rgba8(px, w, h);
    stbi_image_free(px);
    return tex;
}

// Overflow-safe `a + b <= limit` for cgltf_size (size_t) operands.
bool sum_fits(cgltf_size a, cgltf_size b, cgltf_size limit)
{
    return a <= limit && b <= limit - a;
}

// cgltf_validate() computes its bounds with UNCHECKED size_t add/mul (accessor
// req_size, bufferView offset+size, sparse index reads), so a .glb crafted to
// overflow those wraps cgltf's own checks and drives an OOB read inside the
// library itself. Re-verify, overflow-safe, that every bufferView and accessor
// fits its backing storage BEFORE cgltf_validate / cgltf_accessor_read_* run.
bool glb_bounds_safe(const cgltf_data* d)
{
    for (cgltf_size i = 0; i < d->buffer_views_count; ++i)
    {
        const cgltf_buffer_view& bv = d->buffer_views[i];
        if (bv.buffer && bv.buffer->data && !sum_fits(bv.offset, bv.size, bv.buffer->size))
            return false;
    }
    for (cgltf_size i = 0; i < d->accessors_count; ++i)
    {
        const cgltf_accessor& a    = d->accessors[i];
        const cgltf_size      elem = cgltf_calc_size(a.type, a.component_type);
        const cgltf_size      comp = cgltf_component_size(a.component_type);
        if (elem == 0 || comp == 0)
            return false;
        // Alignment: cgltf dereferences component-typed pointers (u16*/u32*/float*);
        // an unaligned address is UB / SIGBUS on strict-alignment targets. glTF
        // requires component alignment, but a hostile .glb may not honor it.
        if (a.stride % comp != 0)
            return false;
        if (a.buffer_view)
        {
            if (((a.buffer_view->offset % comp) + (a.offset % comp)) % comp != 0)
                return false;
            const cgltf_size vsize   = a.buffer_view->size;
            cgltf_size       strided = 0;
            if (a.count > 0)
            {
                const cgltf_size n1 = a.count - 1;
                if (a.stride != 0 && n1 > (static_cast<cgltf_size>(-1) / a.stride))
                    return false;  // stride * (count - 1) overflows
                strided = a.stride * n1;
            }
            if (!sum_fits(a.offset, strided, vsize) || !sum_fits(a.offset + strided, elem, vsize))
                return false;
        }
        if (a.is_sparse)
        {
            const cgltf_accessor_sparse& sp  = a.sparse;
            const cgltf_size             isz = cgltf_component_size(sp.indices_component_type);
            if (isz == 0 || !sp.indices_buffer_view || !sp.values_buffer_view)
                return false;
            // Same component-alignment requirement for the sparse index/value reads.
            if (((sp.indices_buffer_view->offset % isz) + (sp.indices_byte_offset % isz)) % isz !=
                0)
                return false;
            if (((sp.values_buffer_view->offset % comp) + (sp.values_byte_offset % comp)) % comp !=
                0)
                return false;
            // Indices are packed: indices_byte_offset + count * isz.
            if (sp.count != 0 && isz > (static_cast<cgltf_size>(-1) / sp.count))
                return false;
            if (!sum_fits(sp.indices_byte_offset, isz * sp.count, sp.indices_buffer_view->size))
                return false;
            // Values are read STRIDED by accessor->stride, not packed by elem:
            // cgltf_find_sparse_index() returns value_data + offset * stride
            // (cgltf.h), offset in [0, count). Reach = stride*(count-1) + elem.
            cgltf_size vstrided = 0;
            if (sp.count > 0)
            {
                const cgltf_size n1 = sp.count - 1;
                if (a.stride != 0 && n1 > (static_cast<cgltf_size>(-1) / a.stride))
                    return false;
                vstrided = a.stride * n1;
            }
            if (!sum_fits(sp.values_byte_offset, vstrided, sp.values_buffer_view->size) ||
                !sum_fits(sp.values_byte_offset + vstrided, elem, sp.values_buffer_view->size))
                return false;
        }
    }
    return true;
}

// Free the GL objects owned by a single mesh. Shared by GltfScene::destroy and
// the in-flight cleanup guard in load_glb_impl.
void destroy_gpu_mesh(GpuMesh& m)
{
    if (m.vao)
        glDeleteVertexArrays(1, &m.vao);
    if (m.vbo_pos)
        glDeleteBuffers(1, &m.vbo_pos);
    if (m.vbo_normal)
        glDeleteBuffers(1, &m.vbo_normal);
    if (m.vbo_uv)
        glDeleteBuffers(1, &m.vbo_uv);
    if (m.ebo)
        glDeleteBuffers(1, &m.ebo);
    m = GpuMesh{};
}

}  // namespace

void GltfScene::destroy()
{
    for (auto& m : meshes)
        destroy_gpu_mesh(m);
    for (auto& mat : materials)
    {
        if (mat.base_color_tex)
            glDeleteTextures(1, &mat.base_color_tex);
    }
    meshes.clear();
    materials.clear();
}

static bool load_glb_impl(const std::uint8_t* data, std::size_t size, GltfScene& out,
                          std::string* err)
{
    cgltf_options opts{};
    cgltf_data*   gltf = nullptr;
    if (cgltf_parse(&opts, data, size, &gltf) != cgltf_result_success)
    {
        if (err)
            *err = "cgltf_parse failed";
        return false;
    }
    struct Guard
    {
        cgltf_data* d;
        ~Guard()
        {
            if (d)
                cgltf_free(d);
        }
    } guard{gltf};

    if (cgltf_load_buffers(&opts, gltf, nullptr) != cgltf_result_success)
    {
        if (err)
            *err = "cgltf_load_buffers failed (embedded buffer expected in .glb)";
        return false;
    }

    // First barrier: our own overflow-safe bounds check. cgltf_validate() and the
    // accessor readers use unchecked size_t arithmetic, so a crafted overflow can
    // slip past cgltf's own checks and OOB-read inside the library; verify the
    // bounds ourselves before letting cgltf touch any buffer.
    if (!glb_bounds_safe(gltf))
    {
        if (err)
            *err = "glb bounds check failed (overflow / out-of-range accessor)";
        return false;
    }
    // Second barrier: cgltf's structural validation, now safe to run.
    if (cgltf_validate(gltf) != cgltf_result_success)
    {
        if (err)
            *err = "cgltf_validate failed (rejecting malformed .glb)";
        return false;
    }

    // Materials + textures
    out.materials.reserve(gltf->materials_count);
    for (cgltf_size mi = 0; mi < gltf->materials_count; ++mi)
    {
        const cgltf_material& mat = gltf->materials[mi];
        GpuMaterial           gm;
        gm.double_sided = mat.double_sided;
        if (mat.has_pbr_metallic_roughness)
        {
            const auto& pbr      = mat.pbr_metallic_roughness;
            gm.base_color_factor = glm::vec4(pbr.base_color_factor[0], pbr.base_color_factor[1],
                                             pbr.base_color_factor[2], pbr.base_color_factor[3]);
            // cgltf parses these via atof; an oversized JSON number becomes Inf.
            // Sanitize before the factor ever reaches a shader uniform: clamp to
            // the valid [0,1] range, and reset a non-finite component to opaque 1.
            for (int k = 0; k < 4; ++k)
                gm.base_color_factor[k] = finite_f(gm.base_color_factor[k])
                                              ? glm::clamp(gm.base_color_factor[k], 0.0f, 1.0f)
                                              : 1.0f;
            if (pbr.base_color_texture.texture)
            {
                const cgltf_texture* tex = pbr.base_color_texture.texture;
                // EXT_texture_webp stores its image on a separate field; cgltf
                // promotes it via has_webp/webp_image. Prefer that when the
                // standard `image` slot is empty (the common case for tiles
                // that ship WebP-only textures, e.g. Cesium / geofox.ai).
                const cgltf_image* img = tex->image;
                if (tex->has_webp && tex->webp_image)
                    img = tex->webp_image;
                if (img && img->buffer_view)
                {
                    const auto* bv = img->buffer_view;
                    // The slice is attacker-controlled; verify it lies inside its
                    // buffer (overflow-safe) before handing a pointer to stb / WebP.
                    if (bv->buffer && bv->buffer->data && bv->size > 0 &&
                        bv->offset <= bv->buffer->size && bv->size <= bv->buffer->size - bv->offset)
                    {
                        const std::uint8_t* p =
                            static_cast<const std::uint8_t*>(bv->buffer->data) + bv->offset;
                        gm.base_color_tex = upload_texture_from_memory(p, bv->size, img->mime_type);
                    }
                }
            }
        }
        out.materials.push_back(gm);
    }

    glm::vec3 scene_min(std::numeric_limits<float>::max());
    glm::vec3 scene_max(std::numeric_limits<float>::lowest());

    // Meshes
    for (cgltf_size mi = 0; mi < gltf->meshes_count; ++mi)
    {
        const cgltf_mesh& mesh = gltf->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
        {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles)
                continue;

            const cgltf_accessor* pos_acc = nullptr;
            const cgltf_accessor* nrm_acc = nullptr;
            const cgltf_accessor* uv_acc  = nullptr;
            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
            {
                const auto& at = prim.attributes[ai];
                if (at.type == cgltf_attribute_type_position)
                    pos_acc = at.data;
                else if (at.type == cgltf_attribute_type_normal)
                    nrm_acc = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0)
                    uv_acc = at.data;
            }
            if (!pos_acc)
                continue;

            std::vector<float> pos = read_accessor_float(pos_acc, 3);
            if (pos.empty())
                continue;  // missing positions, or count over the accessor cap
            if (!all_finite(pos))
            {
                std::fprintf(stderr, "[gltf] non-finite positions, skipping primitive\n");
                continue;  // NaN/Inf would poison the AABB, normals, and culling
            }
            if (!within_extent(pos, kMaxVertexCoord))
            {
                std::fprintf(stderr, "[gltf] out-of-range positions, skipping primitive\n");
                continue;  // finite-but-absurd coords overflow the shader transform
            }
            if (prim.indices && prim.indices->count > kMaxAccessorElements)
                continue;  // hostile index count; skip before allocating GL objects
            const std::size_t vcount = pos.size() / 3;

            std::vector<float> nrm;
            if (nrm_acc && nrm_acc->count == pos_acc->count)
                nrm = read_accessor_float(nrm_acc, 3);
            // Discard supplied normals that carry NaN/Inf so the synthesis path
            // below regenerates a clean, unit-length set.
            if (!nrm.empty() && !all_finite(nrm))
                nrm.clear();
            // No usable per-vertex normals (absent, wrong cardinality, or capped):
            // synthesize them so the normal buffer matches the vertex count.
            // normalize(vec3(0)) is NaN and would collapse every lighting term, so
            // we build smooth normals from area-weighted face cross products.
            if (nrm.size() != pos.size())
            {
                nrm.assign(pos.size(), 0.0f);
                auto add_face = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2)
                {
                    // A malformed or hostile .glb can index past the vertex array
                    // (cgltf does not range-check indices for us). Skip the face
                    // rather than read/write pos[] and nrm[] out of bounds -- the
                    // content is streamed from arbitrary URLs, so this is untrusted.
                    if (i0 >= vcount || i1 >= vcount || i2 >= vcount)
                        return;
                    glm::vec3 a(pos[3 * i0 + 0], pos[3 * i0 + 1], pos[3 * i0 + 2]);
                    glm::vec3 b(pos[3 * i1 + 0], pos[3 * i1 + 1], pos[3 * i1 + 2]);
                    glm::vec3 c(pos[3 * i2 + 0], pos[3 * i2 + 1], pos[3 * i2 + 2]);
                    glm::vec3 fn = glm::cross(b - a, c - a);
                    // Positions are finite, but huge finite coordinates can make
                    // the subtraction / cross product overflow to Inf/NaN; drop
                    // such a face rather than poison the accumulated normals.
                    if (!finite_f(fn.x) || !finite_f(fn.y) || !finite_f(fn.z))
                        return;
                    for (std::uint32_t idx : {i0, i1, i2})
                    {
                        nrm[3 * idx + 0] += fn.x;
                        nrm[3 * idx + 1] += fn.y;
                        nrm[3 * idx + 2] += fn.z;
                    }
                };
                if (prim.indices)
                {
                    const cgltf_accessor* idx = prim.indices;
                    for (cgltf_size i = 0; i + 2 < idx->count; i += 3)
                    {
                        add_face(static_cast<std::uint32_t>(cgltf_accessor_read_index(idx, i)),
                                 static_cast<std::uint32_t>(cgltf_accessor_read_index(idx, i + 1)),
                                 static_cast<std::uint32_t>(cgltf_accessor_read_index(idx, i + 2)));
                    }
                }
                else
                {
                    for (std::size_t i = 0; i + 2 < vcount; i += 3)
                    {
                        add_face(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + 1),
                                 static_cast<std::uint32_t>(i + 2));
                    }
                }
                for (std::size_t v = 0; v + 2 < nrm.size(); v += 3)
                {
                    glm::vec3 n(nrm[v], nrm[v + 1], nrm[v + 2]);
                    float     len2 = glm::dot(n, n);
                    if (finite_f(len2) && len2 > 1e-12f)
                    {
                        n *= glm::inversesqrt(len2);
                        if (!finite_f(n.x) || !finite_f(n.y) || !finite_f(n.z))
                            n = glm::vec3(0.0f, 1.0f, 0.0f);
                    }
                    else
                    {
                        n = glm::vec3(0.0f, 1.0f, 0.0f);
                    }
                    nrm[v]     = n.x;
                    nrm[v + 1] = n.y;
                    nrm[v + 2] = n.z;
                }
            }
            std::vector<float> uv;
            if (uv_acc && uv_acc->count == pos_acc->count)
                uv = read_accessor_float(uv_acc, 2);
            if (uv.size() != vcount * 2 || !all_finite(uv))
                uv.assign(vcount * 2, 0.0f);

            GpuMesh gm;
            // If a later allocation in this iteration throws before gm is moved
            // into out.meshes, free its GL objects here -- out.destroy() cannot
            // see gm yet.
            struct GlMeshGuard
            {
                GpuMesh* m;
                ~GlMeshGuard()
                {
                    if (m)
                        destroy_gpu_mesh(*m);
                }
            } gl_mesh_guard{&gm};
            bool have_aabb = false;
            if (pos_acc->has_min && pos_acc->has_max && pos_acc->type == cgltf_type_vec3 &&
                pos_acc->component_type == cgltf_component_type_r_32f)
            {
                glm::vec3 mn(pos_acc->min[0], pos_acc->min[1], pos_acc->min[2]);
                glm::vec3 mx(pos_acc->max[0], pos_acc->max[1], pos_acc->max[2]);
                // min/max are attacker-controlled; trust them only if finite AND
                // within the same extent we enforce on positions, else fall back to
                // computing the box from the (already bounded) position data.
                const std::vector<float> mm = {mn.x, mn.y, mn.z, mx.x, mx.y, mx.z};
                if (all_finite(mm) && within_extent(mm, kMaxVertexCoord))
                {
                    gm.aabb_min = mn;
                    gm.aabb_max = mx;
                    have_aabb   = true;
                }
            }
            if (!have_aabb)
            {
                gm.aabb_min = glm::vec3(std::numeric_limits<float>::max());
                gm.aabb_max = glm::vec3(std::numeric_limits<float>::lowest());
                for (std::size_t i = 0; i + 2 < pos.size(); i += 3)
                {
                    gm.aabb_min = glm::min(gm.aabb_min, glm::vec3(pos[i], pos[i + 1], pos[i + 2]));
                    gm.aabb_max = glm::max(gm.aabb_max, glm::vec3(pos[i], pos[i + 1], pos[i + 2]));
                }
            }
            scene_min = glm::min(scene_min, gm.aabb_min);
            scene_max = glm::max(scene_max, gm.aabb_max);

            glGenVertexArrays(1, &gm.vao);
            glBindVertexArray(gm.vao);

            gm.vbo_pos = upload_buffer(GL_ARRAY_BUFFER, pos.data(), pos.size() * sizeof(float),
                                       GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

            gm.vbo_normal = upload_buffer(GL_ARRAY_BUFFER, nrm.data(), nrm.size() * sizeof(float),
                                          GL_STATIC_DRAW);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

            gm.vbo_uv = upload_buffer(GL_ARRAY_BUFFER, uv.data(), uv.size() * sizeof(float),
                                      GL_STATIC_DRAW);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

            if (prim.indices)
            {
                const cgltf_accessor* idx = prim.indices;
                gm.index_count            = static_cast<std::uint32_t>(idx->count);
                // Unpack to u32 regardless of source type. Indices are untrusted:
                // clamp any that fall outside the vertex array to 0, so the GPU
                // never fetches a vertex out of bounds (a hostile .glb otherwise
                // drives an OOB vertex fetch in the driver on the draw call).
                std::vector<std::uint32_t> ibuf(idx->count);
                std::uint32_t              clamped = 0;
                for (cgltf_size i = 0; i < idx->count; ++i)
                {
                    std::size_t v = cgltf_accessor_read_index(idx, i);
                    if (v >= vcount)
                    {
                        v = 0;
                        ++clamped;
                    }
                    ibuf[i] = static_cast<std::uint32_t>(v);
                }
                if (clamped)
                    std::fprintf(stderr, "[gltf] clamped %u out-of-range indices to 0\n", clamped);
                gm.index_type = GL_UNSIGNED_INT;
                gm.ebo        = upload_buffer(GL_ELEMENT_ARRAY_BUFFER, ibuf.data(),
                                              ibuf.size() * sizeof(std::uint32_t), GL_STATIC_DRAW);
            }
            else
            {
                gm.index_count = static_cast<std::uint32_t>(pos.size() / 3);
                std::vector<std::uint32_t> ibuf(gm.index_count);
                for (std::uint32_t i = 0; i < gm.index_count; ++i)
                    ibuf[i] = i;
                gm.index_type = GL_UNSIGNED_INT;
                gm.ebo        = upload_buffer(GL_ELEMENT_ARRAY_BUFFER, ibuf.data(),
                                              ibuf.size() * sizeof(std::uint32_t), GL_STATIC_DRAW);
            }

            if (prim.material)
            {
                gm.material_index = static_cast<std::int32_t>(prim.material - gltf->materials);
            }

            glBindVertexArray(0);
            out.meshes.push_back(gm);
            gl_mesh_guard.m = nullptr;  // ownership transferred to out.meshes
        }
    }

    out.aabb_min = scene_min;
    out.aabb_max = scene_max;

    std::fprintf(
        stderr, "[gltf] loaded %zu meshes, %zu materials, AABB (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
        out.meshes.size(), out.materials.size(), scene_min.x, scene_min.y, scene_min.z, scene_max.x,
        scene_max.y, scene_max.z);
    return true;
}

// Public entry point. The work above is cap-bounded but a hostile tile can still
// request large allocations; turn an OOM into a clean skip (and free any GL
// objects already created) instead of std::terminate.
bool load_glb(const std::uint8_t* data, std::size_t size, GltfScene& out, std::string* err)
{
    try
    {
        return load_glb_impl(data, size, out, err);
    }
    catch (const std::bad_alloc&)
    {
        out.destroy();
        if (err)
            *err = "out of memory loading .glb (rejecting tile)";
        return false;
    }
}

}  // namespace v3dt
