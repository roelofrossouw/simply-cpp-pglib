#pragma once
// sc_oneweb's C++ side, without PostgreSQL (sc_oneweb.cpp is the database's side): the settings
// file, the signed request and its answer. A call is only ever "run 1web's function <name> with
// these params": POST <url><name>, signed with a key both sides share.
//
// The settings come from a file on the database server (/etc/sc-pglib/oneweb.ini), never from the
// database, which is restored from production onto the development server every night:
//
//   [default]                       ; for a database without its own section
//   url = https://example.com/interface/index.php/
//   key_file = /etc/sc-pglib/oneweb.key   ; or key = <secret>; at least 32 bytes
//   timeout_ms = 30000
//   connect_timeout_ms = 5000
//   max_response_bytes = 10485760
//   ca_file =                       ; a CA bundle used instead of the system's
//   on_error = error                ; or warning
//
//   [database whd_12345]            ; one database's own settings, the rest from [default]
//   url = https://whd-12345.example.com/interface/index.php/

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace sc_pglib::oneweb {
    // The file's sections by name ("default", "database whd_12345"), each its keys. Lines are
    // "key = value", "[section]", or comments (";" or "#" first, or after a space at a line's end).
    // Throws std::invalid_argument naming the line for anything else, an unknown key or section,
    // or a key given twice.
    using ini = std::map<std::string, std::map<std::string, std::string>>;
    ini parse_ini(std::string_view text);

    struct settings {
        std::string section;            // "database whd_12345" or "default"
        std::string url;                // https://..., ending in "/"
        std::string key;                // the secret (from key or key_file)
        std::string key_file;           // where it came from; empty for key =
        long timeout_ms = 30000;
        long connect_timeout_ms = 5000;
        std::int64_t max_response_bytes = 10 * 1024 * 1024;
        std::string ca_file;
        bool warn = false;              // on_error = warning
    };

    // database's settings: its own section over [default]. read_file reads key_file. Throws
    // std::invalid_argument when nothing applies, the url or key is missing, or a value is wrong.
    settings settings_for(const ini &file, const std::string &database,
                          const std::function<std::string(const std::string &)> &read_file);

    // Whether name is a 1web function's name: ^[a-z][a-z0-9_]{0,63}$.
    bool valid_name(std::string_view name);

    // JSON text (as PostgreSQL's jsonb prints it, with ", " and ": ") without the spaces outside
    // strings.
    std::string compact_json(std::string_view json);

    // Hex HMAC-SHA256 with key over name \n timestamp \n nonce \n database \n body.
    std::string signature(std::string_view key, std::string_view name, std::string_view timestamp,
                          std::string_view nonce, std::string_view database, std::string_view body);

    // 16 random bytes as 32 hex digits. Throws std::runtime_error when there's no randomness.
    std::string nonce();

    struct request {
        std::string name, body, database, role, session_role, user_agent;
        std::time_t timestamp = 0;
        std::string nonce;
    };

    struct response {
        long status = 0;          // 0: no answer
        std::string body;
        std::string failure;      // why there was no (usable) answer: timeout, TLS, too large ...
        bool cancelled = false;   // stopped because cancelled() said so
        long milliseconds = 0;
    };

    // Sends it: POST settings.url + name, TLS verified, no redirects, no proxy, at most
    // timeout_ms (the setting, or one for this call). cancelled() is asked while it runs and
    // stops it. Never throws for the transfer's own failures: they're in response::failure.
    response send(const settings &s, const request &r, long timeout_ms, const std::function<bool()> &cancelled);

    // What 1web answered. ok: the body is {"ok": true, "data": ...}, and null says data is null
    // (the database reads data from the body itself, so numbers keep every digit); otherwise
    // error says why (the call's name not included).
    struct answer {
        bool ok = false;
        bool null = false;
        std::string error;
    };
    answer read_answer(const response &r, std::int64_t max_response_bytes);

    // At most 200 characters of text, on one line, invalid UTF-8 as "?".
    std::string excerpt(std::string_view text, std::size_t characters = 200);
}
