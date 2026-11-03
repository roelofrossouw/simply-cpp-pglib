// sc_oneweb's C++ side (oneweb.h): the settings file, signing and the transfer, on libcurl and
// OpenSSL. Nothing here knows PostgreSQL.

#include "oneweb.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace sc_pglib::oneweb {
    namespace {
        const std::set<std::string> known_keys{"url", "key", "key_file", "timeout_ms", "connect_timeout_ms",
                                               "max_response_bytes", "ca_file", "on_error"};

        std::string_view trimmed(std::string_view text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
            return text;
        }

        // A value without a comment at its end: ";" or "#" after a space.
        std::string_view without_comment(std::string_view value) {
            for (std::size_t i = 1; i < value.size(); ++i) {
                if ((value[i] == ';' || value[i] == '#') && std::isspace(static_cast<unsigned char>(value[i - 1])))
                    return value.substr(0, i);
            }
            return value;
        }

        long number(const std::string &section, const std::string &key, const std::string &value, const long low, const long high) {
            long result = 0;
            std::size_t used = 0;
            try {
                result = std::stol(value, &used);
            } catch (const std::exception &) {
                used = 0;
            }
            if (used == 0 || used != value.size() || result < low || result > high)
                throw std::invalid_argument{"[" + section + "] " + key + " must be a whole number from " + std::to_string(low) + " to " + std::to_string(high)};
            return result;
        }

        std::string hex(const unsigned char *bytes, const std::size_t size) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(size * 2);
            for (std::size_t i = 0; i < size; ++i) {
                out += digits[bytes[i] >> 4];
                out += digits[bytes[i] & 15];
            }
            return out;
        }

        // The UTF-8 sequence's length at text[at], 0 when it isn't one.
        std::size_t utf8_length(const std::string_view text, const std::size_t at) {
            const auto first = static_cast<unsigned char>(text[at]);
            std::size_t length = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 0;
            if (length == 0 || (length == 2 && first < 0xc2) || at + length > text.size()) return 0;
            for (std::size_t i = 1; i < length; ++i)
                if ((static_cast<unsigned char>(text[at + i]) & 0xc0) != 0x80) return 0;
            return length;
        }

        void global_init() {
            static std::once_flag once;
            std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        }

        struct transfer {
            std::string *body;
            std::int64_t limit;
            bool too_large = false;
            const std::function<bool()> *cancelled;
            bool stopped = false;
        };

        std::size_t on_data(char *data, const std::size_t size, const std::size_t count, void *user) {
            auto *t = static_cast<transfer *>(user);
            const std::size_t bytes = size * count;
            if (static_cast<std::int64_t>(t->body->size() + bytes) > t->limit) {
                t->too_large = true;
                return 0; // curl stops: CURLE_WRITE_ERROR
            }
            t->body->append(data, bytes);
            return bytes;
        }

        int on_progress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            auto *t = static_cast<transfer *>(user);
            if (*t->cancelled && (*t->cancelled)()) {
                t->stopped = true;
                return 1; // curl stops: CURLE_ABORTED_BY_CALLBACK
            }
            return 0;
        }
    }

    ini parse_ini(const std::string_view text) {
        ini file;
        std::string section;
        std::size_t start = 0, line_number = 0;
        while (start <= text.size()) {
            const auto end = std::min(text.find('\n', start), text.size());
            const auto line = trimmed(text.substr(start, end - start));
            start = end + 1;
            ++line_number;
            const auto where = "line " + std::to_string(line_number);
            if (line.empty() || line.front() == ';' || line.front() == '#') {
                if (end == text.size()) break;
                continue;
            }
            if (line.front() == '[') {
                const auto close = line.find(']');
                if (close == std::string_view::npos || !trimmed(without_comment(line.substr(close + 1))).empty())
                    throw std::invalid_argument{where + ": a section is [default] or [database <name>]"};
                std::string name{trimmed(line.substr(1, close - 1))};
                if (name.starts_with("database") && name.size() > 8 && std::isspace(static_cast<unsigned char>(name[8]))) {
                    name = "database " + std::string{trimmed(std::string_view{name}.substr(9))};
                } else if (name != "default") {
                    throw std::invalid_argument{where + ": a section is [default] or [database <name>], not [" + name + "]"};
                }
                if (file.contains(name)) throw std::invalid_argument{where + ": [" + name + "] is there twice"};
                section = name;
                file[section];
            } else {
                const auto equals = line.find('=');
                if (equals == std::string_view::npos) throw std::invalid_argument{where + ": not \"key = value\""};
                const std::string key{trimmed(line.substr(0, equals))};
                const std::string value{trimmed(without_comment(trimmed(line.substr(equals + 1))))};
                if (section.empty()) throw std::invalid_argument{where + ": " + key + " is before any section"};
                if (!known_keys.contains(key)) throw std::invalid_argument{where + ": unknown setting \"" + key + "\""};
                auto &keys = file[section];
                if (keys.contains(key)) throw std::invalid_argument{where + ": " + key + " is there twice in [" + section + "]"};
                keys[key] = value;
            }
            if (end == text.size()) break;
        }
        return file;
    }

    settings settings_for(const ini &file, const std::string &database,
                          const std::function<std::string(const std::string &)> &read_file) {
        const auto own = file.find("database " + database);
        const auto fallback = file.find("default");
        if (own == file.end() && fallback == file.end())
            throw std::invalid_argument{"no [database " + database + "] or [default] section"};
        settings s;
        s.section = own != file.end() ? own->first : "default";
        // A key from the database's section, else [default]'s; nullopt when neither has it (or it's empty).
        const auto value = [&](const std::string &key) -> std::optional<std::string> {
            for (const auto &section: {own, fallback}) {
                if (section == file.end()) continue;
                const auto found = section->second.find(key);
                if (found != section->second.end()) {
                    if (found->second.empty()) return std::nullopt;
                    return found->second;
                }
            }
            return std::nullopt;
        };
        const auto url = value("url");
        if (!url) throw std::invalid_argument{"no url"};
        if (!url->starts_with("https://") || !url->ends_with("/") || url->size() <= 9)
            throw std::invalid_argument{"url must be https://... and end with \"/\""};
        s.url = *url;
        const auto key = value("key");
        const auto key_file = value("key_file");
        if (key && key_file) throw std::invalid_argument{"key and key_file both set: give one"};
        if (key) {
            s.key = *key;
        } else if (key_file) {
            s.key_file = *key_file;
            s.key = std::string{trimmed(read_file(*key_file))};
        } else {
            throw std::invalid_argument{"no key or key_file"};
        }
        if (s.key.size() < 32) throw std::invalid_argument{"the key must be at least 32 bytes"};
        if (const auto v = value("timeout_ms")) s.timeout_ms = number(s.section, "timeout_ms", *v, 1000, 600000);
        if (const auto v = value("connect_timeout_ms")) s.connect_timeout_ms = number(s.section, "connect_timeout_ms", *v, 100, 600000);
        if (const auto v = value("max_response_bytes")) s.max_response_bytes = number(s.section, "max_response_bytes", *v, 1, 1024L * 1024 * 1024);
        if (const auto v = value("ca_file")) s.ca_file = *v;
        if (const auto v = value("on_error")) {
            if (*v != "error" && *v != "warning") throw std::invalid_argument{"on_error must be error or warning"};
            s.warn = *v == "warning";
        }
        return s;
    }

    bool valid_name(const std::string_view name) {
        if (name.empty() || name.size() > 64 || name[0] < 'a' || name[0] > 'z') return false;
        return std::all_of(name.begin(), name.end(), [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
    }

    std::string compact_json(const std::string_view json) {
        std::string out;
        out.reserve(json.size());
        bool in_string = false, escaped = false;
        for (const char c: json) {
            if (in_string) {
                out += c;
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
            } else if (c == '"') {
                in_string = true;
                out += c;
            } else if (c != ' ' && c != '\n' && c != '\t' && c != '\r') {
                out += c;
            }
        }
        return out;
    }

    std::string signature(const std::string_view key, const std::string_view name, const std::string_view timestamp,
                          const std::string_view nonce, const std::string_view database, const std::string_view body) {
        std::string message;
        message.reserve(name.size() + timestamp.size() + nonce.size() + database.size() + body.size() + 4);
        message.append(name).append("\n").append(timestamp).append("\n").append(nonce).append("\n").append(database).append("\n").append(body);
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(message.data()),
                  message.size(), digest, &length))
            throw std::runtime_error{"HMAC-SHA256 failed"};
        return hex(digest, length);
    }

    std::string nonce() {
        unsigned char bytes[16];
        if (RAND_bytes(bytes, sizeof bytes) != 1) throw std::runtime_error{"no random bytes for the nonce"};
        return hex(bytes, sizeof bytes);
    }

    response send(const settings &s, const request &r, const long timeout_ms, const std::function<bool()> &cancelled) {
        global_init();
        response out;
        CURL *curl = curl_easy_init();
        if (!curl) {
            out.failure = "libcurl couldn't start";
            return out;
        }
        const auto timestamp = std::to_string(r.timestamp);
        std::vector<std::string> lines{
            "Content-Type: application/json",
            "Accept: application/json",
            "Expect:", // no 100-continue round trip
            "X-OneWeb-Database: " + r.database,
            "X-OneWeb-Role: " + r.role,
            "X-OneWeb-Session-Role: " + r.session_role,
            "X-OneWeb-Timestamp: " + timestamp,
            "X-OneWeb-Nonce: " + r.nonce,
            "X-OneWeb-Signature: " + signature(s.key, r.name, timestamp, r.nonce, r.database, r.body),
        };
        curl_slist *headers = nullptr;
        for (const auto &line: lines) headers = curl_slist_append(headers, line.c_str());
        const std::string url = s.url + r.name;
        transfer t{&out.body, s.max_response_bytes, false, &cancelled, false};

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, r.user_agent.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!s.ca_file.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, s.ca_file.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXY, ""); // none, whatever the environment says
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min(s.connect_timeout_ms, timeout_ms));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(s.max_response_bytes));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        const auto started = std::chrono::steady_clock::now();
        const CURLcode result = curl_easy_perform(curl);
        out.milliseconds = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
        if (t.stopped) {
            out.cancelled = true;
            out.failure = "cancelled";
        } else if (t.too_large || result == CURLE_FILESIZE_EXCEEDED) {
            out.failure = "the answer is larger than max_response_bytes (" + std::to_string(s.max_response_bytes) + ")";
        } else if (result == CURLE_OPERATION_TIMEDOUT) {
            out.failure = "no answer within " + std::to_string(timeout_ms) + " ms";
        } else if (result != CURLE_OK) {
            out.failure = curl_easy_strerror(result);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return out;
    }

    answer read_answer(const response &r, const std::int64_t max_response_bytes) {
        answer out;
        const auto status = "HTTP " + std::to_string(r.status);
        if (!r.failure.empty()) {
            out.error = r.status ? status + ": " + r.failure : r.failure;
            return out;
        }
        if (static_cast<std::int64_t>(r.body.size()) > max_response_bytes) {
            out.error = status + ": the answer is larger than max_response_bytes (" + std::to_string(max_response_bytes) + ")";
            return out;
        }
        const auto json = nlohmann::json::parse(r.body, nullptr, false);
        const bool shaped = !json.is_discarded() && json.is_object() && json.contains("ok") && json["ok"].is_boolean();
        if (r.status == 200 && shaped && json["ok"].get<bool>()) {
            if (!json.contains("data")) {
                out.error = status + ": an answer without data: " + excerpt(r.body);
                return out;
            }
            out.ok = true;
            out.null = json["data"].is_null();
            return out;
        }
        if (shaped && !json["ok"].get<bool>() && json.contains("error") && json["error"].is_string()) {
            // 1web's own refusal: its message, and the status when it isn't 200.
            const auto message = excerpt(json["error"].get<std::string>(), 400);
            out.error = r.status == 200 ? message : status + ": " + message;
            return out;
        }
        out.error = status + ": " + (r.body.empty() ? std::string{"(no body)"} : excerpt(r.body));
        return out;
    }

    std::string excerpt(const std::string_view text, const std::size_t characters) {
        std::string out;
        std::size_t count = 0, at = 0;
        while (at < text.size() && count < characters) {
            const auto length = utf8_length(text, at);
            if (length == 0) {
                out += '?';
                ++at;
            } else if (length == 1) {
                const char c = text[at++];
                out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : (static_cast<unsigned char>(c) < 0x20 ? '?' : c);
            } else {
                out.append(text.substr(at, length));
                at += length;
            }
            ++count;
        }
        if (at < text.size()) out += "...";
        return out;
    }
}
