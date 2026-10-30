#include "xlsx_format.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace sc_pglib::xlsx {
    namespace excel = sc::office::excel;

    namespace {
        [[noreturn]] void wrong(const std::string &where, const std::string &what) { throw std::invalid_argument(where + ": " + what); }

        std::string lower(std::string text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // "dash_dot" as Excel names it: "dashDot" (a name already so stays).
        std::string excel_name(const std::string_view name) {
            std::string out;
            bool upper = false;
            for (const char c: name) {
                if (c == '_') upper = true;
                else out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c, upper = false;
            }
            return out;
        }

        bool boolean(const json &value, const std::string &where) {
            if (!value.is_boolean()) wrong(where, "true or false, not " + value.dump());
            return value.get<bool>();
        }

        double number(const json &value, const std::string &where, const double low, const double high) {
            if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < low || value.get<double>() > high)
                wrong(where, "a number from " + json(low).dump() + " to " + json(high).dump() + ", not " + value.dump());
            return value.get<double>();
        }

        std::string text(const json &value, const std::string &where) {
            if (!value.is_string()) wrong(where, "text, not " + value.dump());
            return value.get<std::string>();
        }

        // "#c00000", "#c00", or a CSS name ("red", "lightgray"); null for none.
        std::optional<sc::color> color(const json &value, const std::string &where) {
            if (value.is_null()) return std::nullopt;
            const auto given = text(value, where);
            if (given.starts_with('#')) {
                if ((given.size() != 7 && given.size() != 4) || !std::all_of(given.begin() + 1, given.end(), [](const unsigned char c) { return std::isxdigit(c); }))
                    wrong(where, "\"" + given + "\" isn't a colour: #rrggbb, #rgb or a name such as \"red\"");
                return sc::color::from_hex(given);
            }
            const auto made = sc::color::from_string(lower(given));
            // from_string() gives black for a name it doesn't know
            if (made.to_hex() == "#000000" && lower(given) != "black") wrong(where, "\"" + given + "\" isn't a colour: #rrggbb, #rgb or a name such as \"red\"");
            return made;
        }

        // A border: "thin", true (thin), false or null (none), or {"line": "thin", "color": "#999999"}.
        excel::edge edge(const json &value, const std::string &where) {
            if (value.is_null()) return {};
            if (value.is_boolean()) return value.get<bool>() ? excel::edge{excel::line::thin} : excel::edge{};
            const auto line = [&](const json &name, const std::string &at) {
                try {
                    return excel::line_named(excel_name(text(name, at)));
                } catch (const std::invalid_argument &) {
                    wrong(at, "\"" + name.get<std::string>() +
                                  "\" isn't a line: none, thin, medium, thick, hair, dotted, dashed, double, dash_dot, dash_dot_dot, "
                                  "slant_dash_dot, medium_dashed, medium_dash_dot, medium_dash_dot_dot");
                }
            };
            if (value.is_string()) return {line(value, where)};
            if (!value.is_object()) wrong(where, R"(a line ("thin"), true, false or {"line": "thin", "color": "#999999"}, not )" + value.dump());
            excel::edge made{excel::line::thin};
            for (const auto &[key, each]: value.items()) {
                if (key == "line") made.line = line(each, where + "." + key);
                else if (key == "color") made.color = color(each, where + "." + key);
                else wrong(where, "\"" + key + "\" isn't a border's: line, color");
            }
            return made;
        }
    }

    namespace {
        // A number format by name, or Excel's own code ("0.0%", "#,##0.00 \"kg\"").
        std::string number_format(const json &value, const std::string &where, const std::string &symbol) {
            const auto given = text(value, where);
            const auto name = lower(given);
            if (name == "general") return "";
            if (name == "text") return "@";
            if (name == "integer") return "0";
            if (name == "thousands") return "#,##0";
            if (name == "decimal") return "0.00";
            if (name == "number") return "#,##0.00";
            if (name == "percent") return "0%";
            if (name == "currency") return "\"" + symbol + "\"#,##0.00;-\"" + symbol + "\"#,##0.00";
            if (name == "accounting") return "_-\"" + symbol + "\"* #,##0.00_-;-\"" + symbol + "\"* #,##0.00_-;_-\"" + symbol + "\"* \"-\"??_-;_-@_-";
            if (name == "date") return "yyyy-mm-dd";
            if (name == "datetime") return "yyyy-mm-dd hh:mm:ss";
            if (name == "time") return "hh:mm:ss";
            if (given.empty() || given.size() > 255) wrong(where, "a number format is a name (general, text, integer, thousands, decimal, number, "
                                                                  "percent, currency, accounting, date, datetime, time) or Excel's code, up to 255 characters");
            return given;
        }

        enum class place { cells, columns, rows };

        // An entry: a number format's name or code, or an object of what it changes.
        change change_of(const json &value, const std::string &where, const place at, const std::string &symbol) {
            change made;
            if (value.is_string()) {
                made.number_format = number_format(value, where, symbol);
                return made;
            }
            if (!value.is_object()) wrong(where, R"(a number format ("currency") or an object ({"bold": true}), not )" + value.dump());
            for (const auto &[key, each]: value.items()) {
                const auto here = where + "." + key;
                if (key == "number_format") made.number_format = number_format(each, here, symbol);
                else if (key == "font") {
                    made.font = text(each, here);
                    if (made.font->empty() || made.font->size() > 31) wrong(here, "a font's name is 1 to 31 characters");
                } else if (key == "font_size") made.font_size = number(each, here, 1, 409);
                else if (key == "bold") made.bold = boolean(each, here);
                else if (key == "italic") made.italic = boolean(each, here);
                else if (key == "strike") made.strike = boolean(each, here);
                else if (key == "underline") {
                    if (each.is_boolean()) made.underline = each.get<bool>() ? excel::underline::single : excel::underline::none;
                    else try {
                        made.underline = excel::underline_named(excel_name(text(each, here)));
                    } catch (const std::invalid_argument &) {
                        wrong(here, "true, false, single, double, single_accounting or double_accounting");
                    }
                } else if (key == "color") made.color.emplace(color(each, here));
                else if (key == "background") made.background.emplace(color(each, here));
                else if (key == "border") made.left = made.right = made.top = made.bottom = edge(each, here);
                else if (key == "border_left") made.left = edge(each, here);
                else if (key == "border_right") made.right = edge(each, here);
                else if (key == "border_top") made.top = edge(each, here);
                else if (key == "border_bottom") made.bottom = edge(each, here);
                else if (key == "align") {
                    const auto name = text(each, here);
                    try {
                        made.horizontal = excel::horizontal_named(name == "center_across" ? "centerContinuous" : name);
                    } catch (const std::invalid_argument &) {
                        wrong(here, "general, left, center, right, fill, justify, center_across or distributed");
                    }
                } else if (key == "valign") {
                    try {
                        made.vertical = excel::vertical_named(text(each, here));
                    } catch (const std::invalid_argument &) {
                        wrong(here, "top, center, bottom, justify or distributed");
                    }
                } else if (key == "wrap") made.wrap = boolean(each, here);
                else if (key == "indent") made.indent = static_cast<unsigned>(number(each, here, 0, 250));
                else if (key == "rotation") made.rotation = static_cast<int>(number(each, here, -90, 90));
                else if (key == "width" && at == place::columns) {
                    if (each.is_string() && each.get<std::string>() == "auto") made.width.emplace(std::nullopt);
                    else made.width.emplace(number(each, here, 0, 255));
                } else if (key == "height" && at == place::rows) made.height = number(each, here, 0, 409);
                else if (key == "hidden" && at != place::cells) made.hidden = boolean(each, here);
                else if (key == "width" || key == "height" || key == "hidden")
                    wrong(here, key == "width" ? "only columns have a width (\"C\", \"C:E\" or by name in \"columns\")"
                                : key == "height" ? "only rows have a height (\"2\", \"2:5\")"
                                                  : "only whole columns or rows are hidden");
                else
                    wrong(where, "\"" + key + "\" isn't something to change: number_format, font, font_size, bold, italic, underline, strike, color, "
                                              "background, border, border_left, border_right, border_top, border_bottom, align, valign, wrap, indent, "
                                              "rotation" + std::string{at == place::columns ? ", width, hidden" : at == place::rows ? ", height, hidden" : ""});
            }
            return made;
        }

        // "B2", "F2:F999", "C", "C:E", "2", "2:5" (any case).
        target target_of(const std::string &key, const std::string &where) {
            std::string name;
            for (const char c: key) name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            const auto colon = name.find(':');
            const auto first = name.substr(0, colon), last = colon == std::string::npos ? first : name.substr(colon + 1);
            const auto digits = [](const std::string &part) {
                return !part.empty() && std::all_of(part.begin(), part.end(), [](const unsigned char c) { return std::isdigit(c) != 0; });
            };
            const auto letters = [](const std::string &part) {
                return !part.empty() && std::all_of(part.begin(), part.end(), [](const unsigned char c) { return std::isalpha(c) != 0; });
            };
            target made;
            try {
                if (digits(first) && digits(last)) {
                    const auto from = std::stoul(first), to = std::stoul(last);
                    if (first.size() > 7 || last.size() > 7 || from < 1 || to < 1 || from > excel::max_rows || to > excel::max_rows) throw std::out_of_range("rows");
                    made.is = target::kind::rows;
                    made.first_row = std::min(from, to) - 1;
                    made.last_row = std::max(from, to) - 1;
                    made.last_column = excel::max_columns - 1;
                } else if (letters(first) && letters(last)) {
                    const auto from = excel::column_index(first), to = excel::column_index(last);
                    made.is = target::kind::columns;
                    made.first_column = std::min(from, to);
                    made.last_column = std::max(from, to);
                    made.last_row = excel::max_rows - 1;
                } else {
                    const excel::range cells{name};
                    if (cells.last.row >= excel::max_rows || cells.last.column >= excel::max_columns) throw std::out_of_range("cells");
                    made = {target::kind::cells, cells.first.row, cells.last.row, cells.first.column, cells.last.column};
                }
            } catch (const std::exception &) {
                wrong(where, "\"" + key + "\" isn't a cell (\"B2\"), range (\"F2:F999\"), column (\"C\", \"C:E\") or row (\"2\", \"2:5\")");
            }
            return made;
        }
    }

    void change::apply(excel::style &style) const {
        if (number_format) style.number_format = *number_format;
        if (font) style.font.name = *font;
        if (font_size) style.font.size = *font_size;
        if (bold) style.font.bold = *bold;
        if (italic) style.font.italic = *italic;
        if (strike) style.font.strike = *strike;
        if (underline) style.font.underline = *underline;
        if (color) style.font.color = *color;
        if (background) style.background = *background;
        if (left) style.borders.left = *left;
        if (right) style.borders.right = *right;
        if (top) style.borders.top = *top;
        if (bottom) style.borders.bottom = *bottom;
        if (horizontal) style.alignment.horizontal = *horizontal;
        if (vertical) style.alignment.vertical = *vertical;
        if (wrap) style.alignment.wrap = *wrap;
        if (indent) style.alignment.indent = *indent;
        if (rotation) style.alignment.rotation = *rotation;
    }

    bool change::styles() const {
        return number_format || font || font_size || bold || italic || strike || underline || color || background || left || right || top || bottom ||
               horizontal || vertical || wrap || indent || rotation;
    }

    sheet_format sheet_format::read(const json &options) {
        sheet_format made;
        if (options.is_null()) return made;
        if (!options.is_object()) wrong("format", R"(an object, such as {"freeze_panes": "B2"}, not )" + options.dump());
        std::string symbol = "R ";
        if (options.contains("currency_symbol")) {
            symbol = text(options["currency_symbol"], "format.currency_symbol");
            if (symbol.find('"') != std::string::npos || symbol.size() > 10) wrong("format.currency_symbol", "up to 10 characters, without \"");
        }
        for (const auto &[key, value]: options.items()) {
            const auto where = "format." + key;
            if (key == "header") made.header = boolean(value, where);
            else if (key == "freeze_panes") {
                if (value.is_null() || (value.is_boolean() && !value.get<bool>())) made.freeze.emplace(std::nullopt);
                else {
                    try {
                        const excel::address cell{text(value, where)};
                        if (cell.row >= excel::max_rows || cell.column >= excel::max_columns) throw std::out_of_range("cell");
                        if (cell == excel::address{0, 0}) made.freeze.emplace(std::nullopt);
                        else made.freeze.emplace(cell);
                    } catch (const std::exception &) {
                        wrong(where, "the cell below and right of what stays in view (\"B2\"), or false, not " + value.dump());
                    }
                }
            } else if (key == "autofilter") {
                if (value.is_boolean()) made.filter_data = value.get<bool>();
                else {
                    try {
                        made.filter = excel::range{text(value, where)};
                        if (made.filter->last.row >= excel::max_rows || made.filter->last.column >= excel::max_columns) throw std::out_of_range("cells");
                    } catch (const std::exception &) {
                        wrong(where, "true, false or a range (\"A1:F20\"), not " + value.dump());
                    }
                }
            } else if (key == "auto_width") made.auto_width = boolean(value, where);
            else if (key == "date_format") made.date = number_format(value, where, symbol);
            else if (key == "datetime_format") made.date_time = number_format(value, where, symbol);
            else if (key == "time_format") made.time = number_format(value, where, symbol);
            else if (key == "currency_symbol") continue;
            else if (key == "style") made.style = change_of(value, where, place::cells, symbol);
            else if (key == "header_style") made.header_style = change_of(value, where, place::cells, symbol);
            else if (key == "columns" || key == "format") {
                if (!value.is_object()) wrong(where, R"(an object, such as {")" + std::string{key == "columns" ? "price" : "F2:F999"} + R"(": "currency"})");
                for (const auto &[name, entry]: value.items()) {
                    const auto here = where + "." + name;
                    if (key == "columns") {
                        made.columns.emplace_back(name, change_of(entry, here, place::columns, symbol));
                    } else {
                        const auto cells = target_of(name, where);
                        made.format.emplace_back(cells, change_of(entry, here, cells.is == target::kind::columns ? place::columns
                                                                               : cells.is == target::kind::rows    ? place::rows
                                                                                                                   : place::cells, symbol));
                    }
                }
            } else {
                wrong("format", "\"" + key + "\" isn't a sheet's: header, freeze_panes, autofilter, auto_width, date_format, datetime_format, "
                                             "time_format, currency_symbol, style, header_style, columns, format");
            }
        }
        return made;
    }

    json merged(const json &common, const json &own) {
        if (own.is_null()) return common;
        if (common.is_null() || !common.is_object() || !own.is_object()) return own;
        json out = common;
        for (const auto &[key, value]: own.items()) {
            if ((key == "columns" || key == "format") && value.is_object() && out.contains(key) && out[key].is_object()) {
                for (const auto &[name, entry]: value.items()) {
                    out[key].erase(name); // so it comes after the common ones
                    out[key][name] = entry;
                }
            } else {
                out[key] = value;
            }
        }
        return out;
    }
}
