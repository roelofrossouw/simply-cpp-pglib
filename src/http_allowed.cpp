#include "http_allowed.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

namespace sc_pglib {
    namespace {
        struct parsed_url {
            std::string scheme, host, port, path;
        };

        std::string lower(std::string_view text) {
            std::string out{text};
            for (auto &c: out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // scheme://host[:port][/path][?query][#fragment]; nothing for what isn't an http(s) URL of
        // that shape, has user information, or a path that could step out of its folder.
        std::optional<parsed_url> parse(const std::string_view url) {
            const auto colon = url.find("://");
            if (colon == std::string_view::npos) return std::nullopt;
            parsed_url out;
            out.scheme = lower(url.substr(0, colon));
            if (out.scheme != "http" && out.scheme != "https") return std::nullopt;
            const auto rest = url.substr(colon + 3);
            const auto authority_end = rest.find_first_of("/?#");
            const auto authority = rest.substr(0, authority_end);
            if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;
            if (authority.find_first_of(" \t\r\n\\") != std::string_view::npos) return std::nullopt;
            std::string_view host = authority, port;
            if (authority.front() == '[') {
                const auto close = authority.find(']');
                if (close == std::string_view::npos) return std::nullopt;
                host = authority.substr(0, close + 1);
                const auto after = authority.substr(close + 1);
                if (!after.empty()) {
                    if (after.front() != ':') return std::nullopt;
                    port = after.substr(1);
                }
            } else if (const auto p = authority.rfind(':'); p != std::string_view::npos) {
                host = authority.substr(0, p);
                port = authority.substr(p + 1);
            }
            if (host.empty()) return std::nullopt;
            if (!std::all_of(port.begin(), port.end(), [](const char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) return std::nullopt;
            out.host = lower(host);
            out.port = port.empty() ? (out.scheme == "https" ? "443" : "80") : std::string{port};
            while (out.port.size() > 1 && out.port.front() == '0') out.port.erase(0, 1);
            std::string_view path = authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);
            path = path.substr(0, path.find_first_of("?#"));
            out.path = path.empty() ? "/" : std::string{path};
            // No step out of a folder, plainly or encoded
            const auto low = lower(out.path);
            if (low.find("%2e") != std::string::npos || low.find("%2f") != std::string::npos || low.find("%5c") != std::string::npos) return std::nullopt;
            std::size_t at = 0;
            while (at < out.path.size()) {
                const auto end = std::min(out.path.find('/', at + 1), out.path.size());
                const auto segment = std::string_view{out.path}.substr(at + 1, end - at - 1);
                if (segment == "." || segment == "..") return std::nullopt;
                at = end;
            }
            return out;
        }
    }

    std::vector<std::string> http_prefixes(const std::string_view setting) {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at < setting.size()) {
            const auto start = setting.find_first_not_of(", \t\r\n", at);
            if (start == std::string_view::npos) break;
            const auto end = std::min(setting.find_first_of(", \t\r\n", start), setting.size());
            const auto prefix = setting.substr(start, end - start);
            if (!parse(prefix)) throw std::invalid_argument("\"" + std::string{prefix} + "\" isn't an http:// or https:// URL");
            out.emplace_back(prefix);
            at = end;
        }
        return out;
    }

    bool http_allowed(const std::string_view url, const std::vector<std::string> &prefixes) {
        const auto asked = parse(url);
        if (!asked) return false;
        for (const auto &each: prefixes) {
            const auto prefix = parse(each);
            if (!prefix || prefix->scheme != asked->scheme || prefix->host != asked->host || prefix->port != asked->port) continue;
            auto folder = prefix->path;
            while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
            if (folder == "/") return true;
            if (asked->path == folder || asked->path.starts_with(folder + "/")) return true;
        }
        return false;
    }
}
