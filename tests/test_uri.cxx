// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
//
// Unit tests for the RFC 3986 URI resolver. Pure logic: no GL, no network, no
// filesystem -- so it runs anywhere, including CI.

#include "uri.h"

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;

void check(const std::string& got, const std::string& want, const char* desc)
{
    if (got != want)
    {
        std::fprintf(stderr, "FAIL  %-22s got \"%s\"  want \"%s\"\n", desc, got.c_str(),
                     want.c_str());
        ++g_failures;
    }
}

}  // namespace

int main()
{
    using v3dt::remove_dot_segments;
    using v3dt::resolve_relative_uri;

    const std::string base = "https://host.example/a/b/tileset.json";

    // The everyday case: a sibling leaf next to the tileset.
    check(resolve_relative_uri(base, "103746.glb"), "https://host.example/a/b/103746.glb",
          "sibling");

    // Parent traversal -- the case the naive substr+join breaks on.
    check(resolve_relative_uri(base, "../c/1.glb"), "https://host.example/a/c/1.glb", "parent");
    check(resolve_relative_uri(base, "../../1.glb"), "https://host.example/1.glb", "two-parents");

    // Absolute-path reference: keep authority, replace path.
    check(resolve_relative_uri(base, "/abs/1.glb"), "https://host.example/abs/1.glb",
          "absolute-path");

    // Query string must survive the join (cache-busted content URIs).
    check(resolve_relative_uri(base, "sub/2.glb?v=3"), "https://host.example/a/b/sub/2.glb?v=3",
          "query");

    // Already-absolute references pass through untouched.
    check(resolve_relative_uri(base, "https://other.example/x.glb"), "https://other.example/x.glb",
          "absolute-uri");
    check(resolve_relative_uri(base, "http://other.example/x.glb"), "http://other.example/x.glb",
          "absolute-uri-http");

    // Leading "./" and a mid-path "..".
    check(resolve_relative_uri(base, "./d/3.glb"), "https://host.example/a/b/d/3.glb",
          "dot-prefix");
    check(resolve_relative_uri("https://host.example/t.json", "x/y/../z.glb"),
          "https://host.example/x/z.glb", "mid-dotdot");

    // Over-popping ".." can never escape the authority.
    check(resolve_relative_uri(base, "../../../../../etc/passwd"),
          "https://host.example/etc/passwd", "no-escape");

    // Scheme-relative reference inherits only the scheme.
    check(resolve_relative_uri(base, "//cdn.example/x.glb"), "https://cdn.example/x.glb",
          "scheme-relative");

    // Same-document reference: keep path, swap query.
    check(resolve_relative_uri(base, "?v=9"), "https://host.example/a/b/tileset.json?v=9",
          "same-doc-query");

    // URI schemes are case-insensitive: an uppercase absolute reference must
    // pass through, not be mistaken for a relative path.
    check(resolve_relative_uri(base, "HTTP://other.example/x.glb"), "HTTP://other.example/x.glb",
          "uppercase-scheme");

    // A fragment on a relative reference is preserved.
    check(resolve_relative_uri(base, "x.glb#sec"), "https://host.example/a/b/x.glb#sec",
          "rel-fragment");
    check(resolve_relative_uri(base, "#frag"), "https://host.example/a/b/tileset.json#frag",
          "same-doc-fragment");

    // A query string on the BASE is dropped before merging the reference.
    check(resolve_relative_uri("https://host.example/a/t.json?k=1", "x.glb"),
          "https://host.example/a/x.glb", "base-query-dropped");

    // --- Hostile / hardening cases (empty string == "rejected, skip the tile") ---

    // Control characters (CR/LF and friends) are rejected outright.
    check(resolve_relative_uri(base, "a\r\nHost: evil/x.glb"), "", "reject-crlf");
    check(resolve_relative_uri(base, std::string("x\x01y.glb")), "", "reject-ctrl");

    // Percent-encoded dot / slash in the PATH are rejected (server-side traversal).
    check(resolve_relative_uri(base, "..%2f..%2fsecret.glb"), "", "reject-pct-slash");
    check(resolve_relative_uri(base, "%2e%2e/secret.glb"), "", "reject-pct-dot");
    check(resolve_relative_uri(base, "%252e%252e/x.glb"), "", "reject-double-encoded");

    // ...but the same encoding in the QUERY is fine (signed CDN URLs use it).
    check(resolve_relative_uri(base, "x.glb?sig=a%2Fb"), "https://host.example/a/b/x.glb?sig=a%2Fb",
          "query-pct-allowed");

    // The percent-encoding screen must also catch ABSOLUTE and SCHEME-RELATIVE
    // references, not just relative ones (the path is what a server would decode).
    check(resolve_relative_uri(base, "https://h/%2e%2e/x.glb"), "", "reject-pct-absolute");
    check(resolve_relative_uri(base, "//h/%2fsecret.glb"), "", "reject-pct-scheme-relative");
    // An absolute URL with the encoding only in its query still passes.
    check(resolve_relative_uri(base, "https://h/x.glb?s=a%2Fb"), "https://h/x.glb?s=a%2Fb",
          "absolute-query-pct-allowed");

    // Backslash has no special meaning here: resolved as an ordinary path char.
    check(resolve_relative_uri(base, "a\\b.glb"), "https://host.example/a/b/a\\b.glb", "backslash");

    // IPv6 literal authority and an explicit port are preserved.
    check(resolve_relative_uri("https://[::1]:8080/a/t.json", "x.glb"),
          "https://[::1]:8080/a/x.glb", "ipv6-authority");
    check(resolve_relative_uri("https://host.example:8443/a/t.json", "x.glb"),
          "https://host.example:8443/a/x.glb", "explicit-port");

    // Base with no path component, and empty authority, stay deterministic.
    check(resolve_relative_uri("https://host.example", "x.glb"), "https://host.example/x.glb",
          "base-no-path");
    check(resolve_relative_uri("https:///a/t.json", "x.glb"), "https:///a/x.glb",
          "empty-authority");

    // remove_dot_segments in isolation (RFC 3986 5.2.4 worked examples).
    check(remove_dot_segments("/a/b/c/./../../g"), "/a/g", "rfc-example-1");
    check(remove_dot_segments("mid/content=5/../6"), "mid/6", "rfc-example-2");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "\n%d URI test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all URI tests passed\n");
    return 0;
}
