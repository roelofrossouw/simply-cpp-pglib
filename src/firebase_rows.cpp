#include "firebase_rows.h"

namespace sc_pglib::firebase_rows {
    std::size_t leading_known(const std::vector<std::optional<std::string>> &known) {
        std::size_t count = 0;
        while (count < known.size() && known[count]) ++count;
        return count;
    }

    std::string fetch_path(const std::string &base, const std::vector<std::optional<std::string>> &known) {
        std::string path = base;
        for (std::size_t i = 0; i < leading_known(known); ++i) path += "/" + *known[i];
        return path;
    }

    namespace {
        void walk(const json &node, const std::vector<std::optional<std::string>> &known, std::vector<std::string> &keys,
                  std::vector<row> &out) {
            const auto level = keys.size();
            if (level == known.size()) {
                if (node.is_object()) out.push_back({keys, &node});
                return;
            }
            if (!node.is_object()) return;
            if (known[level]) {
                const auto found = node.find(*known[level]);
                if (found == node.end()) return;
                keys.push_back(*known[level]);
                walk(*found, known, keys, out);
                keys.pop_back();
                return;
            }
            for (const auto &[key, child]: node.items()) {
                keys.push_back(key);
                walk(child, known, keys, out);
                keys.pop_back();
            }
        }
    }

    std::vector<row> rows(const json &fetched, const std::vector<std::optional<std::string>> &known) {
        std::vector<row> out;
        // The leading known keys are in the fetched path already: start below them.
        const auto leading = leading_known(known);
        std::vector<std::string> keys;
        for (std::size_t i = 0; i < leading; ++i) keys.push_back(*known[i]);
        walk(fetched, known, keys, out);
        return out;
    }

    const json *field(const json &node, const std::string_view path) {
        const json *at = &node;
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto slash = path.find('/', start);
            const auto name = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (!name.empty()) {
                if (!at->is_object()) return nullptr;
                const auto found = at->find(std::string{name});
                if (found == at->end()) return nullptr;
                at = &*found;
            }
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
        return at;
    }

    std::optional<std::string> text_of(const json &value, const bool as_json) {
        if (value.is_null()) return std::nullopt;
        if (as_json) return value.dump();
        if (value.is_string()) return value.get<std::string>();
        return value.dump(); // numbers, booleans ("true"), objects, arrays
    }

    std::vector<std::string> split_list(const std::string &list) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= list.size()) {
            const auto comma = list.find(',', start);
            auto item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const auto first = item.find_first_not_of(" \t");
            if (first != std::string::npos) out.push_back(item.substr(first, item.find_last_not_of(" \t") - first + 1));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }
}
