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
// call returns NULL (or false), a foreign table no rows.
//
// The wrapper's handler also makes foreign tables of it, read and written: see the end of the file.

#include <sc_firebase.h>

#include "firebase_rows.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pg_glue.h"

extern "C" {
#include <access/reloptions.h>
#include <catalog/pg_attribute.h>
#include <catalog/pg_foreign_server.h>
#include <catalog/pg_foreign_table.h>
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
#include <commands/explain.h>
#if PG_VERSION_NUM >= 180000
#include <commands/explain_format.h>
#endif
#include <executor/executor.h>
#include <foreign/fdwapi.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/appendinfo.h>
#include <optimizer/clauses.h>
#include <optimizer/inherit.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/planmain.h>
#include <optimizer/restrictinfo.h>
#include <utils/lsyscache.h>
#include <utils/json.h>
#include <utils/rel.h>

PG_FUNCTION_INFO_V1(sc_firebase_fdw_handler);
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

    // The settings of an sc_firebase server, checking the current user may use it.
    settings settings_for_server(ForeignServer *server) {
        const char *name = server->servername;
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

    // The settings of the named sc_firebase server.
    settings settings_for(text *name_argument) {
        return settings_for_server(GetForeignServerByName(text_to_cstring(name_argument), false));
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

    Datum done_result(const outcome &result, [[maybe_unused]] FunctionCallInfo fcinfo) {
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
        } else if (catalog == ForeignTableRelationId) {
            if (std::strcmp(option->defname, "path") != 0 && std::strcmp(option->defname, "keys") != 0) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                                errmsg("invalid option \"%s\" for a %s foreign table", option->defname, wrapper_name),
                                errhint("Valid options are path (under the server's root) and keys (the key columns, a level each).")));
            }
        } else if (catalog == AttributeRelationId) {
            if (std::strcmp(option->defname, "field") != 0) {
                ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                                errmsg("invalid option \"%s\" for a %s foreign table's column", option->defname, wrapper_name),
                                errhint("The only valid option is field (the node's field, or a path into it, if not the column's name).")));
            }
        } else {
            ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
                            errmsg("%s takes options only on servers, user mappings, foreign tables and their columns", wrapper_name)));
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

// ---------------------------------------------------------------------------------------------
// Foreign tables. A table maps a path under the server's root and the key
// levels under it to rows (see firebase_rows.h):
//
//   CREATE FOREIGN TABLE device (account_id text, imei text, lat float8, online int, reg_nr text)
//       SERVER app_db OPTIONS (path '', keys 'account_id,imei');
//
// A WHERE that sets the leading key columns with = (a constant or a parameter) fetches only that
// part of the tree; PostgreSQL checks every condition again on the rows, so the others filter as
// usual. Columns that aren't keys are the node's fields of the same name, or the column option
// field ('gps/lat'). json and jsonb columns get the field as JSON; an empty string is NULL in a
// column that isn't text-like (the device records hold "" for unknown numbers).

namespace {
    struct table_options {
        const char *path = "";
        const char *keys = "";
    };

    table_options options_of(const Oid relid) {
        table_options t;
        ListCell *cell;
        foreach(cell, GetForeignTable(relid)->options) {
            auto *option = static_cast<DefElem *>(lfirst(cell));
            if (std::strcmp(option->defname, "path") == 0) t.path = defGetString(option);
            else if (std::strcmp(option->defname, "keys") == 0) t.keys = defGetString(option);
        }
        return t;
    }

    // 'a, b' as palloc'd names; their count.
    int split_keys(const char *keys, char ***names) {
        char *copy = pstrdup(keys);
        int count = 0;
        *names = static_cast<char **>(palloc(sizeof(char *) * (std::strlen(keys) / 2 + 2)));
        char *save = nullptr;
        for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(nullptr, ",", &save)) {
            while (*item == ' ' || *item == '\t') ++item;
            char *end = item + std::strlen(item);
            while (end > item && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
            if (*item) (*names)[count++] = item;
        }
        return count;
    }

    // The key columns' attribute numbers, in level order; an ERROR for a key that isn't a column.
    AttrNumber *key_attnums(const Oid relid, char **names, const int count) {
        auto *attnums = static_cast<AttrNumber *>(palloc(sizeof(AttrNumber) * (count + 1)));
        for (int i = 0; i < count; ++i) {
            attnums[i] = get_attnum(relid, names[i]);
            if (attnums[i] == InvalidAttrNumber) {
                ereport(ERROR, (errcode(ERRCODE_FDW_COLUMN_NAME_NOT_FOUND),
                                errmsg("keys names \"%s\", which isn't a column of \"%s\"", names[i], get_rel_name(relid))));
            }
        }
        return attnums;
    }

    Node *without_relabel(Node *node) {
        while (node && IsA(node, RelabelType)) node = reinterpret_cast<Node *>(reinterpret_cast<RelabelType *>(node)->arg);
        return node;
    }

    void firebase_rel_size(PlannerInfo *, RelOptInfo *baserel, Oid) {
        baserel->rows = 1000;
    }

    void firebase_paths(PlannerInfo *root, RelOptInfo *baserel, Oid) {
        add_path(baserel, reinterpret_cast<Path *>(create_foreignscan_path(root, baserel, nullptr, baserel->rows,
#if PG_VERSION_NUM >= 180000
                                                                           0,
#endif
                                                                           10, 10 + baserel->rows, NIL, baserel->lateral_relids, nullptr,
#if PG_VERSION_NUM >= 170000
                                                                           NIL,
#endif
                                                                           NIL)));
    }

    // Finds "key column = something constant for the scan" among the conditions: the somethings go
    // in fdw_exprs (evaluated when the scan starts), their key levels in fdw_private. All the
    // conditions stay for PostgreSQL to check.
    ForeignScan *firebase_plan(PlannerInfo *, RelOptInfo *baserel, const Oid relid, ForeignPath *, List *tlist, List *scan_clauses,
                               Plan *outer_plan) {
        const table_options t = options_of(relid);
        char **names = nullptr;
        const int count = split_keys(t.keys, &names);
        const AttrNumber *attnums = key_attnums(relid, names, count);
        List *exprs = NIL;
        List *pushed = NIL;
        ListCell *cell;
        foreach(cell, scan_clauses) {
            auto *info = static_cast<RestrictInfo *>(lfirst(cell));
            if (!IsA(info->clause, OpExpr)) continue;
            auto *op = reinterpret_cast<OpExpr *>(info->clause);
            if (list_length(op->args) != 2) continue;
            const char *name = get_opname(op->opno);
            if (!name || std::strcmp(name, "=") != 0) continue;
            Node *sides[2] = {without_relabel(static_cast<Node *>(linitial(op->args))), without_relabel(static_cast<Node *>(lsecond(op->args)))};
            for (int side = 0; side < 2; ++side) {
                if (!sides[side] || !IsA(sides[side], Var)) continue;
                auto *var = reinterpret_cast<Var *>(sides[side]);
                Node *other = sides[1 - side];
                if (static_cast<Index>(var->varno) != baserel->relid || var->varlevelsup != 0 || !is_pseudo_constant_clause(other)) continue;
                for (int level = 0; level < count; ++level) {
                    if (attnums[level] != var->varattno || list_member_int(pushed, level)) continue;
                    exprs = lappend(exprs, other);
                    pushed = lappend_int(pushed, level);
                }
            }
        }
        scan_clauses = extract_actual_clauses(scan_clauses, false);
        return make_foreignscan(tlist, scan_clauses, baserel->relid, exprs, list_make1(pushed), NIL, NIL, outer_plan);
    }

    // A scan's state. Plain data, in the query's memory.
    struct scan_state {
        settings s;
        const char *path = "";
        char **key_names = nullptr;
        int key_count = 0;
        int pushed_count = 0;
        int *pushed_level = nullptr;
        List *expr_states = NIL;
        Oid *expr_types = nullptr;
        int natts = 0;
        int *column_key = nullptr;      // the key level a column is, or -1
        char **column_field = nullptr;  // else its field (nullptr: a dropped column)
        bool *column_json = nullptr;
        bool *column_text_like = nullptr;
        FmgrInfo *in_functions = nullptr;
        Oid *in_ioparams = nullptr;
        int32 *typmods = nullptr;
        char *explained = nullptr;      // EXPLAIN's "Firebase path"
        bool fetched = false;
        int row_count = 0;
        char **values = nullptr;        // row_count x natts; nullptr: NULL
        int next = 0;
    };

    void firebase_begin(ForeignScanState *node, int) {
        auto *plan = reinterpret_cast<ForeignScan *>(node->ss.ps.plan);
        Relation relation = node->ss.ss_currentRelation;
        const Oid relid = RelationGetRelid(relation);
        auto *state = static_cast<scan_state *>(palloc0(sizeof(scan_state)));
        state->s = settings_for_server(GetForeignServer(GetForeignTable(relid)->serverid));
        const table_options t = options_of(relid);
        state->path = t.path;
        state->key_count = split_keys(t.keys, &state->key_names);

        TupleDesc description = RelationGetDescr(relation);
        state->natts = description->natts;
        const int n = state->natts + 1;
        state->column_key = static_cast<int *>(palloc(sizeof(int) * n));
        state->column_field = static_cast<char **>(palloc0(sizeof(char *) * n));
        state->column_json = static_cast<bool *>(palloc0(sizeof(bool) * n));
        state->column_text_like = static_cast<bool *>(palloc0(sizeof(bool) * n));
        state->in_functions = static_cast<FmgrInfo *>(palloc0(sizeof(FmgrInfo) * n));
        state->in_ioparams = static_cast<Oid *>(palloc0(sizeof(Oid) * n));
        state->typmods = static_cast<int32 *>(palloc0(sizeof(int32) * n));
        for (int i = 0; i < state->natts; ++i) {
            Form_pg_attribute attribute = TupleDescAttr(description, i);
            state->column_key[i] = -1;
            if (attribute->attisdropped) continue;
            const char *name = NameStr(attribute->attname);
            for (int level = 0; level < state->key_count; ++level)
                if (std::strcmp(state->key_names[level], name) == 0) state->column_key[i] = level;
            state->column_field[i] = pstrdup(name);
            ListCell *cell;
            foreach(cell, GetForeignColumnOptions(relid, attribute->attnum)) {
                auto *option = static_cast<DefElem *>(lfirst(cell));
                if (std::strcmp(option->defname, "field") == 0) state->column_field[i] = defGetString(option);
            }
            state->column_json[i] = attribute->atttypid == JSONOID || attribute->atttypid == JSONBOID;
            char category = 0;
            bool preferred = false;
            get_type_category_preferred(attribute->atttypid, &category, &preferred);
            state->column_text_like[i] = category == TYPCATEGORY_STRING;
            Oid input = InvalidOid;
            getTypeInputInfo(attribute->atttypid, &input, &state->in_ioparams[i]);
            fmgr_info(input, &state->in_functions[i]);
            state->typmods[i] = attribute->atttypmod;
        }

        List *pushed = static_cast<List *>(linitial(plan->fdw_private));
        state->pushed_count = list_length(pushed);
        state->pushed_level = static_cast<int *>(palloc(sizeof(int) * (state->pushed_count + 1)));
        state->expr_types = static_cast<Oid *>(palloc(sizeof(Oid) * (state->pushed_count + 1)));
        int index = 0;
        ListCell *cell;
        foreach(cell, pushed) state->pushed_level[index++] = lfirst_int(cell);
        index = 0;
        foreach(cell, plan->fdw_exprs) state->expr_types[index++] = exprType(static_cast<Node *>(lfirst(cell)));
        state->expr_states = ExecInitExprList(plan->fdw_exprs, reinterpret_cast<PlanState *>(node));

        // EXPLAIN: the path fetched, the levels a WHERE sets as {name}.
        StringInfoData text;
        initStringInfo(&text);
        appendStringInfoString(&text, state->s.root);
        if (*state->path) appendStringInfo(&text, "%s%s", text.len ? "/" : "", state->path);
        for (int level = 0; level < state->key_count; ++level) {
            bool set = false;
            for (int i = 0; i < state->pushed_count; ++i) set = set || state->pushed_level[i] == level;
            if (!set) break;
            appendStringInfo(&text, "%s{%s}", text.len ? "/" : "", state->key_names[level]);
        }
        if (text.len == 0) appendStringInfoChar(&text, '/');
        state->explained = text.data;
        node->fdw_state = state;
    }

    // Evaluates the key values and fetches the rows: in the query's memory, once per (re)scan.
    void fetch(ForeignScanState *node, scan_state *state) {
        ExprContext *context = node->ss.ps.ps_ExprContext;
        auto **known = static_cast<char **>(palloc0(sizeof(char *) * (state->key_count + 1)));
        bool impossible = false;
        int index = 0;
        ListCell *cell;
        foreach(cell, state->expr_states) {
            bool is_null = false;
            const Datum value = ExecEvalExpr(static_cast<ExprState *>(lfirst(cell)), context, &is_null);
            if (is_null) {
                impossible = true; // key = NULL matches nothing
            } else {
                Oid output = InvalidOid;
                bool varlena = false;
                getTypeOutputInfo(state->expr_types[index], &output, &varlena);
                known[state->pushed_level[index]] = OidOutputFunctionCall(output, value);
            }
            ++index;
        }
        state->fetched = true;
        state->next = 0;
        state->row_count = 0;
        if (impossible) return;

        const outcome result = run(state->s, [state, known](const sc::firebase::database &db, outcome &out) {
            namespace rows = sc_pglib::firebase_rows;
            std::vector<std::optional<std::string>> keys(static_cast<std::size_t>(state->key_count));
            for (int level = 0; level < state->key_count; ++level)
                if (known[level]) keys[static_cast<std::size_t>(level)] = std::string{known[level]};
            const auto fetched = db.get(full_path(state->s, rows::fetch_path(state->path, keys).c_str()));
            const auto found = rows::rows(fetched, keys);
            if (found.empty()) return;
            const auto cells = found.size() * static_cast<std::size_t>(state->natts);
            auto **values = static_cast<char **>(sc_pglib::alloc_no_oom(sizeof(char *) * cells, out.error));
            if (!values) return;
            std::fill(values, values + cells, nullptr);
            for (std::size_t r = 0; r < found.size(); ++r) {
                for (int c = 0; c < state->natts; ++c) {
                    char *&cell = values[r * static_cast<std::size_t>(state->natts) + static_cast<std::size_t>(c)];
                    std::optional<std::string> text;
                    if (state->column_key[c] >= 0) {
                        text = found[r].keys[static_cast<std::size_t>(state->column_key[c])];
                    } else if (state->column_field[c]) {
                        if (const auto *value = rows::field(*found[r].node, state->column_field[c]))
                            text = rows::text_of(*value, state->column_json[c]);
                        if (text && text->empty() && !state->column_text_like[c]) text.reset();
                    }
                    if (text && !(cell = sc_pglib::new_cstring(*text, out.error))) return;
                }
            }
            state->values = values;
            state->row_count = static_cast<int>(found.size());
        });
        result.error.raise();
    }

    TupleTableSlot *firebase_iterate(ForeignScanState *node) {
        auto *state = static_cast<scan_state *>(node->fdw_state);
        TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
        ExecClearTuple(slot);
        if (!state->fetched) {
            // The executor calls this in the per-tuple memory, which is reset row by row.
            const MemoryContext previous = MemoryContextSwitchTo(node->ss.ps.state->es_query_cxt);
            fetch(node, state);
            MemoryContextSwitchTo(previous);
        }
        if (state->next >= state->row_count) return slot;
        char **row = state->values + static_cast<std::size_t>(state->next++) * static_cast<std::size_t>(state->natts);
        for (int c = 0; c < state->natts; ++c) {
            if (!row[c]) {
                slot->tts_isnull[c] = true;
                slot->tts_values[c] = 0;
            } else {
                slot->tts_isnull[c] = false;
                slot->tts_values[c] = InputFunctionCall(&state->in_functions[c], row[c], state->in_ioparams[c], state->typmods[c]);
            }
        }
        ExecStoreVirtualTuple(slot);
        return slot;
    }

    void firebase_rescan(ForeignScanState *node) {
        static_cast<scan_state *>(node->fdw_state)->fetched = false;
    }

    void firebase_end(ForeignScanState *) {
    }

    void firebase_explain(ForeignScanState *node, ExplainState *es) {
        ExplainPropertyText("Firebase path", static_cast<scan_state *>(node->fdw_state)->explained, es);
    }
}


// ---------------------------------------------------------------------------------------------
// Writing through foreign tables. A statement's changes are collected and sent as one multi-path
// update of the table's path when it ends, so they're all written or none are:
//   INSERT   the row's node, replacing one with the same keys (its non-NULL columns as fields)
//   UPDATE   the columns it sets, each a field (NULL removes it); setting a key column moves the
//            node: the old one is read, the new columns applied, and it's removed where it was
//   DELETE   the node
// Key columns are the row's identity, so they may not be NULL, empty, or hold . $ # [ ] /.

namespace {
    enum class op_kind { set_node, set_fields, remove, move };

    // One row's change. Plain data, in the query's memory.
    struct write_op {
        op_kind kind = op_kind::remove;
        char *path = nullptr;          // the row's keys joined with '/'
        char *to = nullptr;            // move: the new keys
        int field_count = 0;
        char **field_paths = nullptr;  // set_node, set_fields, move: fields
        char **field_values = nullptr; // their values as JSON
    };

    struct modify_state {
        settings s;
        const char *path = "";
        char **key_names = nullptr;
        int key_count = 0;
        AttrNumber *key_attnums = nullptr;   // in the table
        AttrNumber *junk_attnums = nullptr;  // the keys' row identity in the plan's output (update, delete)
        int natts = 0;
        int *column_key = nullptr;
        char **column_field = nullptr;
        Oid *column_type = nullptr;
        bool *updated = nullptr;             // update: the columns it sets
        write_op *ops = nullptr;
        int op_count = 0;
        int op_capacity = 0;
        MemoryContext memory = nullptr;
    };

    // A row identity column's name for key level n.
    char *junk_name(const int level) { return psprintf("sc_firebase_key%d", level); }

    void firebase_update_targets(PlannerInfo *root, const Index rtindex, RangeTblEntry *, Relation relation) {
        const Oid relid = RelationGetRelid(relation);
        char **names = nullptr;
        const int count = split_keys(options_of(relid).keys, &names);
        if (count == 0) {
            ereport(ERROR, (errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
                            errmsg("foreign table \"%s\" has no keys option, so its rows can't be told apart", RelationGetRelationName(relation))));
        }
        const AttrNumber *attnums = key_attnums(relid, names, count);
        TupleDesc description = RelationGetDescr(relation);
        for (int level = 0; level < count; ++level) {
            Form_pg_attribute attribute = TupleDescAttr(description, attnums[level] - 1);
            Var *var = makeVar(rtindex, attnums[level], attribute->atttypid, attribute->atttypmod, attribute->attcollation, 0);
            add_row_identity_var(root, var, rtindex, junk_name(level));
        }
    }

    // fdw_private: the columns an UPDATE sets (an IntList of attribute numbers); NIL otherwise.
    List *firebase_plan_modify(PlannerInfo *root, ModifyTable *plan, const Index result_relation, int) {
        List *updated = NIL;
        if (plan->operation == CMD_UPDATE) {
            Bitmapset *columns = get_rel_all_updated_cols(root, find_base_rel(root, static_cast<int>(result_relation)));
            int member = -1;
            while ((member = bms_next_member(columns, member)) >= 0) {
                const AttrNumber attnum = static_cast<AttrNumber>(member + FirstLowInvalidHeapAttributeNumber);
                if (attnum > 0) updated = lappend_int(updated, attnum);
            }
        }
        return list_make1(updated);
    }

    void firebase_begin_modify(ModifyTableState *mtstate, ResultRelInfo *info, List *fdw_private, int, const int eflags) {
        if (eflags & EXEC_FLAG_EXPLAIN_ONLY) return;
        Relation relation = info->ri_RelationDesc;
        const Oid relid = RelationGetRelid(relation);
        EState *estate = mtstate->ps.state;
        const MemoryContext previous = MemoryContextSwitchTo(estate->es_query_cxt);
        auto *state = static_cast<modify_state *>(palloc0(sizeof(modify_state)));
        state->memory = estate->es_query_cxt;
        state->s = settings_for_server(GetForeignServer(GetForeignTable(relid)->serverid));
        const table_options t = options_of(relid);
        state->path = t.path;
        state->key_count = split_keys(t.keys, &state->key_names);
        state->key_attnums = key_attnums(relid, state->key_names, state->key_count);
        TupleDesc description = RelationGetDescr(relation);
        state->natts = description->natts;
        const int n = state->natts + 1;
        state->column_key = static_cast<int *>(palloc(sizeof(int) * n));
        state->column_field = static_cast<char **>(palloc0(sizeof(char *) * n));
        state->column_type = static_cast<Oid *>(palloc0(sizeof(Oid) * n));
        state->updated = static_cast<bool *>(palloc0(sizeof(bool) * n));
        for (int i = 0; i < state->natts; ++i) {
            Form_pg_attribute attribute = TupleDescAttr(description, i);
            state->column_key[i] = -1;
            if (attribute->attisdropped) continue;
            for (int level = 0; level < state->key_count; ++level)
                if (state->key_attnums[level] == attribute->attnum) state->column_key[i] = level;
            state->column_type[i] = attribute->atttypid;
            state->column_field[i] = pstrdup(NameStr(attribute->attname));
            ListCell *cell;
            foreach(cell, GetForeignColumnOptions(relid, attribute->attnum)) {
                auto *option = static_cast<DefElem *>(lfirst(cell));
                if (std::strcmp(option->defname, "field") == 0) state->column_field[i] = defGetString(option);
            }
        }
        ListCell *cell;
        foreach(cell, static_cast<List *>(linitial(fdw_private))) state->updated[lfirst_int(cell) - 1] = true;
        const CmdType operation = mtstate->operation;
        if (operation == CMD_UPDATE || operation == CMD_DELETE) {
            Plan *subplan = outerPlanState(mtstate)->plan;
            state->junk_attnums = static_cast<AttrNumber *>(palloc(sizeof(AttrNumber) * (state->key_count + 1)));
            for (int level = 0; level < state->key_count; ++level) {
                state->junk_attnums[level] = ExecFindJunkAttributeInTlist(subplan->targetlist, junk_name(level));
                if (!AttributeNumberIsValid(state->junk_attnums[level]))
                    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("sc_firebase: key %d missing from the plan", level)));
            }
        }
        info->ri_FdwState = state;
        MemoryContextSwitchTo(previous);
    }

    // A key's text, checked: Firebase keys can't be empty or hold . $ # [ ] / (or be NULL here).
    char *key_text(const modify_state *state, const int level, const Datum value, const bool is_null, const Oid type) {
        if (is_null) {
            ereport(ERROR, (errcode(ERRCODE_NOT_NULL_VIOLATION), errmsg("key column \"%s\" can't be NULL", state->key_names[level])));
        }
        Oid output = InvalidOid;
        bool varlena = false;
        getTypeOutputInfo(type, &output, &varlena);
        char *text = OidOutputFunctionCall(output, value);
        if (!*text || std::strpbrk(text, ".$#[]/")) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("key column \"%s\" holds \"%s\": Firebase keys can't be empty or hold . $ # [ ] /", state->key_names[level], text)));
        }
        return text;
    }

    // A value as JSON: numbers and booleans as such, json and jsonb as they are, the rest as strings.
    char *json_of(const Datum value, const bool is_null, const Oid type) {
        if (is_null) return pstrdup("null");
        Oid output = InvalidOid;
        bool varlena = false;
        getTypeOutputInfo(type, &output, &varlena);
        char *text = OidOutputFunctionCall(output, value);
        if (type == JSONOID || type == JSONBOID) return text;
        if (type == BOOLOID) return pstrdup(DatumGetBool(value) ? "true" : "false");
        char category = 0;
        bool preferred = false;
        get_type_category_preferred(type, &category, &preferred);
        const bool finite = std::strcmp(text, "NaN") != 0 && std::strstr(text, "Infinity") == nullptr;
        if (category == TYPCATEGORY_NUMERIC && finite) return text;
        StringInfoData quoted;
        initStringInfo(&quoted);
        escape_json(&quoted, text);
        return quoted.data;
    }

    write_op &new_op(modify_state *state) {
        if (state->op_count == state->op_capacity) {
            state->op_capacity = state->op_capacity ? state->op_capacity * 2 : 16;
            const auto size = sizeof(write_op) * static_cast<std::size_t>(state->op_capacity);
            state->ops = static_cast<write_op *>(state->ops ? repalloc(state->ops, size) : palloc(size));
        }
        write_op &op = state->ops[state->op_count++];
        op = write_op{};
        return op;
    }

    char *joined(const modify_state *state, char **keys) {
        StringInfoData path;
        initStringInfo(&path);
        for (int level = 0; level < state->key_count; ++level) appendStringInfo(&path, "%s%s", level ? "/" : "", keys[level]);
        return path.data;
    }

    // The columns of slot that aren't keys (and, when only_updated, that the UPDATE sets) as fields of op.
    void add_fields(modify_state *state, write_op &op, TupleTableSlot *slot, const bool only_updated, const bool skip_null) {
        slot_getallattrs(slot);
        op.field_paths = static_cast<char **>(palloc(sizeof(char *) * (state->natts + 1)));
        op.field_values = static_cast<char **>(palloc(sizeof(char *) * (state->natts + 1)));
        for (int c = 0; c < state->natts; ++c) {
            if (state->column_key[c] >= 0 || !state->column_field[c]) continue;
            if (only_updated && !state->updated[c]) continue;
            if (skip_null && slot->tts_isnull[c]) continue;
            op.field_paths[op.field_count] = state->column_field[c];
            op.field_values[op.field_count++] = json_of(slot->tts_values[c], slot->tts_isnull[c], state->column_type[c]);
        }
    }

    TupleTableSlot *firebase_insert(EState *estate, ResultRelInfo *info, TupleTableSlot *slot, TupleTableSlot *) {
        auto *state = static_cast<modify_state *>(info->ri_FdwState);
        const MemoryContext previous = MemoryContextSwitchTo(estate->es_query_cxt);
        slot_getallattrs(slot);
        auto **keys = static_cast<char **>(palloc(sizeof(char *) * (state->key_count + 1)));
        for (int level = 0; level < state->key_count; ++level) {
            const int c = state->key_attnums[level] - 1;
            keys[level] = key_text(state, level, slot->tts_values[c], slot->tts_isnull[c], state->column_type[c]);
        }
        write_op &op = new_op(state);
        op.kind = op_kind::set_node;
        op.path = joined(state, keys);
        add_fields(state, op, slot, false, true);
        MemoryContextSwitchTo(previous);
        return slot;
    }

    // The row's current keys, from the plan's row identity columns.
    char **old_keys(modify_state *state, ResultRelInfo *info, TupleTableSlot *plan_slot) {
        auto **keys = static_cast<char **>(palloc(sizeof(char *) * (state->key_count + 1)));
        TupleDesc description = RelationGetDescr(info->ri_RelationDesc);
        for (int level = 0; level < state->key_count; ++level) {
            bool is_null = false;
            const Datum value = ExecGetJunkAttribute(plan_slot, state->junk_attnums[level], &is_null);
            keys[level] = key_text(state, level, value, is_null, TupleDescAttr(description, state->key_attnums[level] - 1)->atttypid);
        }
        return keys;
    }

    TupleTableSlot *firebase_update(EState *estate, ResultRelInfo *info, TupleTableSlot *slot, TupleTableSlot *plan_slot) {
        auto *state = static_cast<modify_state *>(info->ri_FdwState);
        const MemoryContext previous = MemoryContextSwitchTo(estate->es_query_cxt);
        char **keys = old_keys(state, info, plan_slot);
        slot_getallattrs(slot);
        // A key column set to another value moves the node.
        auto **new_keys = static_cast<char **>(palloc(sizeof(char *) * (state->key_count + 1)));
        bool moved = false;
        for (int level = 0; level < state->key_count; ++level) {
            const int c = state->key_attnums[level] - 1;
            new_keys[level] = keys[level];
            if (!state->updated[c]) continue;
            new_keys[level] = key_text(state, level, slot->tts_values[c], slot->tts_isnull[c], state->column_type[c]);
            moved = moved || std::strcmp(new_keys[level], keys[level]) != 0;
        }
        write_op &op = new_op(state);
        op.kind = moved ? op_kind::move : op_kind::set_fields;
        op.path = joined(state, keys);
        if (moved) op.to = joined(state, new_keys);
        add_fields(state, op, slot, true, false);
        MemoryContextSwitchTo(previous);
        return slot;
    }

    TupleTableSlot *firebase_delete(EState *estate, ResultRelInfo *info, TupleTableSlot *slot, TupleTableSlot *plan_slot) {
        auto *state = static_cast<modify_state *>(info->ri_FdwState);
        const MemoryContext previous = MemoryContextSwitchTo(estate->es_query_cxt);
        write_op &op = new_op(state);
        op.kind = op_kind::remove;
        op.path = joined(state, old_keys(state, info, plan_slot));
        MemoryContextSwitchTo(previous);
        return slot;
    }

    // Sends the statement's changes as one update of the table's path.
    void firebase_end_modify(EState *, ResultRelInfo *info) {
        auto *state = static_cast<modify_state *>(info->ri_FdwState);
        if (!state || state->op_count == 0) return;
        const outcome result = run(state->s, [state](const sc::firebase::database &db, outcome &) {
            namespace rows = sc_pglib::firebase_rows;
            const auto base = full_path(state->s, state->path);
            std::vector<rows::change> changes;
            changes.reserve(static_cast<std::size_t>(state->op_count));
            for (int i = 0; i < state->op_count; ++i) {
                const write_op &op = state->ops[i];
                rows::change each;
                each.kind = op.kind == op_kind::set_node ? rows::change_kind::set_node
                            : op.kind == op_kind::set_fields ? rows::change_kind::set_fields
                            : op.kind == op_kind::move ? rows::change_kind::move
                                                       : rows::change_kind::remove;
                each.path = op.path;
                if (op.to) each.to = op.to;
                for (int f = 0; f < op.field_count; ++f) each.fields.emplace_back(op.field_paths[f], op.field_values[f]);
                changes.push_back(std::move(each));
            }
            sc::firebase::json values = sc::firebase::json::object();
            for (auto &[path, value]: rows::update_members(changes, [&](const std::string &path) { return db.get(base + "/" + path); }))
                values[path] = std::move(value);
            db.update(base, values);
        });
        result.error.raise();
    }

    int firebase_updatable(Relation) {
        return (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE);
    }
}

// sc_firebase_fdw_handler(): the callbacks that make sc_firebase's foreign tables readable and writable.
extern "C" Datum sc_firebase_fdw_handler(PG_FUNCTION_ARGS) {
    FdwRoutine *routine = makeNode(FdwRoutine);
    routine->GetForeignRelSize = firebase_rel_size;
    routine->GetForeignPaths = firebase_paths;
    routine->GetForeignPlan = firebase_plan;
    routine->BeginForeignScan = firebase_begin;
    routine->IterateForeignScan = firebase_iterate;
    routine->ReScanForeignScan = firebase_rescan;
    routine->EndForeignScan = firebase_end;
    routine->ExplainForeignScan = firebase_explain;
    routine->AddForeignUpdateTargets = firebase_update_targets;
    routine->PlanForeignModify = firebase_plan_modify;
    routine->BeginForeignModify = firebase_begin_modify;
    routine->ExecForeignInsert = firebase_insert;
    routine->ExecForeignUpdate = firebase_update;
    routine->ExecForeignDelete = firebase_delete;
    routine->EndForeignModify = firebase_end_modify;
    routine->IsForeignRelUpdatable = firebase_updatable;
    PG_RETURN_POINTER(routine);
}
