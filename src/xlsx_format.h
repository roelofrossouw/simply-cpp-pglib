#pragma once

// sc_xlsx's format: how a sheet looks, from json (README.md, "Formatting a sheet"). Plain C++: what
// isn't right throws std::invalid_argument, naming the key.
//
//   {"freeze_panes": "B2", "autofilter": true, "style": {"font": "Arial", "font_size": 10},
//    "columns": {"price": "currency"}, "format": {"A": {"width": 30}, "F2:F999": {"bold": true}}}

#include <excel.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sc_pglib::xlsx {
    using json = nlohmann::ordered_json;

    // What one entry changes: only what it names, over what the cells have already.
    struct change {
        std::optional<std::string> number_format;
        std::optional<std::string> font;
        std::optional<double> font_size;
        std::optional<bool> bold, italic, strike;
        std::optional<sc::office::excel::underline> underline;
        std::optional<std::optional<sc::color>> color, background;
        std::optional<sc::office::excel::edge> left, right, top, bottom;
        std::optional<sc::office::excel::horizontal> horizontal;
        std::optional<sc::office::excel::vertical> vertical;
        std::optional<bool> wrap;
        std::optional<unsigned> indent;
        std::optional<int> rotation;
        // Columns only: a width in characters (std::nullopt inside: "auto", as wide as the contents).
        std::optional<std::optional<double>> width;
        // Rows only: a height in points.
        std::optional<double> height;
        // Columns or rows.
        std::optional<bool> hidden;

        void apply(sc::office::excel::style &style) const;
        [[nodiscard]] bool styles() const;
    };

    // Where a "format" entry goes: cells ("B2", "F2:F999"), whole columns ("C", "C:E") or whole rows
    // ("2", "2:5"), from 0.
    struct target {
        enum class kind { cells, columns, rows } is = kind::cells;
        std::size_t first_row = 0, last_row = 0, first_column = 0, last_column = 0;
    };

    struct sheet_format {
        bool header = true;
        // Unset: below the header (A2), or none without one.
        std::optional<std::optional<sc::office::excel::address>> freeze;
        // true: the header and data; a range: that one.
        bool filter_data = false;
        std::optional<sc::office::excel::range> filter;
        bool auto_width = true;
        std::string date = "yyyy-mm-dd", date_time = "yyyy-mm-dd hh:mm:ss", time = "hh:mm:ss";
        change style, header_style;
        std::vector<std::pair<std::string, change>> columns; // by the query's column names
        std::vector<std::pair<target, change>> format;

        // From sc_xlsx's json (null: the defaults).
        [[nodiscard]] static sheet_format read(const json &options);
    };

    // A sheet's format: the one for all sheets with the sheet's own over it, key by key; "columns" and
    // "format" entries are added to (the sheet's after the common ones, so they win).
    [[nodiscard]] json merged(const json &common, const json &own);
}
