#pragma once
// Which URLs sc_http_get() may fetch: those under one of the configured prefixes (the setting
// sc_pglib.http_urls, set in postgresql.conf: the database's own roles can't widen it). Without
// PostgreSQL, so the tests have it.
//
// A prefix is a scheme, host and optional port, and optionally a path:
//   https://api.example.com            any path on that host
//   https://api.example.com/tracking   /tracking and what's below it (/tracking/x, /tracking?x),
//                                      not /trackingx
// Scheme and host compare without case, the port as the scheme's default when it's left out. A URL
// with user information (user@host), a ".." or "." path segment, or an encoded dot or slash in its
// path is never allowed, so none can step out of a prefix.

#include <string>
#include <string_view>
#include <vector>

namespace sc_pglib {
    // The prefixes in a setting: commas, spaces or new lines between them; std::invalid_argument
    // naming one that isn't an http(s) URL.
    std::vector<std::string> http_prefixes(std::string_view setting);

    // Whether url is under one of prefixes.
    bool http_allowed(std::string_view url, const std::vector<std::string> &prefixes);

    // The URLs 1web's plpython3u functions fetched (2026-11): sc_pglib.http_urls' default.
    inline constexpr const char *default_http_urls =
        "https://csv.webfleet.com/extern, https://truck-assist-application.firebaseio.com, https://oauth2.googleapis.com/token, "
        "https://accounts.google.com/o/oauth2/auth, https://www.googleapis.com/oauth2/v1/certs, "
        "https://www.googleapis.com/robot/v1/metadata/x509";
}
