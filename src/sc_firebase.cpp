// A Firebase Realtime Database from SQL (sc::firebase). Settings live in PostgreSQL's foreign
// server catalog, as for sc_redis: a server of the sc_firebase foreign data wrapper holds the
// database's URL, a root path its calls work under, the timeout and on_error; a user mapping
// holds the service account's key, as JSON (service_account) or as a file the server reads
// (credentials_file). PostgreSQL shows a user mapping's options only to its owner and superusers.
//
// Each session keeps one client per foreign server, so the access token (an hour's worth) is
// reused across calls; changing the server's or mapping's options makes the next call start over.
//
// Calls aren't transactional: a write happens at once, and stays when the transaction rolls back.
// With on_error 'error' (the default) a failure is an ERROR; with 'warning' it's a WARNING and the
// call returns NULL (or false).

#include <sc_firebase.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "pg_glue.h"

extern "C" {
#include <access/reloptions.h>
#include <catalog/pg_foreign_server.h>
#include <catalog/pg_type.h>
#include <catalog/pg_user_mapping.h>
#include <commands/defrem.h>
#include <foreign/foreign.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/array.h>
#include <utils/builtins.h>
#include <utils/jsonb.h>
#include <utils/syscache.h>

PG_FUNCTION_INFO_V1(sc_firebase_validator);
PG_FUNCTION_INFO_V1(sc_firebase_get);
PG_FUNCTION_INFO_V1(sc_firebase_keys);
PG_FUNCTION_INFO_V1(sc_firebase_set);
PG_FUNCTION_INFO_V1(sc_firebase_update);
PG_FUNCTION_INFO_V1(sc_firebase_push);
PG_FUNCTION_INFO_V1(sc_firebase_remove);
PG_FUNCTION_INFO_V1(sc_firebase_move);
}

namespace {
    constexpr const char *wrapper_name = "sc_firebase";
    constexpr const char *unknown_option = "unknown option";

    // One foreign server's settings. Plain data: it is filled while PostgreSQL may still raise.
    struct settings {
        Oid server = InvalidOid;
        const char *name = nullptr;
        const char *url = nullptr;
        const char *root = "";
        const char *service_account = nullptr;  // the key's JSON (user mapping)
        const char *credentials_file = nullptr; // or a file holding it (user mapping)
        int timeout_ms = 30000;
        bool raise_errors = true;
    };

    // Applies one server option to s; returns unknown_option, a problem with its value, or nullptr.
    const char *apply_server_option(settings &s, const char *name, const char *value) {
        if (std::strcmp(name, "url") == 0) {
            if (std::strncmp(value, "https://", 8) != 0) return "url must start with https://";
            s.url = value;
        } else if (std::strcmp(name, "root") == 0) {
            s.root = value;
        } else if (std::strcmp(name, "timeout_ms") == 0) {
            char *end = nullptr;
            const long ms = std::strtol(value, &end, 10);
            if (end == value || *end || ms < 1000 || ms > 600000) return "timeout_ms must be 1000 to 600000";
            s.timeout_ms = static_cast<int>(ms);
        } else if (std::strcmp(name, "on_error") == 0) {
            if (std::strcmp(value, "warning") == 0) s.raise_errors = false;
            else if (std::strcmp(value, "error") == 0) s.raise_errors = true;
            else return "on_error must be 'error' or 'warning'";
        } else {
            return unknown_option;
        }
        return nullptr;
    }

    // The current user's mapping options for server, or PUBLIC's.
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

    // The settings of the named sc_firebase server, checking the current user may use it.
    settings settings_for(text *name_argument) {
        const char *name = text_to_cstring(name_argument);
        ForeignServer *server = GetForeignServerByName(name, false);
        if (std::strcmp(GetForeignDataWrapper(server->fdwid)->fdwname, wrapper_name) != 0) {
            ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("server \"%s\" is not a %s server", name, wrapper_name)));
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
            if (std::strcmp(option->defname, "service_account") == 0) s.service_account = defGetString(const_cast<DefElem *>(option));
            else if (std::strcmp(option->defname, "credentials_file") == 0) s.credentials_file = defGetString(const_cast<DefElem *>(option));
        }
        if (!s.url) {
            ereport(ERROR, (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND), errmsg("server \"%s\" has no url option", name)));
        }
        if (!s.service_account && !s.credentials_file) {
            ereport(ERROR, (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                            errmsg("no user mapping with service_account or credentials_file for server \"%s\"", name),
                            errhint("CREATE USER MAPPING FOR CURRENT_USER SERVER %s OPTIONS (credentials_file '/etc/...json')", name)));
        }
        return s;
    }

    struct client_entry {
        std::string signature; // the settings it was made with
        std::unique_ptr<sc::firebase::database> database;
    };

    std::unordered_map<Oid, client_entry> &clients() {
        static std::unordered_map<Oid, client_entry> per_server;
        return per_server;
    }

    // How a call went, as plain data that outlives the call's C++ objects.
    struct outcome {
        bool done = false;
        bool flag = false;     // move: moved
        char *text = nullptr;  // get: JSON for jsonb_in; push: the key
        Datum *items = nullptr; // keys
        int item_count = 0;
        sc_pglib::pending_error error;
    };

    // root and path joined: the path a call means.
    std::string full_path(const settings &s, const char *path) {
        return std::string{s.root} + "/" + path;
    }

    // Runs command (const sc::firebase::database &, outcome &) on s's client, making it first if needed.
    template<typename Command>
    outcome run(const settings &s, Command command) noexcept {
        outcome result;
        const int failure_level = s.raise_errors ? ERROR : WARNING;
        try {
            auto &entry = clients()[s.server];
            const std::string signature = std::string{s.url} + '\n' + (s.service_account ? s.service_account : "") + '\n' +
                                          (s.credentials_file ? s.credentials_file : "") + '\n' + std::to_string(s.timeout_ms);
            if (entry.signature != signature || !entry.database) {
                entry = client_entry{};
                const auto account = s.service_account ? sc::firebase::service_account::parse(s.service_account)
                                                       : sc::firebase::service_account::load(s.credentials_file);
                entry.database = std::make_unique<sc::firebase::database>(s.url, account);
                entry.database->timeout((s.timeout_ms + 999) / 1000);
                entry.signature = signature;
            }
            command(*entry.database, result);
            result.done = !result.error.any();
        } catch (const sc::firebase::error &exception) {
            result.error.set(exception.status() == 0 ? ERRCODE_CONNECTION_FAILURE : ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, failure_level,
                             "Firebase server \"%s\": %s", s.name, exception.what());
        } catch (const std::exception &exception) {
            result.error.set(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, failure_level, "Firebase server \"%s\": %s", s.name, exception.what());
        } catch (...) {
            result.error.set(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION, failure_level, "Firebase server \"%s\": unexpected C++ exception", s.name);
        }
        return result;
    }

    // A jsonb argument as JSON text (PostgreSQL may raise, so before the C++ part).
    const char *json_text(Jsonb *value) { return JsonbToCString(nullptr, &value->root, VARSIZE(value)); }

    Datum done_result(const outcome &result, FunctionCallInfo fcinfo) {
        result.error.raise();
        PG_RETURN_BOOL(result.done);
    }
}

// sc_firebase_validator(text[], oid): checks options as they are set on the sc_firebase wrapper's
// servers (url, root, timeout_ms, on_error) and user mappings (service_account, credentials_file).
extern "C" Datum sc_firebase_validator(PG_FUNCTION_ARGS) {
    List *options = untransformRelOptions(PG_GETARG_DATUM(0));
    const Oid catalog = PG_GETARG_OID(1);
    settings s;
    bool has_url = false, has_key = false;
    ListCell *cell;
    foreach(cell, options) {
        auto *option = static_cast<DefElem *>(lfirst(cell));
        const char *value = defGetString(option);
        if (catalog == ForeignServerRelationId) {
            const char *problem = apply_server_option(s, option->defname, value);
            if (problem == unknown_option) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                                errmsg("invalid option \"%s\" for a %s server", option->defname, wrapper_name),
                                errhint("Valid options are url, root, timeout_ms and on_error.")));
            }
            if (problem) ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE), errmsg("%s", problem)));
            if (std::strcmp(option->defname, "url") == 0) has_url = true;
        } else if (catalog == UserMappingRelationId) {
            if (std::strcmp(option->defname, "service_account") != 0 && std::strcmp(option->defname, "credentials_file") != 0) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                                errmsg("invalid option \"%s\" for a %s user mapping", option->defname, wrapper_name),
                                errhint("Valid options are service_account (the key's JSON) and credentials_file.")));
            }
            if (has_key) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
                                errmsg("a %s user mapping takes service_account or credentials_file, not both", wrapper_name)));
            }
            has_key = true;
        } else {
            ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                            errmsg("%s takes options only on servers and user mappings", wrapper_name)));
        }
    }
    if (catalog == ForeignServerRelationId && !has_url) {
        ereport(ERROR, (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND), errmsg("a %s server needs the url option", wrapper_name),
                        errhint("For example: OPTIONS (url 'https://example-app.firebaseio.com', root 'devices')")));
    }
    PG_RETURN_VOID();
}

// sc_firebase_get(server, path, shallow) -> jsonb: NULL when there's nothing (or it failed)
extern "C" Datum sc_firebase_get(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const bool shallow = PG_GETARG_BOOL(2);
    const outcome result = run(s, [&](const sc::firebase::database &db, outcome &out) {
        const auto value = db.get(full_path(s, path), shallow);
        if (!value.is_null()) out.text = sc_pglib::new_cstring(value.dump(), out.error);
    });
    result.error.raise();
    if (!result.text) PG_RETURN_NULL();
    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(result.text)));
}

// sc_firebase_keys(server, path) -> text[]: the child keys, sorted; NULL when it failed
extern "C" Datum sc_firebase_keys(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const outcome result = run(s, [&](const sc::firebase::database &db, outcome &out) {
        const auto keys = db.keys(full_path(s, path));
        if (keys.empty()) return;
        auto *items = static_cast<Datum *>(sc_pglib::alloc_no_oom(sizeof(Datum) * keys.size(), out.error));
        if (!items) return;
        int count = 0;
        for (const auto &key: keys) {
            struct varlena *value = sc_pglib::new_varlena(key, out.error);
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

// sc_firebase_set(server, path, value jsonb) -> boolean: false when it failed
extern "C" Datum sc_firebase_set(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *value = json_text(PG_GETARG_JSONB_P(2));
    return done_result(run(s, [&](const sc::firebase::database &db, outcome &) {
        db.set(full_path(s, path), sc::firebase::json::parse(value));
    }), fcinfo);
}

// sc_firebase_update(server, path, values jsonb) -> boolean: each member written under path (a
// member's name may be a path; null removes), all at once; false when it failed
extern "C" Datum sc_firebase_update(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *values = json_text(PG_GETARG_JSONB_P(2));
    return done_result(run(s, [&](const sc::firebase::database &db, outcome &) {
        db.update(full_path(s, path), sc::firebase::json::parse(values));
    }), fcinfo);
}

// sc_firebase_push(server, path, value jsonb) -> text: the new child's key; NULL when it failed
extern "C" Datum sc_firebase_push(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *value = json_text(PG_GETARG_JSONB_P(2));
    const outcome result = run(s, [&](const sc::firebase::database &db, outcome &out) {
        out.text = sc_pglib::new_cstring(db.push(full_path(s, path), sc::firebase::json::parse(value)), out.error);
    });
    result.error.raise();
    if (!result.text) PG_RETURN_NULL();
    PG_RETURN_TEXT_P(cstring_to_text(result.text));
}

// sc_firebase_remove(server, path) -> boolean: false when it failed
extern "C" Datum sc_firebase_remove(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(1));
    return done_result(run(s, [&](const sc::firebase::database &db, outcome &) {
        db.remove(full_path(s, path));
    }), fcinfo);
}

// sc_firebase_move(server, from, to, overwrite) -> boolean: moved, in one update; false when there
// was nothing to move or (without overwrite) the target was taken; NULL when it failed
extern "C" Datum sc_firebase_move(PG_FUNCTION_ARGS) {
    const settings s = settings_for(PG_GETARG_TEXT_PP(0));
    const char *from = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *to = text_to_cstring(PG_GETARG_TEXT_PP(2));
    const bool overwrite = PG_GETARG_BOOL(3);
    const outcome result = run(s, [&](const sc::firebase::database &db, outcome &out) {
        out.flag = db.move(full_path(s, from), full_path(s, to), overwrite);
    });
    result.error.raise();
    if (!result.done) PG_RETURN_NULL();
    PG_RETURN_BOOL(result.flag);
}
