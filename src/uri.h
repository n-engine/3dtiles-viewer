// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
#pragma once

#include <string>

namespace v3dt
{

// Remove "." and ".." path segments per RFC 3986 section 5.2.4. The input is a
// path component only (no scheme/authority/query); the output never escapes
// above the root.
std::string remove_dot_segments(const std::string& path);

// True if `s` begins with a case-insensitive "http://" or "https://" scheme.
// URI schemes are case-insensitive (RFC 3986 3.1).
bool has_http_scheme(const std::string& s);

// Resolve a relative reference against an absolute http(s) base URL, following
// the path-merging and dot-segment rules of RFC 3986 sections 5.2.4 / 5.3.
//
// Handles the cases that the naive "base directory + relative" join gets wrong
// on real tilesets: parent traversals ("../"), absolute paths ("/x"), query
// strings ("a.glb?v=2"), same-document references, scheme-relative references
// ("//host/x"), and already-absolute http(s) references (returned unchanged).
//
// `base` is expected to be an absolute http(s) URL (e.g. the tileset.json URL).
// If it carries no scheme, the reference is returned unchanged so a non-URL
// caller can resolve it in its own namespace (e.g. the filesystem loader).
std::string resolve_relative_uri(const std::string& base, const std::string& rel);

}  // namespace v3dt
