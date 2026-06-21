// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2026 naskel.com
// https://github.com/n-engine/3dtiles-viewer
// libcurl-backed HTTP(S) loader. Synchronous in v0.1; async in v0.2.

#include "tile_loader.h"
#include "uri.h"

#include <curl/curl.h>

// Older libcurl headers (< 7.87.0) lack this helper; fall back to a passthrough.
#ifndef CURL_IGNORE_DEPRECATION
#define CURL_IGNORE_DEPRECATION(statements) statements
#endif

#include <cstdio>
#include <string>

namespace v3dt
{

namespace
{

class CurlGlobal
{
   public:
    static CurlGlobal& instance()
    {
        static CurlGlobal g;
        return g;
    }

   private:
    CurlGlobal()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobal()
    {
        curl_global_cleanup();
    }
};

// libcurl write callback. It runs across a C call frame, so it must never let an
// exception unwind; and a hostile/buggy server must not be able to drive
// unbounded growth, an integer overflow, or OOM. Bounded + overflow-checked +
// noexcept; returning a short count tells libcurl to abort the transfer.
constexpr std::size_t kMaxResponseBytes = 256u * 1024u * 1024u;  // 256 MB per fetch

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) noexcept
{
    auto* out = static_cast<Bytes*>(userp);
    if (size != 0 && nmemb > (static_cast<std::size_t>(-1) / size))
        return 0;  // size * nmemb would overflow
    const std::size_t n = size * nmemb;
    if (n > kMaxResponseBytes - out->size())
        return 0;  // response exceeds the per-fetch cap
    const auto* src = static_cast<const std::uint8_t*>(contents);
    try
    {
        out->insert(out->end(), src, src + n);
    }
    catch (...)
    {
        return 0;  // allocation failure: abort cleanly rather than unwind through C
    }
    return n;
}

}  // namespace

class HttpTileLoader final : public ITileLoader
{
   public:
    explicit HttpTileLoader(std::string url) : tileset_url_(std::move(url))
    {
        CurlGlobal::instance();
    }

    std::string root() const override
    {
        return tileset_url_;
    }
    const char* backend_name() const override
    {
        return "http";
    }

    Bytes fetch(const std::string& relative_path, std::string* err) override
    {
        std::string target = (relative_path == tileset_url_)
                                 ? tileset_url_
                                 : resolve_relative_uri(tileset_url_, relative_path);

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            if (err)
                *err = "curl_easy_init failed";
            return {};
        }
        Bytes buf;
        char  errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

        // Confine the transport to HTTP(S). Without this a redirect (we follow
        // them) could hand libcurl a file://, scp://, gopher:// ... URL and turn
        // a tileset fetch into a local-file read or an SSRF primitive. Lock both
        // the initial request and the redirect target.
        //
        // LIBCURL_VERSION_NUM is the COMPILE-time header version; the runtime lib
        // may be older and reject the string API with CURLE_UNKNOWN_OPTION. So we
        // try the string API, fall back to the (still supported) bitmask API, and
        // only if NEITHER sticks do we fail CLOSED -- never fetch on an unlocked
        // handle.
        CURLcode prc = CURLE_UNKNOWN_OPTION;
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0: the string-based protocol API
        prc = curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        if (prc == CURLE_OK)
            prc = curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
        if (prc != CURLE_OK)
        {
            // The bitmask API is deprecated since 7.85.0 but still works; curl's
            // own macro silences the deprecation note where the header provides it.
            CURL_IGNORE_DEPRECATION(
                prc = curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
                if (prc == CURLE_OK) prc = curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                                                            CURLPROTO_HTTP | CURLPROTO_HTTPS);)
        }
        if (prc != CURLE_OK)
        {
            curl_easy_cleanup(curl);
            if (err)
                *err = "curl: failed to restrict transport to http/https";
            return {};
        }
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);  // bound a redirect loop
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        // Abort a stalled transfer: < 1 KB/s sustained for 30 s. A hostile or
        // dead server cannot pin a worker thread on a trickle forever.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "3dtiles-viewer/0.2");
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        CURLcode rc        = curl_easy_perform(curl);
        long     http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK)
        {
            if (err)
                *err = std::string("curl: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
            return {};
        }
        if (http_code >= 400)
        {
            if (err)
                *err = "HTTP " + std::to_string(http_code) + " on " + target;
            return {};
        }
        return buf;
    }

   private:
    std::string tileset_url_;
};

std::unique_ptr<ITileLoader> make_http_loader(const std::string& tileset_url, std::string* err)
{
    if (!has_http_scheme(tileset_url))
    {
        if (err)
            *err = "not an http(s) URL: " + tileset_url;
        return {};
    }
    return std::make_unique<HttpTileLoader>(tileset_url);
}

}  // namespace v3dt
