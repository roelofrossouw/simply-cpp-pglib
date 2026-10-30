#pragma once
// A Firebase foreign table's rows, from the part of the tree a scan fetched: plain C++, no
// PostgreSQL, so it can be tested on its own.
//
// A table maps a path and the key levels under it (keys 'account_id,imei'): every object node at
// the last key level is a row, the keys on the way to it are its key columns, and its fields the
// other columns. A scan's key values (from WHERE account_id = '42') narrow what's fetched: the
// leading ones that are known are added to the path, and later known ones filter the walk.

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sc_pglib::firebase_rows {
    using json = nlohmann::json;

    // A row: its key values (one per key level) and its node (an object).
    struct row {
        std::vector<std::string> keys;
        const json *node = nullptr;
    };

    // How many of the key values, from the first, are known: the levels added to the fetched path.
    std::size_t leading_known(const std::vector<std::optional<std::string>> &known);

    // The path to fetch: base, then the leading known key values.
    std::string fetch_path(const std::string &base, const std::vector<std::optional<std::string>> &known);

    // The rows under fetched (the value at fetch_path), one per object node at the last key level
    // whose key values match the known ones. Nodes that aren't objects (a "timestamp" beside the
    // devices) are passed over.
    std::vector<row> rows(const json &fetched, const std::vector<std::optional<std::string>> &known);

    // A node's field: a name, or a '/'-separated path into it ("gps/lat"); nullptr when it's not there.
    const json *field(const json &node, std::string_view path);

    // A value as a column's text: a string as it is, a number or boolean as JSON writes it, an
    // object or array as JSON; nullopt for null. as_json: always JSON (for json and jsonb columns).
    std::optional<std::string> text_of(const json &value, bool as_json);

    // A row's change through a foreign table, for one statement's update.
    enum class change_kind { set_node, set_fields, remove, move };
    struct change {
        change_kind kind = change_kind::remove;
        std::string path; // the row's keys joined with '/'
        std::string to;   // move: the new keys
        std::vector<std::pair<std::string, std::string>> fields; // field (a name or path) -> its value as JSON
    };

    // The statement's changes as the members of one multi-path update of the table's path:
    //   set_node    path -> an object of the fields (null ones left out; a path builds nesting)
    //   set_fields  path/field -> each value (null removes the field)
    //   remove      path -> null
    //   move        path -> null, to -> the node read_node(path) gives, with the fields applied
    std::vector<std::pair<std::string, json>> update_members(const std::vector<change> &changes,
                                                             const std::function<json(const std::string &)> &read_node);

    // "a, b" -> {"a", "b"}: a comma-separated list, trimmed, empty items dropped.
    std::vector<std::string> split_list(const std::string &list);
}
