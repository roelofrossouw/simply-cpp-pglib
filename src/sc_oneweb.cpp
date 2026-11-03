// sc_oneweb(name, params, timeout_ms) -> jsonb: the database asks the 1web site to run one of its
// functions by name and gets back its answer's data (oneweb.h has the request, the settings file
// and the answer). sc_oneweb_config() -> jsonb: the settings in force for this database, without
// the key. The settings file is sc_pglib.oneweb_config (superusers only; default
// /etc/sc-pglib/oneweb.ini), read again whenever it changes.
//
// Not general HTTP: only "run <name>" on the one site the file names for this database. EXECUTE
// on both is revoked from PUBLIC: 1web grants SECURITY DEFINER wrappers instead. Nothing of the
// params, the body or the key goes into the server's log; DEBUG1 says the name, status and time.

#include <cerrno>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "oneweb.h"
#include "pg_glue.h"

extern "C" {
#include <commands/dbcommands.h>
#include <miscadmin.h>
#include <utils/builtins.h>
#include <utils/fmgrprotos.h>
#include <utils/guc.h>
#include <utils/jsonb.h>

PG_FUNCTION_INFO_V1(sc_oneweb);
PG_FUNCTION_INFO_V1(sc_oneweb_config);
}

#ifndef SC_PGLIB_VERSION
#define SC_PGLIB_VERSION "0"
#endif

namespace {
    char *config_path = nullptr; // the GUC sc_pglib.oneweb_config

    // The settings file as last read, again when its time or size changes.
    struct cached_file {
        std::string path;
        std::filesystem::file_time_type modified{};
        std::uintmax_t size = 0;
        sc_pglib::oneweb::ini file;
        std::string problem; // why it couldn't be read or parsed; empty when it could
    };
    cached_file cache;

    std::string read_whole(const std::string &path) {
        std::ifstream in{path, std::ios::binary};
        if (!in) throw std::invalid_argument{"can't read " + path + ": " + std::generic_category().message(errno)};
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    const sc_pglib::oneweb::ini &settings_file(const std::string &path) {
        std::error_code failed;
        const auto modified = std::filesystem::last_write_time(path, failed);
        if (failed) throw std::invalid_argument{"there's no " + path + " (" + failed.message() + ")"};
        const auto size = std::filesystem::file_size(path, failed);
        if (failed || cache.path != path || cache.modified != modified || cache.size != size) {
            cache = {};
            cache.path = path;
            cache.modified = modified;
            cache.size = failed ? 0 : size;
            try {
                cache.file = sc_pglib::oneweb::parse_ini(read_whole(path));
            } catch (const std::exception &problem) {
                cache.problem = problem.what();
            }
        }
        if (!cache.problem.empty()) throw std::invalid_argument{path + ": " + cache.problem};
        return cache.file;
    }

    // The settings for database, or an ERROR's text in error.
    std::optional<sc_pglib::oneweb::settings> settings_of(const std::string &path, const std::string &database,
                                                         sc_pglib::pending_error &error) {
        try {
            return sc_pglib::oneweb::settings_for(settings_file(path), database, read_whole);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_CONFIG_FILE_ERROR, ERROR, "sc_oneweb is not set up for database \"%s\": %s", database.c_str(), problem.what());
        }
        return std::nullopt;
    }

    struct call {
        // In: plain data from PostgreSQL.
        const char *name;
        const char *params; // JSON text as jsonb prints it
        long timeout_ms;    // 0: the setting
        const char *database, *role, *session_role, *path;
        // Out.
        sc_pglib::pending_error error;
        char *body = nullptr; // the answer, for jsonb_in (palloc'd)
        bool null = false;
        bool cancelled = false;
        long status = 0, milliseconds = 0;
    };

    // The C++ part of a call: no PostgreSQL error can happen in here.
    void run(call &c) noexcept {
        try {
            if (!sc_pglib::oneweb::valid_name(c.name)) {
                c.error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_oneweb: \"%s\" isn't a 1web function name (^[a-z][a-z0-9_]{0,63}$)",
                            sc_pglib::oneweb::excerpt(c.name, 80).c_str());
                return;
            }
            const auto s = settings_of(c.path, c.database, c.error);
            if (!s) return;
            sc_pglib::oneweb::request r;
            r.name = c.name;
            r.body = sc_pglib::oneweb::compact_json(c.params);
            r.database = c.database;
            r.role = c.role;
            r.session_role = c.session_role;
            r.user_agent = "sc-pglib/" SC_PGLIB_VERSION;
            r.timestamp = std::time(nullptr);
            r.nonce = sc_pglib::oneweb::nonce();
            const auto answer = sc_pglib::oneweb::send(*s, r, c.timeout_ms ? c.timeout_ms : s->timeout_ms,
                                                       [] { return QueryCancelPending || ProcDiePending; });
            c.status = answer.status;
            c.milliseconds = answer.milliseconds;
            if (answer.cancelled) {
                c.cancelled = true;
                c.error.set(ERRCODE_QUERY_CANCELED, ERROR, "sc_oneweb(%s): cancelled", c.name);
                return;
            }
            const auto got = sc_pglib::oneweb::read_answer(answer, s->max_response_bytes);
            if (!got.ok) {
                const int code = answer.status == 0 ? ERRCODE_CONNECTION_FAILURE : ERRCODE_EXTERNAL_ROUTINE_EXCEPTION;
                c.error.set(code, s->warn ? WARNING : ERROR, "sc_oneweb(%s): %s", c.name, got.error.c_str());
                return;
            }
            c.null = got.null;
            if (!c.null) c.body = sc_pglib::new_cstring(answer.body, c.error);
        } catch (const std::exception &problem) {
            c.error.set(ERRCODE_INTERNAL_ERROR, ERROR, "sc_oneweb(%s): %s", c.name, problem.what());
        } catch (...) {
            c.error.set(ERRCODE_INTERNAL_ERROR, ERROR, "sc_oneweb(%s): unexpected C++ exception", c.name);
        }
    }

    // sc_oneweb_config()'s C++ part: the settings as JSON text, or an error.
    char *described(const char *path, const char *database, sc_pglib::pending_error &error) noexcept {
        try {
            const auto s = settings_of(path, database, error);
            if (!s) return nullptr;
            nlohmann::ordered_json out{
                {"database", database},
                {"config_file", path},
                {"section", s->section},
                {"url", s->url},
                {"key_from", s->key_file.empty() ? "key" : "key_file"},
                {"key_file", s->key_file.empty() ? nlohmann::ordered_json() : nlohmann::ordered_json(s->key_file)},
                {"key_bytes", s->key.size()},
                {"timeout_ms", s->timeout_ms},
                {"connect_timeout_ms", s->connect_timeout_ms},
                {"max_response_bytes", s->max_response_bytes},
                {"ca_file", s->ca_file.empty() ? nlohmann::ordered_json() : nlohmann::ordered_json(s->ca_file)},
                {"on_error", s->warn ? "warning" : "error"},
            };
            return sc_pglib::new_cstring(out.dump(), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INTERNAL_ERROR, ERROR, "sc_oneweb_config: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INTERNAL_ERROR, ERROR, "sc_oneweb_config: unexpected C++ exception");
        }
        return nullptr;
    }

    const char *path_setting() { return config_path && config_path[0] ? config_path : "/etc/sc-pglib/oneweb.ini"; }
}

namespace sc_pglib {
    // Called from _PG_init.
    void define_oneweb_settings() {
        DefineCustomStringVariable("sc_pglib.oneweb_config", "The settings file of sc_oneweb().",
                                   "Where the url and key 1web is called with come from, a section per database.",
                                   &config_path, "/etc/sc-pglib/oneweb.ini", PGC_SUSET, 0, nullptr, nullptr, nullptr);
    }
}

// sc_oneweb(name text, params jsonb = '{}', timeout_ms integer = NULL) -> jsonb
extern "C" Datum sc_oneweb(PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("sc_oneweb: the name can't be NULL")));
    call c{};
    c.name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1)) {
        Jsonb *params = PG_GETARG_JSONB_P(1);
        c.params = JsonbToCString(nullptr, &params->root, VARSIZE(params));
    } else {
        c.params = "{}";
    }
    c.timeout_ms = 0;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2)) {
        const int32 timeout = PG_GETARG_INT32(2);
        if (timeout < 1000 || timeout > 600000)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_oneweb: timeout_ms must be 1000 to 600000, not %d", timeout)));
        c.timeout_ms = timeout;
    }
    c.database = get_database_name(MyDatabaseId);
    c.role = GetUserNameFromId(GetUserId(), false);
    c.session_role = GetUserNameFromId(GetSessionUserId(), false);
    c.path = pstrdup(path_setting());

    run(c);

    if (c.status || c.milliseconds) elog(DEBUG1, "sc_oneweb(%s): HTTP %ld in %ld ms", c.name, c.status, c.milliseconds);
    if (c.cancelled) CHECK_FOR_INTERRUPTS(); // the cancel or terminate itself, when PostgreSQL can take it now
    c.error.raise();
    if (c.error.any() || c.null || !c.body) PG_RETURN_NULL(); // a WARNING, or data null
    const Datum answer = DirectFunctionCall1(jsonb_in, CStringGetDatum(c.body));
    PG_RETURN_DATUM(DirectFunctionCall2(jsonb_object_field, answer, CStringGetTextDatum("data")));
}

// sc_oneweb_config() -> jsonb
extern "C" Datum sc_oneweb_config(PG_FUNCTION_ARGS) {
    const char *database = get_database_name(MyDatabaseId);
    const char *path = pstrdup(path_setting());
    sc_pglib::pending_error error;
    char *json = described(path, database, error);
    error.raise();
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(json)));
}
