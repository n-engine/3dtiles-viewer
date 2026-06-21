// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer

#include "uri.h"

namespace v3dt
{

namespace
{

char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// Case-insensitive substring search.
bool contains_ci(const std::string& hay, const char* needle)
{
    std::string lo = hay;
    for (char& c : lo)
        c = ascii_lower(c);
    return lo.find(needle) != std::string::npos;
}

// True if the PATH component of any reference -- absolute ("scheme://auth/path"),
// scheme-relative ("//auth/path"), or relative ("path") -- contains a
// percent-encoded dot / slash / percent. Such an encoding survives our
// dot-segment removal as literal text, but a server may decode it to "." / "/"
// and perform its own path traversal. The query / fragment is intentionally NOT
// screened (signed CDN URLs legitimately carry "%2F" there).
bool path_has_bad_encoding(const std::string& ref)
{
    std::size_t       path_begin = 0;
    const std::size_t scheme     = ref.find("://");
    if (scheme != std::string::npos)
    {
        const std::size_t p = ref.find_first_of("/?#", scheme + 3);
        path_begin          = (p == std::string::npos || ref[p] != '/') ? std::string::npos : p;
    }
    else if (ref.rfind("//", 0) == 0)
    {
        const std::size_t p = ref.find_first_of("/?#", 2);
        path_begin          = (p == std::string::npos || ref[p] != '/') ? std::string::npos : p;
    }
    if (path_begin == std::string::npos)
        return false;  // no path component to screen
    const std::size_t path_end = ref.find_first_of("?#", path_begin);
    const std::string path     = ref.substr(
        path_begin, path_end == std::string::npos ? std::string::npos : path_end - path_begin);
    return contains_ci(path, "%2e") || contains_ci(path, "%2f") || contains_ci(path, "%25");
}

}  // namespace

// Case-insensitive test for a leading "http://" / "https://" scheme. URI schemes
// are case-insensitive (RFC 3986 3.1), so "HTTP://host" is a valid absolute
// reference and must not be mistaken for a relative path.
bool has_http_scheme(const std::string& s)
{
    for (const char* pfx : {"http://", "https://"})
    {
        std::size_t n = 0;
        while (pfx[n] != '\0')
            ++n;
        if (s.size() < n)
            continue;
        bool match = true;
        for (std::size_t i = 0; i < n; ++i)
            if (ascii_lower(s[i]) != pfx[i])
            {
                match = false;
                break;
            }
        if (match)
            return true;
    }
    return false;
}

// RFC 3986 section 5.2.4: iteratively consume the input buffer, copying real
// path segments to the output and collapsing "." / ".." in place. Pure string
// manipulation; no allocation surprises beyond the two working strings.
std::string remove_dot_segments(const std::string& path)
{
    std::string in = path;
    std::string out;
    out.reserve(in.size());

    auto drop_last_segment = [&out]()
    {
        const auto pos = out.find_last_of('/');
        if (pos == std::string::npos)
            out.clear();
        else
            out.erase(pos);
    };

    while (!in.empty())
    {
        if (in.rfind("../", 0) == 0)
            in.erase(0, 3);
        else if (in.rfind("./", 0) == 0)
            in.erase(0, 2);
        else if (in.rfind("/./", 0) == 0)
            in.replace(0, 3, "/");
        else if (in == "/.")
            in = "/";
        else if (in.rfind("/../", 0) == 0)
        {
            in.replace(0, 4, "/");
            drop_last_segment();
        }
        else if (in == "/..")
        {
            in = "/";
            drop_last_segment();
        }
        else if (in == "." || in == "..")
            in.clear();
        else
        {
            // Move the first path segment (including any leading '/') to output.
            const std::size_t start = (in[0] == '/') ? 1 : 0;
            const std::size_t next  = in.find('/', start);
            if (next == std::string::npos)
            {
                out += in;
                in.clear();
            }
            else
            {
                out += in.substr(0, next);
                in.erase(0, next);
            }
        }
    }
    return out;
}

std::string resolve_relative_uri(const std::string& base, const std::string& rel)
{
    if (rel.empty())
        return base;

    // Reject control characters (CR/LF and other bytes < 0x20, plus DEL): they
    // have no place in a tile URL and, depending on the libcurl version, could
    // enable request/header smuggling. Empty result => the loader skips the tile.
    for (unsigned char c : rel)
        if (c < 0x20 || c == 0x7F)
            return std::string();

    // Screen the path of EVERY reference shape for percent-encoded traversal,
    // before the absolute / scheme-relative shortcuts return below.
    if (path_has_bad_encoding(rel))
        return std::string();

    // An already-absolute http(s) reference is used as-is. (Other schemes are a
    // job for the transport's protocol allow-list, not the resolver.)
    if (has_http_scheme(rel))
        return rel;

    const std::size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos)
        return rel;  // base is not an absolute URL; caller resolves it elsewhere

    // Scheme-relative reference ("//host/path"): inherit only the base scheme.
    if (rel.rfind("//", 0) == 0)
        return base.substr(0, scheme_end) + ":" + rel;

    // Split base into "scheme://authority" and its path. The authority ends at
    // the first '/', '?' or '#' after "://".
    const std::size_t auth_start = scheme_end + 3;
    const std::size_t path_start = base.find_first_of("/?#", auth_start);
    std::string       scheme_authority;
    std::string       base_path;
    if (path_start == std::string::npos)
    {
        scheme_authority = base;
        base_path        = "/";
    }
    else
    {
        scheme_authority = base.substr(0, path_start);
        base_path        = (base[path_start] == '/') ? base.substr(path_start) : "/";
    }
    // Drop the base query/fragment; only its path participates in merging.
    const std::size_t base_qf = base_path.find_first_of("?#");
    if (base_qf != std::string::npos)
        base_path.erase(base_qf);

    // Separate the reference's query/fragment; only its path is merged.
    std::string       rel_path = rel;
    std::string       rel_suffix;
    const std::size_t rel_qf = rel.find_first_of("?#");
    if (rel_qf != std::string::npos)
    {
        rel_path   = rel.substr(0, rel_qf);
        rel_suffix = rel.substr(rel_qf);
    }

    // Same-document reference: keep the base path, swap query/fragment.
    if (rel_path.empty())
        return scheme_authority + base_path + rel_suffix;

    std::string merged;
    if (rel_path[0] == '/')
    {
        merged = rel_path;  // absolute-path reference
    }
    else
    {
        const std::size_t slash = base_path.find_last_of('/');
        const std::string base_dir =
            (slash == std::string::npos) ? "/" : base_path.substr(0, slash + 1);
        merged = base_dir + rel_path;
    }
    return scheme_authority + remove_dot_segments(merged) + rel_suffix;
}

}  // namespace v3dt
