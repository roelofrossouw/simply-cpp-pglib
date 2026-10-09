// Redis from SQL (sc::redis). Connection settings live in PostgreSQL's foreign server catalog,
// as dblink and postgres_fdw do it: a server of the sc_redis foreign data wrapper holds the
// Redis servers and timeouts, and an optional user mapping holds the password.
//
// Each session (backend process) keeps one client per foreign server and reuses it, so a call
// costs one Redis round trip, not a connection. Changing the server's or user mapping's options
// makes the next call reconnect with the new ones.
//
// Calls aren't transactional: a write happens at once, and stays when the transaction rolls back.
// When Redis is unavailable a call fails fast instead of waiting on every row: after a failed
// connection the server is skipped for retry_interval_ms. With on_error 'warning' (the default)
// failures are WARNINGs and the call returns false or NULL, so a trigger never breaks the
// statement that fired it; with on_error 'error' they are ERRORs.

#include <ip_endpoints.h>
#include <redis.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pg_glue.h"

extern "C" {
#include <access/reloptions.h>
#include <catalog/pg_type.h>
#include <catalog/pg_foreign_server.h>
#include <catalog/pg_user_mapping.h>
#include <commands/defrem.h>
#include <foreign/foreign.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/syscache.h>

PG_FUNCTION_INFO_V1(sc_redis_validator);
PG_FUNCTION_INFO_V1(sc_redis_set);
PG_FUNCTION_INFO_V1(sc_redis_get);
PG_FUNCTION_INFO_V1(sc_redis_del);
PG_FUNCTION_INFO_V1(sc_redis_hset);
PG_FUNCTION_INFO_V1(sc_redis_hget);
PG_FUNCTION_INFO_V1(sc_redis_hmget);
PG_FUNCTION_INFO_V1(sc_redis_hgetall);
PG_FUNCTION_INFO_V1(sc_redis_sadd);
PG_FUNCTION_INFO_V1(sc_redis_sadd_many);
PG_FUNCTION_INFO_V1(sc_redis_srem);
PG_FUNCTION_INFO_V1(sc_redis_srem_many);
PG_FUNCTION_INFO_V1(sc_redis_scard);
PG_FUNCTION_INFO_V1(sc_redis_smembers);
PG_FUNCTION_INFO_V1(sc_redis_sismember);
}

namespace {
    constexpr const char *wrapper_name = "sc_redis";

    // One foreign server's settings. Plain data: it is filled while PostgreSQL may still raise.
    struct settings {
        Oid server = InvalidOid;
        const char *name = nullptr;
        const char *servers = nullptr;
        const char *password = nullptr;
        int connect_timeout_ms = 1000;
        int timeout_ms = 1000;
        int retry_interval_ms = 10000;
        bool raise_errors = false;
    };

    bool is_server_option(const char *name) {
        for (const char *known: {"servers", "connect_timeout_ms", "timeout_ms", "retry_interval_ms", "on_error"}) {
            if (std::strcmp(name, known) == 0) return true;
        }
        return false;
    }

    // A whole number of milliseconds from minimum to an hour, or -1.
    int milliseconds(const char *text, const int minimum) {
        char *end = nullptr;
        errno = 0;
        const long value = std::strtol(text, &end, 10);
        if (errno || end == text || *end || value < minimum || value > 3600000) return -1;
        return static_cast<int>(value);
    }

    // Applies one server option to s; returns a problem with its value, or nullptr.
    const char *apply_server_option(settings &s, const char *name, const char *value) {
        if (std::strcmp(name, "servers") == 0) {
            s.servers = value;
        } else if (std::strcmp(name, "connect_timeout_ms") == 0) {
            if ((s.connect_timeout_ms = milliseconds(value, 1)) < 0) return "connect_timeout_ms must be 1 to 3600000";
        } else if (std::strcmp(name, "timeout_ms") == 0) {
            if ((s.timeout_ms = milliseconds(value, 1)) < 0) return "timeout_ms must be 1 to 3600000";
        } else if (std::strcmp(name, "retry_interval_ms") == 0) {
            if ((s.retry_interval_ms = milliseconds(value, 0)) < 0) return "retry_interval_ms must be 0 to 3600000";
        } else if (std::strcmp(name, "on_error") == 0) {
            if (std::strcmp(value, "warning") == 0) s.raise_errors = false;
            else if (std::strcmp(value, "error") == 0) s.raise_errors = true;
            else return "on_error must be 'warning' or 'error'";
        }
        return nullptr;
    }

    // Checks a servers option, which sc::ip_endpoints parses; C++, so it reports through error.
    void check_servers(const char *servers, sc_pglib::pending_error &error) noexcept {
        try {
            if (sc::ip_endpoints{servers}.empty()) {
                error.set(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE, ERROR, "servers must name at least one Redis server");
            }
        } catch (const std::exception &exception) {
            error.set(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE, ERROR, "invalid servers option: %s", exception.what());
        }
    }

    // The current user's mapping options for server, or PUBLIC's; none at all means no password.
    List *mapping_options(const Oid server) {
        HeapTuple tuple = SearchSysCache2(USERMAPPINGUSERSERVER, ObjectIdGetDatum(GetUserId()), ObjectIdGetDatum(server));
        if (!HeapTupleIsValid(tuple)) {
            tuple = SearchSysCache2(USERMAPPINGUSERSERVER, ObjectIdGetDatum(InvalidOid), ObjectIdGetDatum(server));
        }
        if (!HeapTupleIsValid(tuple)) return NIL;
        bool is_null = false;
        const Datum datum = SysCacheGetAttr(USERMAPPINGUSERSERVER, tuple, Anum_pg_user_mapping_umoptions, &is_null);
        List *options = is_null ? NIL : untransformRelOptions(datum);
        ReleaseSysCache(tuple);
        return options;
    }

    // The settings of the named sc_redis server, checking the current user may use it.
    settings settings_for(text *name_argument) {
        const char *name = text_to_cstring(name_argument);
        ForeignServer *server = GetForeignServerByName(name, false);
        if (std::strcmp(GetForeignDataWrapper(server->fdwid)->fdwname, wrapper_name) != 0) {
            ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                            errmsg("server \"%s\" is not a %s server", name, wrapper_name)));
        }
#if PG_VERSION_NUM >= 160000
        const AclResult access = object_aclcheck(ForeignServerRelationId, server->serverid, GetUserId(), ACL_USAGE);
#else
        const AclResult access = pg_foreign_server_aclcheck(server->serverid, GetUserId(), ACL_USAGE);
#endif
        if (access != ACLCHECK_OK) aclcheck_error(access, OBJECT_FOREIGN_SERVER, server->servername);

        settings s;
        s.server = server->serverid;
        s.name = server->servername;
        ListCell *cell;
        foreach(cell, server->options) {
            const auto *option = static_cast<DefElem *>(lfirst(cell));
            apply_server_option(s, option->defname, defGetString(const_cast<DefElem *>(option)));
        }
        foreach(cell, mapping_options(server->serverid)) {
            const auto *option = static_cast<DefElem *>(lfirst(cell));
            if (std::strcmp(option->defname, "password") == 0) s.password = defGetString(const_cast<DefElem *>(option));
        }
        if (!s.servers) {
            ereport(ERROR, (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                            errmsg("server \"%s\" has no servers option", name)));
        }
        return s;
    }

    // The session's client for one foreign server.
    struct client_entry {
        std::string signature; // the settings it was made with
        std::unique_ptr<sc::redis> client;
        std::chrono::steady_clock::time_point retry_at{}; // after an outage: not tried before this
    };

    std::unordered_map<Oid, client_entry> &clients() {
        static std::unordered_map<Oid, client_entry> per_server;
        return per_server;
    }

    // How a call went, as plain data that outlives the call's C++ objects.
    struct outcome {
        bool done = false;  // the command ran
        bool found = false; // get/hget: there was a value
        long long count = 0;
        struct varlena *value = nullptr;
        char *json = nullptr; // hmget/hgetall: JSON text for jsonb_in
        Datum *items = nullptr; // smembers: text values for a text[]
        int item_count = 0;
        sc_pglib::pending_error error;
    };

    // The elements of a text[] argument. Plain data, filled while PostgreSQL may still raise.
    struct text_list {
        const char **data = nullptr;
        int *lengths = nullptr;
        int count = 0;
    };

    text_list texts_from(ArrayType *array, const char *what) {
        Datum *elements = nullptr;
        bool *nulls = nullptr;
        text_list list;
        deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT, &elements, &nulls, &list.count);
        list.data = static_cast<const char **>(palloc(sizeof(char *) * (list.count + 1)));
        list.lengths = static_cast<int *>(palloc(sizeof(int) * (list.count + 1)));
        for (int i = 0; i < list.count; ++i) {
            if (nulls[i]) {
                ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("%s must not contain NULL", what)));
            }
            const struct varlena *element = pg_detoast_datum_packed(reinterpret_cast<struct varlena *>(DatumGetPointer(elements[i])));
            list.data[i] = VARDATA_ANY(element);
            list.lengths[i] = static_cast<int>(VARSIZE_ANY_EXHDR(element));
        }
        return list;
    }

    // Appends text as a JSON string. Redis values are bytes; jsonb_in later rejects anything
    // that isn't valid in the database encoding.
    void append_json_string(std::string &json, const std::string_view text) {
        json += '"';
        for (const char c: text) {
            switch (c) {
                case '"': json += "\\\""; break;
                case '\\': json += "\\\\"; break;
                case '\n': json += "\\n"; break;
                case '\r': json += "\\r"; break;
                case '\t': json += "\\t"; break;
                case '\b': json += "\\b"; break;
                case '\f': json += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char escaped[8];
                        snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned char>(c));
                        json += escaped;
                    } else {
                        json += c;
                    }
            }
        }
        json += '"';
    }

    std::vector<std::string> strings_from(const text_list &list) {
        std::vector<std::string> strings;
        strings.reserve(static_cast<std::size_t>(list.count));
        for (int i = 0; i < list.count; ++i) strings.emplace_back(list.data[i], static_cast<std::size_t>(list.lengths[i]));
        return strings;
    }

    // Runs command (sc::redis &, outcome &) on s's client, connecting first if needed.
    template<typename Command>
    outcome run(const settings &s, Command command) noexcept {
        outcome result;
        const int failure_level = s.raise_errors ? ERROR : WARNING;
        client_entry *entry = nullptr;
        try {
            entry = &clients()[s.server];
            const std::string signature = std::string{s.servers} + '\n' + (s.password ? s.password : "") + '\n' +
                                          std::to_string(s.connect_timeout_ms) + ' ' + std::to_string(s.timeout_ms);
            if (entry->signature != signature) *entry = client_entry{signature, nullptr, {}};

            if (!entry->client) {
                if (std::chrono::steady_clock::now() < entry->retry_at) {
                    // Still pausing after an outage, which was reported then: skip it quietly
                    // rather than wait on Redis again (unless errors are wanted).
                    if (s.raise_errors) {
                        result.error.set(ERRCODE_CONNECTION_FAILURE, ERROR,
                                         "Redis server \"%s\" is unavailable (not retried yet)", s.name);
                    }
                    return result;
                }
                sc::redis_options options;
                options.password = s.password ? s.password : "";
                options.connect_timeout = std::chrono::milliseconds{s.connect_timeout_ms};
                options.command_timeout = std::chrono::milliseconds{s.timeout_ms};
                entry->client = std::make_unique<sc::redis>(sc::ip_endpoints{s.servers}, options);
            }
            command(*entry->client, result);
            result.done = true;
        } catch (const sc::redis_unavailable &exception) {
            if (entry) {
                entry->client.reset();
                entry->retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{s.retry_interval_ms};
            }
            result.error.set(ERRCODE_CONNECTION_FAILURE, failure_level,
                             "Redis server \"%s\" is unavailable, skipping it for %d ms: %s",
                             s.name, s.retry_interval_ms, exception.what());
        } catch (const std::exception &exception) {
            result.error.set(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, failure_level, "Redis server \"%s\": %s",
                             s.name, exception.what());
        } catch (...) {
            result.error.set(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, failure_level,
                             "Redis server \"%s\": unexpected C++ exception", s.name);
        }
        return result;
    }
}

// sc_redis_validator(text[], oid): checks options as they are set on the sc_redis wrapper's
// servers (servers, connect_timeout_ms, timeout_ms, retry_interval_ms, on_error) and user
// mappings (password).
extern "C" Datum sc_redis_validator(PG_FUNCTION_ARGS) {
    List *options = untransformRelOptions(PG_GETARG_DATUM(0));
    const Oid catalog = PG_GETARG_OID(1);
    settings s;
    bool has_servers = false;
    ListCell *cell;
    foreach(cell, options) {
        auto *option = static_cast<DefElem *>(lfirst(cell));
        const char *value = defGetString(option);
        if (catalog == ForeignServerRelationId) {
            if (!is_server_option(option->defname)) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                                errmsg("invalid option \"%s\" for a %s server", option->defname, wrapper_name),
                                errhint("Valid options are servers, connect_timeout_ms, timeout_ms, "
                                        "retry_interval_ms and on_error.")));
            }
            if (const char *problem = apply_server_option(s, option->defname, value)) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE), errmsg("%s", problem)));
            }
            if (std::strcmp(option->defname, "servers") == 0) {
                has_servers = true;
                sc_pglib::pending_error error;
                check_servers(value, error);
                error.raise();
            }
        } else if (catalog == UserMappingRelationId) {
            if (std::strcmp(option->defname, "password") != 0) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                                errmsg("invalid option \"%s\" for a %s user mapping", option->defname, wrapper_name),
                                errhint("The only valid option is password.")));
            }
        } else {
            ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                            errmsg("%s takes options only on servers and user mappings", wrapper_name)));
        }
    }
    if (catalog == ForeignServerRelationId && !has_servers) {
        ereport(ERROR, (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                        errmsg("a %s server needs the servers option", wrapper_name),
                        errhint("For example: OPTIONS (servers 'redis1;redis2:6380')")));
    }
    PG_RETURN_VOID();
}

// sc_redis_set(server, key, value) -> boolean: whether it was stored
extern "C" Datum sc_redis_set(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    // Fetched (and detoasted, which may allocate) before the C++ part.
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const std::string_view value = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(2));
    const outcome result = run(s, [key, value](sc::redis &redis, outcome &) {
        redis.set(std::string{key}, std::string{value});
    });
    result.error.raise();
    PG_RETURN_BOOL(result.done);
}

// sc_redis_get(server, key) -> text, or NULL when there is no such key (or it failed)
extern "C" Datum sc_redis_get(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const outcome result = run(s, [key](sc::redis &redis, outcome &out) {
        if (const auto value = redis.get(std::string{key})) {
            out.found = true;
            out.value = sc_pglib::new_varlena(*value, out.error);
        }
    });
    result.error.raise();
    if (!result.found || !result.value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(result.value);
}

// sc_redis_del(server, key) -> bigint: keys removed (0 or 1), or NULL when it failed
extern "C" Datum sc_redis_del(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const outcome result = run(s, [key](sc::redis &redis, outcome &out) {
        out.count = static_cast<long long>(redis.erase(std::string{key}));
    });
    result.error.raise();
    if (!result.done) PG_RETURN_NULL();
    PG_RETURN_INT64(result.count);
}

// sc_redis_hset(server, key, field, value) -> boolean: whether it was stored
extern "C" Datum sc_redis_hset(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const std::string_view field = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(2));
    const std::string_view value = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(3));
    const outcome result = run(s, [key, field, value](sc::redis &redis, outcome &) {
        redis.hset(std::string{key}, std::string{field}, std::string{value});
    });
    result.error.raise();
    PG_RETURN_BOOL(result.done);
}

// sc_redis_hget(server, key, field) -> text, or NULL when there is no such field (or it failed)
extern "C" Datum sc_redis_hget(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const std::string_view field = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(2));
    const outcome result = run(s, [key, field](sc::redis &redis, outcome &out) {
        if (const auto value = redis.hget(std::string{key}, std::string{field})) {
            out.found = true;
            out.value = sc_pglib::new_varlena(*value, out.error);
        }
    });
    result.error.raise();
    if (!result.found || !result.value) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(result.value);
}

// sc_redis_hmget(server, key, fields text[]) -> jsonb: {"field": "value" or null, ...} for the
// requested fields (null where there's no such field), or NULL when it failed
extern "C" Datum sc_redis_hmget(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const text_list fields = texts_from(PG_GETARG_ARRAYTYPE_P(2), "fields");
    const outcome result = run(s, [key, fields](sc::redis &redis, outcome &out) {
        const auto names = strings_from(fields);
        const auto values = redis.hmget(std::string{key}, names);
        std::string json = "{";
        for (std::size_t i = 0; i < names.size() && i < values.size(); ++i) {
            if (i) json += ", ";
            append_json_string(json, names[i]);
            json += ": ";
            if (values[i]) append_json_string(json, *values[i]);
            else json += "null";
        }
        json += '}';
        out.json = sc_pglib::new_cstring(json, out.error);
    });
    result.error.raise();
    if (!result.done || !result.json) PG_RETURN_NULL();
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(result.json)));
}

// sc_redis_hgetall(server, key) -> jsonb: every field of the hash ({} when there's no such key),
// or NULL when it failed
extern "C" Datum sc_redis_hgetall(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const outcome result = run(s, [key](sc::redis &redis, outcome &out) {
        std::string json = "{";
        bool first = true;
        for (const auto &[field, value]: redis.hgetall(std::string{key})) {
            if (!first) json += ", ";
            first = false;
            append_json_string(json, field);
            json += ": ";
            append_json_string(json, value);
        }
        json += '}';
        out.json = sc_pglib::new_cstring(json, out.error);
    });
    result.error.raise();
    if (!result.done || !result.json) PG_RETURN_NULL();
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(result.json)));
}

namespace {
    // A bigint count, or NULL when the call failed.
    Datum count_result(const outcome &result, FunctionCallInfo fcinfo) {
        result.error.raise();
        if (!result.done) PG_RETURN_NULL();
        PG_RETURN_INT64(result.count);
    }
}

// sc_redis_sadd(server, key, member) -> bigint: members added (0 when it was there already),
// or NULL when it failed
extern "C" Datum sc_redis_sadd(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const std::string_view member = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(2));
    return count_result(run(s, [key, member](sc::redis &redis, outcome &out) {
        out.count = static_cast<long long>(redis.sadd(std::string{key}, std::string{member}));
    }), fcinfo);
}

// sc_redis_sadd(server, key, members text[]) -> bigint: members added, or NULL when it failed
extern "C" Datum sc_redis_sadd_many(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const text_list members = texts_from(PG_GETARG_ARRAYTYPE_P(2), "members");
    return count_result(run(s, [key, members](sc::redis &redis, outcome &out) {
        out.count = static_cast<long long>(redis.sadd(std::string{key}, strings_from(members)));
    }), fcinfo);
}

// sc_redis_srem(server, key, member) -> bigint: members removed (0 when it wasn't there), or NULL
// when it failed
extern "C" Datum sc_redis_srem(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const std::string_view member = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(2));
    return count_result(run(s, [key, member](sc::redis &redis, outcome &out) {
        out.count = static_cast<long long>(redis.srem(std::string{key}, std::string{member}));
    }), fcinfo);
}

// sc_redis_srem(server, key, members text[]) -> bigint: members removed, or NULL when it failed
extern "C" Datum sc_redis_srem_many(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const text_list members = texts_from(PG_GETARG_ARRAYTYPE_P(2), "members");
    return count_result(run(s, [key, members](sc::redis &redis, outcome &out) {
        out.count = static_cast<long long>(redis.srem(std::string{key}, strings_from(members)));
    }), fcinfo);
}

// sc_redis_scard(server, key) -> bigint: the set's size (0 when there's no such key), or NULL
// when it failed
extern "C" Datum sc_redis_scard(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    return count_result(run(s, [key](sc::redis &redis, outcome &out) {
        out.count = static_cast<long long>(redis.scard(std::string{key}));
    }), fcinfo);
}

// sc_redis_smembers(server, key) -> text[]: the members, sorted ({} when there's no such key),
// or NULL when it failed
extern "C" Datum sc_redis_smembers(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const outcome result = run(s, [key](sc::redis &redis, outcome &out) {
        const auto members = redis.smembers(std::string{key});
        if (members.empty()) return;
        const Size size = sizeof(Datum) * members.size();
        if (!AllocSizeIsValid(size)) {
            out.error.set(ERRCODE_PROGRAM_LIMIT_EXCEEDED, ERROR, "too many set members");
            return;
        }
        auto *items = static_cast<Datum *>(MemoryContextAllocExtended(CurrentMemoryContext, size, MCXT_ALLOC_NO_OOM));
        if (!items) {
            out.error.set(ERRCODE_OUT_OF_MEMORY, ERROR, "out of memory");
            return;
        }
        int count = 0;
        for (const auto &member: members) {
            struct varlena *value = sc_pglib::new_varlena(member, out.error);
            if (!value) return;
            items[count++] = PointerGetDatum(value);
        }
        out.items = items;
        out.item_count = count;
    });
    result.error.raise();
    if (!result.done) PG_RETURN_NULL();
    if (result.item_count == 0) PG_RETURN_ARRAYTYPE_P(construct_empty_array(TEXTOID));
    PG_RETURN_ARRAYTYPE_P(construct_array(result.items, result.item_count, TEXTOID, -1, false, TYPALIGN_INT));
}

// sc_redis_sismember(server, key, member) -> boolean: whether member is in the set, or NULL when
// it failed
extern "C" Datum sc_redis_sismember(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const std::string_view key = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(1));
    const std::string_view member = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(2));
    const outcome result = run(s, [key, member](sc::redis &redis, outcome &out) {
        out.found = redis.sismember(std::string{key}, std::string{member});
    });
    result.error.raise();
    if (!result.done) PG_RETURN_NULL();
    PG_RETURN_BOOL(result.found);
}
