// What 1web's plperlu and plpython3u functions did, natively (sc-pglib 1.18), so its database can drop
// the untrusted languages:
//
//   sc_html_encode(text) -> text        HTML::HTML5::Entities' encode_entities (sc::html, sc-web)
//   sc_html_decode(text) -> text        its decode_entities
//   sc_uri_encode(text) -> text         every byte but A-Z a-z 0-9 - . _ ~ as %XX (URI::Encode with
//                                       encode_reserved)
//   sc_reverse_dns(inet) -> text        the address' name in reverse DNS; NULL when it has none
//   sc_read_dsn(path) -> jsonb          a bounce's fields (sc::mime::delivery_report, sc-mime)
//   sc_read_text(path) -> text          a file's text, as it is (it must be in the database's encoding)
//   sc_write_text(path, text, append)   a file written (or appended to); its size in bytes
//   sc_http_get(url, params, headers, timeout_ms) -> jsonb
//                                       an HTTP GET of a URL sc_pglib.http_urls allows: {status,
//                                       content_type, body, json (the body parsed, or null)}
//
// The file functions keep to the places the others do (paths.h): to read, the output and efile
// folders; to write, the output folder; any path for roles with pg_read/write_server_files.
// EXECUTE on sc_read_dsn, sc_read_text, sc_write_text and sc_http_get is revoked from PUBLIC.

#include <cerrno>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <html_entities.h>
#include <ip_endpoint.h>
#include <nlohmann/json.hpp>
#include <rest.h>
#include <sc_mime.h>

#include "http_allowed.h"
#include "paths.h"
#include "pg_glue.h"

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <catalog/pg_authid.h>
#include <mb/pg_wchar.h>
#include <utils/builtins.h>
#include <utils/fmgrprotos.h>
#include <utils/guc.h>
#include <utils/inet.h>
#include <utils/jsonb.h>

PG_FUNCTION_INFO_V1(sc_html_encode);
PG_FUNCTION_INFO_V1(sc_html_decode);
PG_FUNCTION_INFO_V1(sc_uri_encode);
PG_FUNCTION_INFO_V1(sc_reverse_dns);
PG_FUNCTION_INFO_V1(sc_read_dsn);
PG_FUNCTION_INFO_V1(sc_read_text);
PG_FUNCTION_INFO_V1(sc_write_text);
PG_FUNCTION_INFO_V1(sc_http_get);
}

namespace {
    char *http_urls = nullptr; // the GUC sc_pglib.http_urls

    using conversion = std::string (*)(std::string_view);

    // convert run on the text argument: its result as new text, any exception as a pending ERROR.
    struct varlena *converted(const conversion convert, const std::string_view text, const char *function, sc_pglib::pending_error &error) noexcept {
        try {
            return sc_pglib::new_varlena(convert(text), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s: %s", function, problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s: unexpected C++ exception", function);
        }
        return nullptr;
    }

    Datum convert_text(FunctionCallInfo fcinfo, const conversion convert, const char *function) {
        const std::string_view text = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(0));
        sc_pglib::pending_error error;
        struct varlena *result = converted(convert, text, function, error);
        error.raise();
        PG_RETURN_TEXT_P(result);
    }

    std::string encode(const std::string_view text) { return sc::html::encode_entities(text); }
    std::string decode(const std::string_view text) { return sc::html::decode_entities(text); }
    std::string uri(const std::string_view text) { return sc::rest::url_encode(text); }

    // The address' name, or nullptr when it has none. Never raises.
    char *host_name(const char *address, sc_pglib::pending_error &error) noexcept {
        try {
            if (const auto name = sc::ip_endpoint{address}.host_name()) return sc_pglib::new_cstring(*name, error);
        } catch (...) {
        }
        return nullptr;
    }

    std::string read_file(const char *path) {
        // O_NOFOLLOW: permitted_path checked there's no link on the way; nothing planted since may be one either
        const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error(strerror(errno));
        std::string out;
        char buffer[65536];
        for (;;) {
            const auto got = ::read(fd, buffer, sizeof buffer);
            if (got < 0) {
                const int code = errno;
                ::close(fd);
                throw std::runtime_error(strerror(code));
            }
            if (got == 0) break;
            out.append(buffer, static_cast<std::size_t>(got));
        }
        ::close(fd);
        return out;
    }

    // A bounce's fields as JSON text, or nullptr with error set. Never raises.
    char *dsn_json(const char *path, sc_pglib::pending_error &error) noexcept {
        try {
            const auto report = sc::mime::delivery_report(sc::mime::message::parse(read_file(path)));
            nlohmann::json out = nlohmann::json::object();
            for (const auto &[name, value]: report) out[name] = value;
            return sc_pglib::new_cstring(out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_read_dsn: \"%s\": %s", path, problem.what());
        } catch (...) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_read_dsn: unexpected C++ exception");
        }
        return nullptr;
    }

    struct varlena *text_of(const char *path, sc_pglib::pending_error &error) noexcept {
        try {
            return sc_pglib::new_varlena(read_file(path), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_read_text: \"%s\": %s", path, problem.what());
        } catch (...) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_read_text: unexpected C++ exception");
        }
        return nullptr;
    }

    // The bytes written, or -1 with error set. A new file is made readable by all (0644): the server's umask
    // (077) would leave it to postgres alone, which the programs that collect these files (as other users,
    // over NFS) can't read; an existing file keeps its mode. Never raises.
    int64 write_file(const char *path, const std::string_view text, const bool append, sc_pglib::pending_error &error) noexcept {
        const int flags = O_WRONLY | O_NOFOLLOW | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
        int fd = ::open(path, flags);
        if (fd < 0 && errno == ENOENT) {
            fd = ::open(path, flags | O_CREAT | O_EXCL, 0644);
            if (fd < 0 && errno == EEXIST) fd = ::open(path, flags); // made by someone else meanwhile
            else if (fd >= 0 && ::fchmod(fd, 0644) != 0) {
                error.set(ERRCODE_IO_ERROR, ERROR, "sc_write_text: \"%s\": %s", path, strerror(errno));
                ::close(fd);
                return -1;
            }
        }
        if (fd < 0) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_write_text: \"%s\": %s", path, strerror(errno));
            return -1;
        }
        std::size_t done = 0;
        while (done < text.size()) {
            const auto wrote = ::write(fd, text.data() + done, text.size() - done);
            if (wrote < 0) {
                if (errno == EINTR) continue;
                error.set(ERRCODE_IO_ERROR, ERROR, "sc_write_text: \"%s\": %s", path, strerror(errno));
                ::close(fd);
                return -1;
            }
            done += static_cast<std::size_t>(wrote);
        }
        if (::close(fd) != 0) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_write_text: \"%s\": %s", path, strerror(errno));
            return -1;
        }
        return static_cast<int64>(done);
    }

    // A query parameter's value as Python's urlencode wrote it from JSON: text as it is, anything
    // else as JSON writes it (true, 12.5); null as empty.
    std::string parameter_value(const nlohmann::json &value) {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_null()) return {};
        return value.dump();
    }

    // The GET's answer as JSON text, or nullptr with error set. Never raises.
    char *http_get(const char *url, const char *params, const char *headers, const int timeout_ms, const char *allowed,
                   sc_pglib::pending_error &error) noexcept {
        try {
            const auto prefixes = sc_pglib::http_prefixes(allowed ? allowed : "");
            if (!sc_pglib::http_allowed(url, prefixes)) {
                error.set(ERRCODE_INSUFFICIENT_PRIVILEGE, ERROR, "sc_http_get: \"%s\" isn't under any of sc_pglib.http_urls", url);
                return nullptr;
            }
            std::string address = url;
            if (params) {
                const auto given = nlohmann::json::parse(params);
                if (!given.is_object()) throw std::invalid_argument("params is a JSON object of the query's parameters");
                std::string query;
                for (const auto &[name, value]: given.items())
                    query += (query.empty() ? "" : "&") + sc::rest::url_encode(name) + "=" + sc::rest::url_encode(parameter_value(value));
                if (!query.empty()) address += (address.find('?') == std::string::npos ? "?" : "&") + query;
            }
            sc::rest call{address};
            call.timeout((timeout_ms + 999) / 1000).connect_timeout(10).quiet();
            if (headers) {
                const auto given = nlohmann::json::parse(headers);
                if (!given.is_object()) throw std::invalid_argument("headers is a JSON object of header names and values");
                for (const auto &[name, value]: given.items()) call.header(name, parameter_value(value));
            }
            const auto body = call.get();
            if (call.status() == 0) {
                error.set(ERRCODE_CONNECTION_FAILURE, ERROR, "sc_http_get: no answer from %s: %s", url, call.error().c_str());
                return nullptr;
            }
            nlohmann::json out{{"status", call.status()}};
            const auto &got = call.response_headers();
            const auto type = got.find("content-type");
            out["content_type"] = type == got.end() ? nlohmann::json(nullptr) : nlohmann::json(type->second);
            out["body"] = body;
            const auto parsed = nlohmann::json::parse(body, nullptr, false);
            out["json"] = parsed.is_discarded() ? nlohmann::json(nullptr) : parsed;
            return sc_pglib::new_cstring(out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_http_get: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_http_get: unexpected C++ exception");
        }
        return nullptr;
    }

    // sc_pglib.http_urls' check: every entry an http(s) URL. Never raises.
    bool check_http_urls(char **value, void **, GucSource) {
        try {
            (void) sc_pglib::http_prefixes(*value ? *value : "");
            return true;
        } catch (const std::exception &problem) {
            GUC_check_errdetail("%s", problem.what());
        } catch (...) {
        }
        return false;
    }

    char *jsonb_text(FunctionCallInfo fcinfo, const int argument) {
        if (PG_NARGS() <= argument || PG_ARGISNULL(argument)) return nullptr;
        Jsonb *value = PG_GETARG_JSONB_P(argument);
        return JsonbToCString(nullptr, &value->root, VARSIZE(value));
    }
}

namespace sc_pglib {
    // Called from _PG_init.
    void define_text_settings() {
        DefineCustomStringVariable("sc_pglib.http_urls", "The URLs sc_http_get() may fetch: prefixes, commas between.",
                                   "https://host fetches anything there; https://host/path that path and below. Set in postgresql.conf.",
                                   &http_urls, default_http_urls, PGC_SIGHUP, 0, check_http_urls, nullptr, nullptr);
    }
}

// sc_html_encode(text) -> text
extern "C" Datum sc_html_encode(PG_FUNCTION_ARGS) { return convert_text(fcinfo, encode, "sc_html_encode"); }

// sc_html_decode(text) -> text
extern "C" Datum sc_html_decode(PG_FUNCTION_ARGS) {
    const std::string_view text = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(0));
    sc_pglib::pending_error error;
    struct varlena *result = converted(decode, text, "sc_html_decode", error);
    error.raise();
    // A reference may name a character the database's encoding can't hold
    pg_verifymbstr(VARDATA(result), static_cast<int>(VARSIZE(result) - VARHDRSZ), false);
    PG_RETURN_TEXT_P(result);
}

// sc_uri_encode(text) -> text
extern "C" Datum sc_uri_encode(PG_FUNCTION_ARGS) { return convert_text(fcinfo, uri, "sc_uri_encode"); }

// sc_reverse_dns(inet) -> text
extern "C" Datum sc_reverse_dns(PG_FUNCTION_ARGS) {
    const char *address = text_to_cstring(DatumGetTextPP(DirectFunctionCall1(network_host, PG_GETARG_DATUM(0))));
    sc_pglib::pending_error error;
    char *name = host_name(address, error);
    error.raise();
    if (!name) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(name));
}

// sc_read_dsn(path text) -> jsonb
extern "C" Datum sc_read_dsn(PG_FUNCTION_ARGS) {
    const char *path = sc_pglib::permitted_path(fcinfo, "sc_read_dsn", ROLE_PG_READ_SERVER_FILES);
    sc_pglib::pending_error error;
    char *json = dsn_json(path, error);
    error.raise();
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
}

// sc_read_text(path text) -> text
extern "C" Datum sc_read_text(PG_FUNCTION_ARGS) {
    const char *path = sc_pglib::permitted_path(fcinfo, "sc_read_text", ROLE_PG_READ_SERVER_FILES);
    sc_pglib::pending_error error;
    struct varlena *result = text_of(path, error);
    error.raise();
    pg_verifymbstr(VARDATA(result), static_cast<int>(VARSIZE(result) - VARHDRSZ), false);
    PG_RETURN_TEXT_P(result);
}

// sc_write_text(path text, content text, append boolean = false) -> bigint
extern "C" Datum sc_write_text(PG_FUNCTION_ARGS) {
    const char *path = sc_pglib::permitted_path(fcinfo, "sc_write_text", ROLE_PG_WRITE_SERVER_FILES);
    if (PG_ARGISNULL(1)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("sc_write_text: the text can't be NULL")));
    const std::string_view text = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const bool append = PG_NARGS() > 2 && !PG_ARGISNULL(2) && PG_GETARG_BOOL(2);
    sc_pglib::pending_error error;
    const int64 written = write_file(path, text, append, error);
    error.raise();
    PG_RETURN_INT64(written);
}

// sc_http_get(url text, params jsonb = NULL, headers jsonb = NULL, timeout_ms integer = NULL) -> jsonb
extern "C" Datum sc_http_get(PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("sc_http_get: the URL can't be NULL")));
    const char *url = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *params = jsonb_text(fcinfo, 1);
    const char *headers = jsonb_text(fcinfo, 2);
    int timeout_ms = 30000;
    if (PG_NARGS() > 3 && !PG_ARGISNULL(3)) {
        timeout_ms = PG_GETARG_INT32(3);
        if (timeout_ms < 1000 || timeout_ms > 300000)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_http_get: timeout_ms must be 1000 to 300000, not %d", timeout_ms)));
    }
    const char *allowed = http_urls ? pstrdup(http_urls) : nullptr;
    sc_pglib::pending_error error;
    char *json = http_get(url, params, headers, timeout_ms, allowed, error);
    error.raise();
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
}
