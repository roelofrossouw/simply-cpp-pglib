// Excel workbooks from SQL (sc::office::excel): queries' rows written as sheets of an .xlsx file
// on the server, as COPY ... TO 'file' writes a CSV one, and with its rule: only superusers and
// members of pg_write_server_files may, and the path is absolute. The file is written as the
// server's operating system user, then made readable by all (0644), as files for a web server
// to send usually need.
//
//   sc_xlsx(path, query [, sheet])     one sheet ("Sheet1" by default)
//   sc_xlsx(path, '{"Sheet": "query", ...}'::json or ::jsonb)
//                                      a sheet per query: json keeps them in the order written,
//                                      jsonb sorts its keys (shorter first)
//   sc_xlsx(path, query, sheet, formatting json)
//   sc_xlsx(path, sheets, formatting json)
//                                      formatted (xlsx_format.h; README.md, "Formatting a sheet"):
//                                      formatting for every sheet, a sheet given as {"query": "...",
//                                      ...} with its own keys over it
//
// Each sheet has the columns' names in a bold first row, kept in view (frozen), then a row per
// row: numbers (integers, float, numeric, oid) as numbers, booleans as booleans, date, timestamp,
// timestamptz (in the session's time zone) and time as Excel's dates and times with a matching
// format, NULL as an empty cell, anything else as its text. Columns are as wide as their contents
// (up to 80 characters), unless formatting says otherwise. Returns how many rows were written, all
// sheets together.
//
// sc_read_xlsx(path) reads one back as json: {"Sheet": [[cell, ...], ...], ...}, each sheet an array
// of its rows from the first, each row its cells from column A up to its last one set (an empty
// row is []); a cell is a number, text, true/false or null (none). A number shown with a date or
// time format is text: "2026-10-31", "2026-10-31 14:05:00" or "14:05:00". A formula gives the
// value it last calculated.
//
// sc_read_xlsx(path, sheet [, from_row]) gives one sheet's rows as rows, the sheet by name (ignoring
// case) or number (1 is the first), from from_row on (2 skips a row of headings), into the call's
// column definition list: each column named after a sheet
// column (a, b, ... aa) gets that column's cell, converted to the column's type from its text (as
// above: numbers, true/false, dates as ISO text); a column named "row" gets the row number. Empty
// rows are left out, empty cells are NULL. sc_xlsx_sheets(path) lists the sheets, numbered.
//
//   SELECT * FROM sc_read_xlsx('/var/www/data/input/prices.xlsx', 1, 2) AS x("row" int, a text, b numeric);
//
// Reading is as COPY from a file: superusers and pg_read_server_files only, an absolute path.
//
// Writing works in three steps, as pg_glue.h has it: the arguments read and checked (C++), the queries
// run through SPI into plain data (PostgreSQL, which may raise), then the workbook made and
// written (C++).
//
// CSV files (sc::office::csv) the same way, without sheets:
//
//   sc_csv(path, query [, delimiter])  a query's rows, the columns' names first; numbers as numbers,
//                                      booleans as TRUE/FALSE, NULL as an empty field, anything else
//                                      (dates as ISO) as its text
//   sc_read_csv(path [, from_row [, delimiter]])
//                                      its rows as rows, as sc_read_xlsx(path, sheet, from_row)
//                                      gives a sheet's; each field as written (no numbers guessed),
//                                      converted to its column's type. The delimiter is found when
//                                      not given: a comma, unless semicolons fit the file better.
//
//   SELECT * FROM sc_read_csv('/var/www/data/input/prices.csv', 2) AS x("row" int, a text, b numeric);
//
//   sc_csvzip(path, query [, entry [, delimiter]])
//                                      sc_csv's file in a ZIP archive, its one entry: entry, or the
//                                      archive's name with .csv
//   sc_csvzip(path, '{"a.csv": "query", ...}'::json or ::jsonb [, delimiter])
//                                      a CSV file per query in the archive, as sc_xlsx's sheets
//   sc_read_csvzip(path, entry [, from_row [, delimiter]])
//                                      that entry read as sc_read_csv reads a file

#include <excel.h>
#include <codec.h>
#include <codec_gzip.h>
#include <sc_archive.h>
#include <sc_csv.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fnmatch.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

#include "pg_glue.h"
#include "xlsx_format.h"

extern "C" {
#include <catalog/pg_authid.h>
#include <catalog/pg_type.h>
#include <executor/spi.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/builtins.h>
#include <funcapi.h>
#include <utils/date.h>
#include <utils/lsyscache.h>
#include <utils/jsonb.h>
#include <utils/timestamp.h>
#include <mb/pg_wchar.h>

PG_FUNCTION_INFO_V1(sc_xlsx_query);
PG_FUNCTION_INFO_V1(sc_xlsx_sheets);
PG_FUNCTION_INFO_V1(sc_read_xlsx);
PG_FUNCTION_INFO_V1(sc_read_xlsx_rows);
PG_FUNCTION_INFO_V1(sc_xlsx_sheet_names);
PG_FUNCTION_INFO_V1(sc_csv_query);
PG_FUNCTION_INFO_V1(sc_read_csv_rows);
PG_FUNCTION_INFO_V1(sc_csvzip_query);
PG_FUNCTION_INFO_V1(sc_csvzip_files);
PG_FUNCTION_INFO_V1(sc_read_csvzip_rows);
}

namespace {
    // Days from Excel's day 0 (1899-12-30) to PostgreSQL's (2000-01-01).
    constexpr double excel_epoch_offset = 36526;

    enum class kind : char { empty, number, boolean, text };
    enum class shown : char { plain, date, date_time, time };

    // One cell, plain data in PostgreSQL's memory (no destructors).
    struct cell_data {
        kind is = kind::empty;
        shown as = shown::plain;
        bool flag = false;
        double number = 0;
        const char *text = nullptr;
    };

    struct sheet_data {
        const char *name = nullptr;
        const char *query = nullptr;
        const char *format = nullptr; // its format's json, checked (xlsx_format.h); nullptr: none
        int columns = 0;
        const char **headers = nullptr;
        uint64 rows = 0;
        cell_data *cells = nullptr; // rows * columns
    };

    // The sheets, their queries and formats from the arguments, checked as Excel checks sheet names,
    // into sheets (allocated in the current context): a sheet's format is format_json's (for all
    // sheets) with its own over it. Never raises.
    int read_sheets(const std::string_view single_query, const std::string_view single_sheet, const char *sheets_json, const char *format_json,
                    sheet_data *&sheets, sc_pglib::pending_error &error) noexcept {
        try {
            struct wanted_sheet {
                std::string name, query, format;
            };
            std::vector<wanted_sheet> wanted;
            nlohmann::ordered_json common;
            if (format_json) {
                try {
                    common = nlohmann::ordered_json::parse(format_json);
                } catch (const nlohmann::json::parse_error &problem) {
                    throw std::invalid_argument(std::string{"format: "} + problem.what());
                }
            }
            const auto format_of = [](const std::string &name, const nlohmann::ordered_json &format) {
                if (format.is_null()) return std::string{};
                try {
                    (void) sc_pglib::xlsx::sheet_format::read(format);
                } catch (const std::invalid_argument &problem) {
                    throw std::invalid_argument("sheet \"" + name + "\": " + problem.what());
                }
                return format.dump();
            };
            if (sheets_json) {
                const auto parsed = nlohmann::ordered_json::parse(sheets_json);
                if (!parsed.is_object() || parsed.empty())
                    throw std::invalid_argument(R"(sheets are a JSON object of sheet names and their queries: {"Sheet": "select ..."})");
                for (const auto &[name, sheet]: parsed.items()) {
                    // "select ...", or {"query": "select ...", and its format's keys}
                    nlohmann::ordered_json own;
                    std::string query;
                    if (sheet.is_object()) {
                        if (!sheet.contains("query") || !sheet["query"].is_string())
                            throw std::invalid_argument("sheet \"" + name + R"(": its object has no "query" text ({"query": "select ...", "freeze_panes": "B2"}))");
                        query = sheet["query"].get<std::string>();
                        own = sheet;
                        own.erase("query");
                    } else if (sheet.is_string()) {
                        query = sheet.get<std::string>();
                    } else {
                        throw std::invalid_argument("sheet \"" + name + "\": its query isn't text");
                    }
                    wanted.push_back({name, query, format_of(name, sc_pglib::xlsx::merged(common, own))});
                }
            } else {
                wanted.push_back({std::string{single_sheet}, std::string{single_query}, format_of(std::string{single_sheet}, common)});
            }
            sc::office::excel::workbook names; // checks each name (length, characters, used already)
            for (const auto &[name, query, format]: wanted) {
                (void) names.add(name);
                if (query.find_first_not_of(" \t\r\n") == std::string::npos) throw std::invalid_argument("sheet \"" + name + "\": its query is empty");
            }
            sheets = static_cast<sheet_data *>(sc_pglib::alloc_no_oom(sizeof(sheet_data) * wanted.size(), error));
            if (!sheets) return 0;
            for (std::size_t i = 0; i < wanted.size(); ++i) {
                sheets[i] = sheet_data{};
                sheets[i].name = sc_pglib::new_cstring(wanted[i].name, error);
                sheets[i].query = sc_pglib::new_cstring(wanted[i].query, error);
                if (!wanted[i].format.empty()) sheets[i].format = sc_pglib::new_cstring(wanted[i].format, error);
                if (error.any()) return 0;
            }
            return static_cast<int>(wanted.size());
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_xlsx: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_xlsx: unexpected C++ exception");
        }
        return 0;
    }

    // A value as its text, in the outer context.
    const char *as_text(HeapTuple tuple, TupleDesc description, int column, MemoryContext outer) {
        char *text = SPI_getvalue(tuple, description, column);
        return MemoryContextStrdup(outer, text ? text : "");
    }

    // Runs a sheet's query and keeps its rows as plain data in outer; who names it in an error
    // ("sc_xlsx: sheet \"Prices\""). PostgreSQL code: may raise.
    void run_query(sheet_data &sheet, MemoryContext outer, const char *who) {
        SPI_execute(sheet.query, false, 0);
        if (!SPI_tuptable)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s: its query returns no rows (it isn't a SELECT)", who)));
        const TupleDesc description = SPI_tuptable->tupdesc;
        sheet.columns = description->natts;
        sheet.rows = SPI_processed;
        sheet.headers = static_cast<const char **>(MemoryContextAlloc(outer, sizeof(char *) * Max(sheet.columns, 1)));
        for (int c = 0; c < sheet.columns; ++c) sheet.headers[c] = MemoryContextStrdup(outer, NameStr(TupleDescAttr(description, c)->attname));
        sheet.cells = static_cast<cell_data *>(MemoryContextAllocHuge(outer, sizeof(cell_data) * Max(sheet.rows * sheet.columns, 1)));

        for (uint64 r = 0; r < sheet.rows; ++r) {
            const HeapTuple tuple = SPI_tuptable->vals[r];
            for (int c = 0; c < sheet.columns; ++c) {
                cell_data &cell = sheet.cells[r * sheet.columns + c];
                cell = cell_data{};
                if (TupleDescAttr(description, c)->attisdropped) continue;
                bool is_null = false;
                const Datum value = SPI_getbinval(tuple, description, c + 1, &is_null);
                if (is_null) continue;
                const auto number = [&](const double n, const shown as = shown::plain) {
                    cell.is = kind::number;
                    cell.number = n;
                    cell.as = as;
                };
                switch (SPI_gettypeid(description, c + 1)) {
                    case INT2OID: number(DatumGetInt16(value)); break;
                    case INT4OID: number(DatumGetInt32(value)); break;
                    case INT8OID: number(static_cast<double>(DatumGetInt64(value))); break;
                    case OIDOID: number(DatumGetObjectId(value)); break;
                    case FLOAT4OID:
                    case FLOAT8OID:
                    case NUMERICOID: {
                        const double n = SPI_gettypeid(description, c + 1) == FLOAT4OID ? DatumGetFloat4(value)
                                         : SPI_gettypeid(description, c + 1) == FLOAT8OID ? DatumGetFloat8(value)
                                                                                         : DatumGetFloat8(DirectFunctionCall1(numeric_float8, value));
                        if (std::isfinite(n)) number(n);
                        else cell.is = kind::text, cell.text = as_text(tuple, description, c + 1, outer);
                        break;
                    }
                    case BOOLOID:
                        cell.is = kind::boolean;
                        cell.flag = DatumGetBool(value);
                        break;
                    case DATEOID: {
                        const DateADT day = DatumGetDateADT(value);
                        if (DATE_NOT_FINITE(day) || day + excel_epoch_offset < 1) cell.is = kind::text, cell.text = as_text(tuple, description, c + 1, outer);
                        else number(day + excel_epoch_offset, shown::date);
                        break;
                    }
                    case TIMESTAMPOID:
                    case TIMESTAMPTZOID: {
                        const Timestamp at = SPI_gettypeid(description, c + 1) == TIMESTAMPOID
                                                 ? DatumGetTimestamp(value)
                                                 : DatumGetTimestamp(DirectFunctionCall1(timestamptz_timestamp, value));
                        const double serial = excel_epoch_offset + static_cast<double>(at) / static_cast<double>(USECS_PER_DAY);
                        if (TIMESTAMP_NOT_FINITE(at) || serial < 1) cell.is = kind::text, cell.text = as_text(tuple, description, c + 1, outer);
                        else number(serial, shown::date_time);
                        break;
                    }
                    case TIMEOID: number(static_cast<double>(DatumGetTimeADT(value)) / static_cast<double>(USECS_PER_DAY), shown::time); break;
                    default:
                        cell.is = kind::text;
                        cell.text = as_text(tuple, description, c + 1, outer);
                }
            }
        }
        SPI_freetuptable(SPI_tuptable);
    }

    // How many characters text shows as (UTF-8 code points).
    std::size_t shown_length(const std::string_view text) {
        std::size_t n = 0;
        for (const char c: text)
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
        return n;
    }

    // A sheet's format applied to what's written: the header (row 0, when there is one) and the
    // data under it, in columns 0 to data.columns - 1. widths are the ones found for the contents.
    void apply_format(sc::office::excel::sheet &sheet, const sc_pglib::xlsx::sheet_format &format, const sheet_data &data,
                      std::vector<std::size_t> &widths) {
        using sc_pglib::xlsx::target;
        const std::size_t first_data = format.header ? 1 : 0, rows = first_data + data.rows, columns = static_cast<std::size_t>(data.columns);
        std::vector<std::optional<double>> width(columns); // the ones given; auto otherwise
        std::vector<bool> given(columns, false);
        // What change does over the block clipped to what's written, and to its columns or rows.
        const auto over = [&](const sc_pglib::xlsx::change &change, const std::size_t first_row, const std::size_t last_row, const std::size_t first_column,
                              const std::size_t last_column, const bool whole_columns, const bool whole_rows) {
            if (columns == 0) return;
            if (change.styles())
                for (auto r = first_row; r <= last_row && r < rows; ++r)
                    for (auto c = first_column; c <= last_column && c < columns; ++c) change.apply(sheet[{r, c}].style);
            if (whole_columns)
                for (auto c = first_column; c <= last_column && c < columns; ++c) {
                    if (change.width) width[c] = *change.width, given[c] = true;
                    if (change.hidden) sheet.hidden_column(c, *change.hidden);
                }
            if (whole_rows)
                for (auto r = first_row; r <= last_row && r < rows; ++r) {
                    if (change.height) sheet.height(r, *change.height);
                    if (change.hidden) sheet.hidden_row(r, *change.hidden);
                }
        };
        if (rows > 0) over(format.style, 0, rows - 1, 0, columns - 1, false, false);
        if (format.header) over(format.header_style, 0, 0, 0, columns - 1, false, false);
        for (const auto &[name, change]: format.columns) {
            int found = -1;
            for (int c = 0; c < data.columns && found < 0; ++c)
                if (name == data.headers[c]) found = c;
            if (found < 0) {
                std::string names;
                for (int c = 0; c < data.columns; ++c) names += (c ? ", " : "") + std::string{data.headers[c]};
                throw std::invalid_argument("format.columns: the query has no column \"" + name + "\" (it has " + (names.empty() ? "none" : names) + ")");
            }
            const auto c = static_cast<std::size_t>(found);
            if (rows > first_data) over(change, first_data, rows - 1, c, c, false, false);
            over(sc_pglib::xlsx::change{.width = change.width, .hidden = change.hidden}, 0, 0, c, c, true, false);
        }
        for (const auto &[cells, change]: format.format)
            over(change, cells.first_row, cells.last_row, cells.first_column, cells.last_column, cells.is == target::kind::columns,
                 cells.is == target::kind::rows);
        for (std::size_t c = 0; c < columns; ++c) {
            if (given[c] && width[c]) sheet.width(c, *width[c]);
            else if (given[c] || format.auto_width) sheet.width(c, static_cast<double>(std::clamp<std::size_t>(widths[c] + 2, 8, 80)));
        }
        if (format.freeze) {
            if (*format.freeze) sheet.freeze(**format.freeze);
        } else if (format.header && columns > 0) {
            sheet.freeze("A2");
        }
        if (format.filter) sheet.filter(*format.filter);
        else if (format.filter_data && columns > 0) sheet.filter(sc::office::excel::range{sc::office::excel::address{0, 0}, sc::office::excel::address{std::max<std::size_t>(rows, 1) - 1, columns - 1}});
    }

    // The workbook made from the sheets and written to path, then made readable by all. Never raises.
    void write_workbook(const char *path, const sheet_data *sheets, const int count, sc_pglib::pending_error &error) noexcept {
        const char *sheet_name = "";
        try {
            using namespace sc::office;
            excel::workbook book;
            for (int s = 0; s < count; ++s) {
                const auto &data = sheets[s];
                sheet_name = data.name;
                const auto format = sc_pglib::xlsx::sheet_format::read(data.format ? sc_pglib::xlsx::json::parse(data.format) : sc_pglib::xlsx::json{});
                auto &sheet = book.add(data.name);
                const std::size_t first_data = format.header ? 1 : 0;
                std::vector<std::size_t> widths(data.columns, 0);
                if (format.header) {
                    for (int c = 0; c < data.columns; ++c) {
                        auto &cell = sheet[{0, static_cast<std::size_t>(c)}];
                        cell = std::string{data.headers[c]};
                        cell.style.font.bold = true;
                        widths[c] = shown_length(data.headers[c]);
                    }
                }
                for (uint64 r = 0; r < data.rows; ++r) {
                    for (int c = 0; c < data.columns; ++c) {
                        const auto &from = data.cells[r * data.columns + c];
                        if (from.is == kind::empty) continue;
                        auto &cell = sheet[{static_cast<std::size_t>(r + first_data), static_cast<std::size_t>(c)}];
                        std::size_t width = 0;
                        switch (from.is) {
                            case kind::number:
                                cell = from.number;
                                switch (from.as) {
                                    case shown::date: cell.style.number_format = format.date; width = std::max<std::size_t>(10, format.date.size()); break;
                                    case shown::date_time: cell.style.number_format = format.date_time; width = std::max<std::size_t>(19, format.date_time.size()); break;
                                    case shown::time: cell.style.number_format = format.time; width = std::max<std::size_t>(8, format.time.size()); break;
                                    case shown::plain: width = std::min<std::size_t>(15, std::to_string(from.number).find('.') + 3); break;
                                }
                                break;
                            case kind::boolean:
                                cell = from.flag;
                                width = 5;
                                break;
                            case kind::text:
                                cell = std::string{from.text};
                                width = shown_length(from.text);
                                break;
                            case kind::empty: break;
                        }
                        widths[c] = std::max(widths[c], width);
                    }
                }
                apply_format(sheet, format, data, widths);
            }
            sheet_name = "";
            excel::write(book, path);
            if (::chmod(path, 0644) != 0) error.set(ERRCODE_IO_ERROR, WARNING, "sc_xlsx: could not make \"%s\" readable by all: %m", path);
        } catch (const std::invalid_argument &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_xlsx: sheet \"%s\": %s", sheet_name, problem.what());
        } catch (const std::exception &problem) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_xlsx: \"%s\": %s", path, problem.what());
        } catch (...) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_xlsx: unexpected C++ exception");
        }
    }

    Datum make_xlsx(FunctionCallInfo fcinfo, const char *sheets_json, const char *format_json, const std::string_view query, const std::string_view sheet_name) {
        if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("sc_xlsx: permission denied to write a file"),
                            errdetail("Only roles with the privileges of the \"pg_write_server_files\" role may, as with COPY to a file.")));
        const char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
        if (path[0] != '/') ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("sc_xlsx: the path must be absolute: \"%s\"", path)));

        sc_pglib::pending_error error;
        sheet_data *sheets = nullptr;
        const int count = read_sheets(query, sheet_name, sheets_json, format_json, sheets, error);
        error.raise();

        const MemoryContext outer = CurrentMemoryContext;
        if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("sc_xlsx: SPI_connect failed")));
        for (int s = 0; s < count; ++s) run_query(sheets[s], outer, psprintf("sc_xlsx: sheet \"%s\"", sheets[s].name));
        SPI_finish();

        int64 rows = 0;
        for (int s = 0; s < count; ++s) rows += static_cast<int64>(sheets[s].rows);
        write_workbook(path, sheets, count, error);
        error.raise();
        PG_RETURN_INT64(rows);
    }
}

namespace {
    // Whether an Excel number format shows a date (or time): its codes outside quoted text,
    // escapes and [colour]/[$-locale] sections have y, d (date) or h, s (time).
    std::pair<bool, bool> date_or_time(const std::string_view format) {
        bool date = false, time = false, quoted = false, bracket = false;
        for (std::size_t i = 0; i < format.size(); ++i) {
            const char c = format[i];
            if (quoted) {
                quoted = c != '"';
            } else if (bracket) {
                bracket = c != ']';
                // [h]:mm elapsed hours are a time
                if ((c == 'h' || c == 'H' || c == 's' || c == 'S') && i > 0 && format[i - 1] == '[') time = true;
            } else if (c == '"') {
                quoted = true;
            } else if (c == '[') {
                bracket = true;
            } else if (c == '\\' || c == '_' || c == '*') {
                ++i;
            } else if (c == 'y' || c == 'Y' || c == 'd' || c == 'D') {
                date = true;
            } else if (c == 'h' || c == 'H' || c == 's' || c == 'S') {
                time = true;
            }
        }
        return {date, time};
    }

    // An Excel serial (days since 1899-12-30) as ISO text: the date, the time, or both.
    std::string serial_text(const double serial, const bool date, const bool time) {
        auto days = static_cast<std::int64_t>(std::floor(serial));
        auto seconds = static_cast<std::int64_t>(std::llround((serial - static_cast<double>(days)) * 86400));
        if (seconds >= 86400) ++days, seconds -= 86400;
        // Civil from days (Howard Hinnant's algorithm), from 1970-01-01 = serial 25569.
        const std::int64_t z = days - 25569 + 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const auto doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned day = doy - (153 * mp + 2) / 5 + 1;
        const unsigned month = mp < 10 ? mp + 3 : mp - 9;
        const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2);
        char text[40];
        if (date && time)
            snprintf(text, sizeof text, "%04lld-%02u-%02u %02lld:%02lld:%02lld", static_cast<long long>(year), month, day,
                          static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60));
        else if (date)
            snprintf(text, sizeof text, "%04lld-%02u-%02u", static_cast<long long>(year), month, day);
        else
            snprintf(text, sizeof text, "%02lld:%02lld:%02lld", static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60),
                          static_cast<long long>(seconds % 60));
        return text;
    }

    nlohmann::ordered_json rows_of(const sc::office::excel::sheet &sheet) {
        nlohmann::ordered_json rows = nlohmann::ordered_json::array();
        for (const auto &row: sheet) {
            while (rows.size() < row.index()) rows.push_back(nlohmann::ordered_json::array());
            nlohmann::ordered_json cells = nlohmann::ordered_json::array();
            for (const auto &cell: row) {
                while (cells.size() < cell.column) cells.push_back(nullptr);
                if (const auto *number = std::get_if<double>(&cell.value)) {
                    const auto [date, time] = date_or_time(cell.style.number_format);
                    if (date || time) cells.push_back(serial_text(*number, date, time));
                    else if (std::nearbyint(*number) == *number && std::fabs(*number) < 9007199254740992.0) cells.push_back(static_cast<std::int64_t>(*number));
                    else cells.push_back(*number);
                } else if (const auto *text = std::get_if<std::string>(&cell.value)) {
                    cells.push_back(*text);
                } else if (const auto *flag = std::get_if<bool>(&cell.value)) {
                    cells.push_back(*flag);
                } else {
                    cells.push_back(nullptr);
                }
            }
            // A row with only styled cells and no values reads as [].
            while (!cells.empty() && cells.back().is_null()) cells.erase(cells.size() - 1);
            rows.push_back(std::move(cells));
        }
        while (!rows.empty() && rows.back().empty()) rows.erase(rows.size() - 1);
        return rows;
    }

    // The workbook at path as json text (all sheets, or the one named), into a new text. Never raises.
    struct varlena *read_workbook(const char *path, const char *only, sc_pglib::pending_error &error) noexcept {
        try {
            const auto book = sc::office::excel::read(path);
            nlohmann::ordered_json out;
            if (only) {
                if (!book.contains(only)) throw std::invalid_argument(std::string{"there's no sheet \""} + only + "\"");
                out = rows_of(book.at(only));
            } else {
                out = nlohmann::ordered_json::object();
                for (const auto &sheet: book) out[sheet.name()] = rows_of(sheet);
            }
            return sc_pglib::new_varlena(out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_read_xlsx: \"%s\": %s", path, problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_read_xlsx: unexpected C++ exception");
        }
        return nullptr;
    }
}

namespace {
    // A cell's value as text, as sc_read_xlsx's json has it (a date or time as ISO text), or none
    template<typename Cell>
    std::optional<std::string> cell_text(const Cell &cell) {
        if (const auto *number = std::get_if<double>(&cell.value)) {
            const auto [date, time] = date_or_time(cell.style.number_format);
            if (date || time) return serial_text(*number, date, time);
            if (std::nearbyint(*number) == *number && std::fabs(*number) < 9007199254740992.0) return std::to_string(static_cast<std::int64_t>(*number));
            return nlohmann::json(*number).dump();
        }
        if (const auto *text = std::get_if<std::string>(&cell.value)) return *text;
        if (const auto *flag = std::get_if<bool>(&cell.value)) return *flag ? "true" : "false";
        return std::nullopt;
    }

    // A sheet's rows as plain data in PostgreSQL's memory (no destructors): each row its number (from 1)
    // and its cells from column A, nullptr for an empty one
    struct read_row {
        int number = 0;
        int count = 0;
        const char **cells = nullptr;
    };

    struct read_rows {
        int count = 0;
        read_row *rows = nullptr;
    };

    // The sheet named (ignoring case), or the one at number (from 1)
    template<typename Book>
    const auto &find_sheet(const Book &book, const char *name, const int number) {
        if (name) {
            if (!book.contains(name)) throw std::invalid_argument(std::string{"there's no sheet \""} + name + "\"");
            return book.at(name);
        }
        int at = 0;
        for (const auto &sheet: book)
            if (++at == number) return sheet;
        throw std::invalid_argument("there's no sheet " + std::to_string(number) + " (the workbook has " + std::to_string(at) + ")");
    }

    // A CSV cell's value as text: it was read as text, so it's what the file has.
    std::optional<std::string> cell_text(const sc::office::csv::cell &cell) {
        if (const auto *text = std::get_if<std::string>(&cell.value)) return *text;
        return std::nullopt;
    }

    // A sheet's rows that have a value, from from_row on, into out (Excel's or CSV's). C++: throws.
    template<typename Sheet>
    void rows_into(const Sheet &sheet, const int from_row, read_rows &out, sc_pglib::pending_error &error) {
        {
            std::vector<std::pair<int, std::vector<std::optional<std::string>>>> rows;
            for (const auto &row: sheet) {
                if (static_cast<int>(row.index()) + 1 < from_row) continue;
                std::vector<std::optional<std::string>> cells;
                for (const auto &cell: row) {
                    auto text = cell_text(cell);
                    if (!text) continue;
                    if (cells.size() <= cell.column) cells.resize(cell.column + 1);
                    cells[cell.column] = std::move(text);
                }
                if (!cells.empty()) rows.emplace_back(static_cast<int>(row.index()) + 1, std::move(cells));
            }
            out.rows = static_cast<read_row *>(sc_pglib::alloc_no_oom(sizeof(read_row) * std::max<std::size_t>(rows.size(), 1), error));
            if (!out.rows) return;
            for (const auto &[row_number, cells]: rows) {
                read_row &row = out.rows[out.count++];
                row = read_row{row_number, static_cast<int>(cells.size()), nullptr};
                row.cells = static_cast<const char **>(sc_pglib::alloc_no_oom(sizeof(char *) * cells.size(), error));
                if (!row.cells) return;
                for (std::size_t c = 0; c < cells.size(); ++c) {
                    row.cells[c] = cells[c] ? sc_pglib::new_cstring(*cells[c], error) : nullptr;
                    if (error.any()) return;
                }
            }
        }
    }

    // One sheet's rows that have a value, into out. Never raises.
    void read_sheet_rows(const char *path, const char *name, const int number, const int from_row, read_rows &out,
                         sc_pglib::pending_error &error) noexcept {
        try {
            const auto book = sc::office::excel::read(path);
            rows_into(find_sheet(book, name, number), from_row, out, error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_read_xlsx: \"%s\": %s", path, problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_read_xlsx: unexpected C++ exception");
        }
    }

    // A CSV file's bytes (an entry of a ZIP archive when entry isn't null), gunzipped when they're
    // compressed (gzip, bzip2, xz or zstd, whatever the name says), into out (allocated in the
    // current context). Never raises.
    struct raw_bytes {
        char *data = nullptr;
        std::size_t size = 0;
    };

    // The entry entry names in an archive: that name; a shell pattern (* and ?, matched against its
    // whole name, or its file name when the pattern has no '/'); or "" for the only .csv entry. One
    // must match: throws, naming the entries, when none or more than one does.
    template<typename Archive>
    std::string entry_named(const Archive &archive, const std::string &entry) {
        std::vector<std::string> names, matched;
        for (const auto &each: archive.entries())
            if (!each.name.ends_with('/')) names.push_back(each.name);
        const bool pattern = entry.find_first_of("*?[") != std::string::npos;
        for (const auto &name: names) {
            if (entry.empty()) {
                std::string lower = name;
                std::ranges::transform(lower, lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.ends_with(".csv")) matched.push_back(name);
            } else if (pattern) {
                const auto slash = name.find_last_of('/');
                const auto base = entry.find('/') == std::string::npos && slash != std::string::npos ? name.substr(slash + 1) : name;
                if (::fnmatch(entry.c_str(), base.c_str(), 0) == 0) matched.push_back(name);
            } else if (name == entry) {
                matched.push_back(name);
            }
        }
        if (matched.size() == 1) return matched.front();
        std::string listed;
        for (const auto &name: names) listed += (listed.empty() ? "" : ", ") + name;
        const auto has = " (it has " + (listed.empty() ? std::string{"nothing"} : listed) + ")";
        const auto what = entry.empty() ? std::string{"a .csv entry"} : "\"" + entry + "\"";
        if (matched.empty()) throw std::invalid_argument("there's no " + what + " in it" + has);
        throw std::invalid_argument("more than one entry is " + what + has);
    }

    void read_csv_bytes(const char *function, const char *path, const char *entry, raw_bytes &out, sc_pglib::pending_error &error) noexcept {
        try {
            std::string bytes;
            if (entry) {
                const auto archive = sc::archive::open(path);
                bytes = archive.read(entry_named(archive, entry));
            } else {
                std::ifstream file{path, std::ios::binary};
                if (!file) throw std::runtime_error(std::string{"can't be opened: "} + strerror(errno));
                bytes.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
                if (file.bad()) throw std::runtime_error("couldn't be read");
            }
            if (const auto codec = sc::compression::detect(bytes)) bytes = codec->decompress(bytes);
            if (bytes.size() >= MaxAllocHugeSize) throw std::runtime_error("it's too big to read");
            out.data = static_cast<char *>(MemoryContextAllocExtended(CurrentMemoryContext, bytes.size() + 1, MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM));
            if (!out.data) {
                error.set(ERRCODE_OUT_OF_MEMORY, ERROR, "%s: out of memory", function);
                return;
            }
            std::memcpy(out.data, bytes.data(), bytes.size());
            out.data[bytes.size()] = '\0';
            out.size = bytes.size();
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s: \"%s\": %s", function, path, problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s: unexpected C++ exception", function);
        }
    }

    // The bytes from encoding (a PostgreSQL name: 'WIN1251', 'windows-1251', 'LATIN1'; "" is UTF-8)
    // into the database's, checked. PostgreSQL code: raises for a name it doesn't know, or bytes that
    // aren't that encoding.
    void to_database_encoding(const char *function, const char *encoding, raw_bytes &bytes) {
        const int from = *encoding ? pg_char_to_encoding(encoding) : PG_UTF8;
        if (from < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s: \"%s\" isn't an encoding PostgreSQL knows", function, encoding)));
        if (from == GetDatabaseEncoding()) {
            // Checked all the same, as COPY checks what it reads.
            (void) pg_verify_mbstr(from, bytes.data, static_cast<int>(bytes.size), false);
            return;
        }
        char *converted = reinterpret_cast<char *>(pg_do_encoding_conversion(reinterpret_cast<unsigned char *>(bytes.data), static_cast<int>(bytes.size), from,
                                                                            GetDatabaseEncoding()));
        if (converted != bytes.data) {
            bytes.data = converted;
            bytes.size = std::strlen(converted);
        }
    }

    // A CSV file's rows that have a value, every field as text, into out. Never raises.
    void parse_csv_rows(const char *function, const char *path, const raw_bytes &bytes, const char delimiter, const int from_row, read_rows &out,
                        sc_pglib::pending_error &error) noexcept {
        try {
            rows_into(sc::office::csv::read_from(std::string{bytes.data, bytes.size}, {.delimiter = delimiter, .typed = false}), from_row, out, error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s: \"%s\": %s", function, path, problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s: unexpected C++ exception", function);
        }
    }

    // sc_read_csv and sc_read_csvzip: bytes, encoding, rows. PostgreSQL code: raises.
    void read_csv_file(const char *function, const char *path, const char *entry, const char *encoding, const char delimiter, const int from_row,
                       read_rows &out) {
        raw_bytes bytes;
        sc_pglib::pending_error error;
        read_csv_bytes(function, path, entry, bytes, error);
        error.raise();
        to_database_encoding(function, encoding, bytes);
        parse_csv_rows(function, path, bytes, delimiter, from_row, out, error);
        error.raise();
    }

    // The workbook's sheet names, in order, into names. Never raises.
    int read_sheet_names(const char *path, const char **&names, sc_pglib::pending_error &error) noexcept {
        try {
            const auto book = sc::office::excel::read(path);
            std::vector<std::string> list;
            for (const auto &sheet: book) list.push_back(sheet.name());
            names = static_cast<const char **>(sc_pglib::alloc_no_oom(sizeof(char *) * std::max<std::size_t>(list.size(), 1), error));
            if (!names) return 0;
            for (std::size_t i = 0; i < list.size(); ++i) {
                names[i] = sc_pglib::new_cstring(list[i], error);
                if (error.any()) return 0;
            }
            return static_cast<int>(list.size());
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_xlsx_sheets: \"%s\": %s", path, problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_xlsx_sheets: unexpected C++ exception");
        }
        return 0;
    }

    // COPY's rule for reading a file: pg_read_server_files, an absolute path. PostgreSQL code: raises.
    const char *readable_path(PG_FUNCTION_ARGS, const char *function) {
        if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("%s: permission denied to read a file", function),
                            errdetail("Only roles with the privileges of the \"pg_read_server_files\" role may, as with COPY from a file.")));
        const char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
        if (path[0] != '/') ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("%s: the path must be absolute: \"%s\"", function, path)));
        return path;
    }

    // The function's rows go into a tuplestore of its result's shape. PostgreSQL code: raises.
    Tuplestorestate *start_rows(PG_FUNCTION_ARGS, TupleDesc description) {
        auto *result = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
        if (!result || !IsA(result, ReturnSetInfo) || !(result->allowedModes & SFRM_Materialize))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("set-valued function called in context that cannot accept a set")));
        const MemoryContext previous = MemoryContextSwitchTo(result->econtext->ecxt_per_query_memory);
        Tuplestorestate *store = tuplestore_begin_heap(true, false, work_mem);
        result->returnMode = SFRM_Materialize;
        result->setResult = store;
        result->setDesc = CreateTupleDescCopy(description);
        MemoryContextSwitchTo(previous);
        return store;
    }

    // A sheet column's letters (a, z, aa, ...: any case) as its index from 0; -1 for anything else
    int column_index(const char *name) {
        const std::size_t length = std::strlen(name);
        if (length == 0 || length > 3) return -1;
        int index = 0;
        for (std::size_t i = 0; i < length; ++i) {
            const char c = static_cast<char>(name[i] | 0x20);
            if (c < 'a' || c > 'z') return -1;
            index = index * 26 + (c - 'a' + 1);
        }
        return index - 1;
    }

    std::string column_letters(int index) {
        std::string letters;
        for (++index; index > 0; index = (index - 1) / 26) letters.insert(letters.begin(), static_cast<char>('A' + (index - 1) % 26));
        return letters;
    }

    // The cell a value comes from, for errors converting it ("CONTEXT: sc_read_xlsx: cell B7")
    struct cell_place {
        const char *function = "";
        char column[8] = {};
        int row = 0;
    };

    void cell_context(void *place) {
        const auto *cell = static_cast<const cell_place *>(place);
        errcontext("%s: cell %s%d", cell->function, cell->column, cell->row);
    }

    // The call's column definition list: each column's sheet column (-1 for "row") and its type's
    // input function. PostgreSQL code: raises.
    struct column_plan {
        TupleDesc description = nullptr;
        int columns = 0;
        int *sources = nullptr;
        FmgrInfo *inputs = nullptr;
        Oid *parameters = nullptr;
    };

    column_plan plan_columns(PG_FUNCTION_ARGS, const char *function) {
        column_plan plan;
        if (get_call_result_type(fcinfo, nullptr, &plan.description) != TYPEFUNC_COMPOSITE)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("%s: name the columns you want", function),
                            errhint("SELECT * FROM %s(...) AS x(a text, b numeric)", function)));
        plan.columns = plan.description->natts;
        plan.sources = static_cast<int *>(palloc(sizeof(int) * Max(plan.columns, 1)));
        plan.inputs = static_cast<FmgrInfo *>(palloc(sizeof(FmgrInfo) * Max(plan.columns, 1)));
        plan.parameters = static_cast<Oid *>(palloc(sizeof(Oid) * Max(plan.columns, 1)));
        for (int c = 0; c < plan.columns; ++c) {
            const Form_pg_attribute attribute = TupleDescAttr(plan.description, c);
            const char *column = NameStr(attribute->attname);
            plan.sources[c] = pg_strcasecmp(column, "row") == 0 ? -1 : column_index(column);
            if (plan.sources[c] == -1 && pg_strcasecmp(column, "row") != 0)
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("%s: column \"%s\" isn't a sheet column (a, b, ... aa) or \"row\"", function, column)));
            Oid input;
            getTypeInputInfo(attribute->atttypid, &input, &plan.parameters[c]);
            fmgr_info(input, &plan.inputs[c]);
        }
        return plan;
    }

    // The rows read, each value converted to its column's type, as the function's result.
    // PostgreSQL code: raises (a value its type won't take, with the cell it came from).
    Datum put_rows(PG_FUNCTION_ARGS, const char *function, const column_plan &plan, const read_rows &data) {
        Tuplestorestate *store = start_rows(fcinfo, plan.description);
        auto *values = static_cast<Datum *>(palloc(sizeof(Datum) * Max(plan.columns, 1)));
        auto *nulls = static_cast<bool *>(palloc(sizeof(bool) * Max(plan.columns, 1)));
        cell_place place;
        place.function = function;
        ErrorContextCallback context{error_context_stack, cell_context, &place};
        error_context_stack = &context;
        for (int r = 0; r < data.count; ++r) {
            const read_row &row = data.rows[r];
            for (int c = 0; c < plan.columns; ++c) {
                const int32 typmod = TupleDescAttr(plan.description, c)->atttypmod;
                const int source = plan.sources[c];
                const char *text = source == -1 ? psprintf("%d", row.number) : source < row.count ? row.cells[source] : nullptr;
                nulls[c] = text == nullptr;
                values[c] = 0;
                if (text) {
                    const std::string letters = source == -1 ? std::string("row ") : column_letters(source);
                    strlcpy(place.column, letters.c_str(), sizeof place.column);
                    place.row = row.number;
                    values[c] = InputFunctionCall(&plan.inputs[c], const_cast<char *>(text), plan.parameters[c], typmod);
                }
            }
            tuplestore_putvalues(store, plan.description, values, nulls);
        }
        error_context_stack = context.previous;
        return (Datum) 0;
    }
}

// sc_read_xlsx(path text, sheet text or integer, from_row integer) -> SETOF record
extern "C" Datum sc_read_xlsx_rows(PG_FUNCTION_ARGS) {
    const char *path = readable_path(fcinfo, "sc_read_xlsx");
    const char *name = nullptr;
    int number = 0;
    if (get_fn_expr_argtype(fcinfo->flinfo, 1) == INT4OID) {
        number = PG_GETARG_INT32(1);
        if (number < 1) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_read_xlsx: sheets are numbered from 1, not %d", number)));
    } else {
        name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }
    const int from_row = PG_NARGS() > 2 ? PG_GETARG_INT32(2) : 1;
    if (from_row < 1) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_read_xlsx: rows are numbered from 1, not %d", from_row)));
    const column_plan plan = plan_columns(fcinfo, "sc_read_xlsx");
    read_rows data;
    sc_pglib::pending_error error;
    read_sheet_rows(path, name, number, from_row, data, error);
    error.raise();
    return put_rows(fcinfo, "sc_read_xlsx", plan, data);
}

// sc_xlsx_sheets(path text) -> TABLE(sheet integer, name text)
extern "C" Datum sc_xlsx_sheet_names(PG_FUNCTION_ARGS) {
    const char *path = readable_path(fcinfo, "sc_xlsx_sheets");
    TupleDesc description = nullptr;
    if (get_call_result_type(fcinfo, nullptr, &description) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("sc_xlsx_sheets: unexpected result type")));
    const char **names = nullptr;
    sc_pglib::pending_error error;
    const int count = read_sheet_names(path, names, error);
    error.raise();
    Tuplestorestate *store = start_rows(fcinfo, description);
    for (int i = 0; i < count; ++i) {
        Datum values[2] = {Int32GetDatum(i + 1), CStringGetTextDatum(names[i])};
        bool nulls[2] = {false, false};
        tuplestore_putvalues(store, description, values, nulls);
    }
    return (Datum) 0;
}

// sc_read_xlsx(path text) -> json
extern "C" Datum sc_read_xlsx(PG_FUNCTION_ARGS) {
    const char *path = readable_path(fcinfo, "sc_read_xlsx");
    // 1.5's sc_read_xlsx(path, sheet) also came here, for one sheet's json, until the extension is updated
    const char *only = PG_NARGS() > 1 ? text_to_cstring(PG_GETARG_TEXT_PP(1)) : nullptr;
    sc_pglib::pending_error error;
    struct varlena *result = read_workbook(path, only, error);
    error.raise();
    PG_RETURN_POINTER(result);
}

// sc_xlsx(path text, query text [, sheet text [, format json]]) -> bigint
extern "C" Datum sc_xlsx_query(PG_FUNCTION_ARGS) {
    const char *query = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *sheet = PG_NARGS() > 2 ? text_to_cstring(PG_GETARG_TEXT_PP(2)) : "Sheet1";
    const char *format = PG_NARGS() > 3 ? text_to_cstring(PG_GETARG_TEXT_PP(3)) : nullptr;
    return make_xlsx(fcinfo, nullptr, format, query, sheet);
}

// sc_xlsx(path text, sheets json or jsonb [, format json]) -> bigint: json is text already; jsonb is
// written out.
extern "C" Datum sc_xlsx_sheets(PG_FUNCTION_ARGS) {
    const char *json = nullptr;
    if (get_fn_expr_argtype(fcinfo->flinfo, 1) == JSONBOID) {
        Jsonb *sheets = PG_GETARG_JSONB_P(1);
        json = JsonbToCString(nullptr, &sheets->root, VARSIZE(sheets));
    } else {
        json = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }
    const char *format = PG_NARGS() > 2 ? text_to_cstring(PG_GETARG_TEXT_PP(2)) : nullptr;
    return make_xlsx(fcinfo, json, format, {}, {});
}

namespace {
    // A delimiter argument: one character, or "" to find it when reading. PostgreSQL code: raises.
    char delimiter_of(const char *text, const char *function, const bool may_be_empty) {
        if (std::strlen(text) > 1 || (!may_be_empty && !*text))
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s: the delimiter is one character, not \"%s\"", function, text)));
        return *text;
    }

    // The query's rows written as a CSV file at path, its columns' names first, or as the one entry of a
    // ZIP archive at path when entry is given; then made readable by all. Never raises.
    // A query's rows as CSV values, its columns' names first
    sc::office::csv::sheet csv_of(const sheet_data &data) {
        sc::office::csv::sheet out;
        for (int c = 0; c < data.columns; ++c) out[{0, static_cast<std::size_t>(c)}] = std::string{data.headers[c]};
        for (uint64 r = 0; r < data.rows; ++r) {
            for (int c = 0; c < data.columns; ++c) {
                const auto &from = data.cells[r * data.columns + c];
                const sc::office::csv::address where{static_cast<std::size_t>(r + 1), static_cast<std::size_t>(c)};
                switch (from.is) {
                    case kind::number:
                        if (from.as == shown::plain) out[where] = from.number;
                        else out[where] = serial_text(from.number, from.as != shown::time, from.as != shown::date);
                        break;
                    case kind::boolean: out[where] = from.flag; break;
                    case kind::text: out[where] = std::string{from.text}; break;
                    case kind::empty: break;
                }
            }
        }
        return out;
    }

    // The queries' rows written as a CSV file at path (the first query's), or as a ZIP archive at path
    // with an entry per query (zip: each sheet_data's name is its entry); then made readable by all.
    // Never raises.
    //
    // A path ending in .gz (not zip) is written gzipped. append (zip): the archive's other entries are
    // kept, an entry of the same name replaced; without one there, it's made.
    void write_csv(const char *path, const sheet_data *data, const int count, const char delimiter, const bool zip, const bool append,
                   sc_pglib::pending_error &error) noexcept {
        const char *function = zip ? "sc_csvzip" : "sc_csv";
        try {
            if (zip) {
                std::vector<std::pair<std::string, std::string>> kept;
                struct stat there{};
                if (append && ::stat(path, &there) == 0) {
                    const auto old = sc::archive::open(path);
                    for (const auto &each: old.entries()) {
                        if (each.name.ends_with('/')) continue;
                        bool replaced = false;
                        for (int i = 0; i < count && !replaced; ++i) replaced = each.name == data[i].name;
                        if (!replaced) kept.emplace_back(each.name, old.read(each.name));
                    }
                }
                auto archive = sc::archive::create(path);
                for (auto &[name, bytes]: kept) archive.add(name, std::move(bytes));
                for (int i = 0; i < count; ++i) archive.add(data[i].name, sc::office::csv::bytes(csv_of(data[i]), {.delimiter = delimiter}));
                archive.close();
            } else if (std::string_view{path}.ends_with(".gz")) {
                const auto packed = sc::compression::gzip{}.compress(sc::office::csv::bytes(csv_of(data[0]), {.delimiter = delimiter}));
                std::ofstream file{path, std::ios::binary | std::ios::trunc};
                if (!file) throw std::runtime_error(std::string{"can't be written: "} + strerror(errno));
                file.write(packed.data(), static_cast<std::streamsize>(packed.size()));
                file.close();
                if (!file) {
                    std::remove(path);
                    throw std::runtime_error("couldn't be written");
                }
            } else {
                sc::office::csv::write(csv_of(data[0]), path, {.delimiter = delimiter});
            }
            if (::chmod(path, 0644) != 0) error.set(ERRCODE_IO_ERROR, WARNING, "%s: could not make \"%s\" readable by all: %m", function, path);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_IO_ERROR, ERROR, "%s: \"%s\": %s", function, path, problem.what());
        } catch (...) {
            error.set(ERRCODE_IO_ERROR, ERROR, "%s: unexpected C++ exception", function);
        }
    }
}

namespace {
    // A ZIP archive's entries and their queries from a JSON object ({"a.csv": "select ...", ...}), each
    // name a relative path without "." or ".." parts, into sheets (allocated in the current context; a
    // sheet_data's name is its entry). Never raises.
    int read_entries(const char *json, sheet_data *&sheets, sc_pglib::pending_error &error) noexcept {
        try {
            const auto parsed = nlohmann::ordered_json::parse(json);
            if (!parsed.is_object() || parsed.empty())
                throw std::invalid_argument(R"(the files are a JSON object of names and their queries: {"prices.csv": "select ..."})");
            std::vector<std::pair<std::string, std::string>> wanted;
            for (const auto &[name, query]: parsed.items()) {
                if (name.empty() || name.front() == '/' || name.back() == '/' || name.find('\\') != std::string::npos)
                    throw std::invalid_argument("\"" + name + "\" isn't a file name in the archive (a relative path, '/' between folders)");
                for (std::size_t start = 0; start <= name.size();) {
                    const auto end = std::min(name.find('/', start), name.size());
                    const std::string_view part{name.data() + start, end - start};
                    if (part.empty() || part == "." || part == "..") throw std::invalid_argument("\"" + name + "\" has an empty, \".\" or \"..\" part");
                    start = end + 1;
                }
                if (!query.is_string()) throw std::invalid_argument("file \"" + name + "\": its query isn't text");
                if (query.get<std::string>().find_first_not_of(" \t\r\n") == std::string::npos)
                    throw std::invalid_argument("file \"" + name + "\": its query is empty");
                wanted.emplace_back(name, query.get<std::string>());
            }
            sheets = static_cast<sheet_data *>(sc_pglib::alloc_no_oom(sizeof(sheet_data) * wanted.size(), error));
            if (!sheets) return 0;
            for (std::size_t i = 0; i < wanted.size(); ++i) {
                sheets[i] = sheet_data{};
                sheets[i].name = sc_pglib::new_cstring(wanted[i].first, error);
                sheets[i].query = sc_pglib::new_cstring(wanted[i].second, error);
                if (error.any()) return 0;
            }
            return static_cast<int>(wanted.size());
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_csvzip: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_csvzip: unexpected C++ exception");
        }
        return 0;
    }

    // sc_csv and sc_csvzip: the queries' rows written as a CSV file, or a ZIP archive of them (zip; each
    // sheet's name its entry). PostgreSQL code: raises.
    Datum make_csv(PG_FUNCTION_ARGS, const char *function, sheet_data *sheets, const int count, const bool zip, const int delimiter_argument,
                   const int append_argument = -1) {
        if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("%s: permission denied to write a file", function),
                            errdetail("Only roles with the privileges of the \"pg_write_server_files\" role may, as with COPY to a file.")));
        const char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
        if (path[0] != '/') ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("%s: the path must be absolute: \"%s\"", function, path)));
        for (int i = 0; i < count; ++i)
            if (std::string_view{sheets[i].query}.find_first_not_of(" \t\r\n") == std::string_view::npos)
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s: the query is empty", function)));
        const char delimiter = PG_NARGS() > delimiter_argument ? delimiter_of(text_to_cstring(PG_GETARG_TEXT_PP(delimiter_argument)), function, false) : ',';

        const MemoryContext outer = CurrentMemoryContext;
        if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("%s: SPI_connect failed", function)));
        for (int i = 0; i < count; ++i)
            run_query(sheets[i], outer, count > 1 || zip ? psprintf("%s: file \"%s\"", function, sheets[i].name) : function);
        SPI_finish();

        int64 rows = 0;
        for (int i = 0; i < count; ++i) rows += static_cast<int64>(sheets[i].rows);
        sc_pglib::pending_error error;
        const bool append = append_argument >= 0 && PG_NARGS() > append_argument && PG_GETARG_BOOL(append_argument);
        write_csv(path, sheets, count, delimiter, zip, append, error);
        error.raise();
        PG_RETURN_INT64(rows);
    }

    // One query, as sc_csv and the first sc_csvzip have it
    sheet_data *one_query(const char *name, const char *query) {
        auto *sheet = static_cast<sheet_data *>(palloc0(sizeof(sheet_data)));
        sheet->name = name;
        sheet->query = query;
        return sheet;
    }

    // The entry a ZIP archive's CSV file is called: the one given, or the archive's file name with .csv
    const char *csv_entry(const char *path, const char *given) {
        if (*given) return given;
        std::string_view name{path};
        name = name.substr(name.find_last_of('/') + 1);
        if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
        return psprintf("%.*s.csv", static_cast<int>(name.size()), name.data());
    }
}

// sc_csv(path text, query text, delimiter text) -> bigint
extern "C" Datum sc_csv_query(PG_FUNCTION_ARGS) {
    return make_csv(fcinfo, "sc_csv", one_query("csv", text_to_cstring(PG_GETARG_TEXT_PP(1))), 1, false, 2);
}

// sc_csvzip(path text, query text, entry text, delimiter text, append boolean) -> bigint
extern "C" Datum sc_csvzip_query(PG_FUNCTION_ARGS) {
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *entry = csv_entry(path, PG_NARGS() > 2 ? text_to_cstring(PG_GETARG_TEXT_PP(2)) : "");
    return make_csv(fcinfo, "sc_csvzip", one_query(entry, text_to_cstring(PG_GETARG_TEXT_PP(1))), 1, true, 3, 4);
}

// sc_csvzip(path text, files json or jsonb, delimiter text, append boolean) -> bigint: a file per query
extern "C" Datum sc_csvzip_files(PG_FUNCTION_ARGS) {
    const char *json = nullptr;
    if (get_fn_expr_argtype(fcinfo->flinfo, 1) == JSONBOID) {
        Jsonb *files = PG_GETARG_JSONB_P(1);
        json = JsonbToCString(nullptr, &files->root, VARSIZE(files));
    } else {
        json = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }
    sheet_data *sheets = nullptr;
    sc_pglib::pending_error error;
    const int count = read_entries(json, sheets, error);
    error.raise();
    return make_csv(fcinfo, "sc_csvzip", sheets, count, true, 2, 3);
}

// sc_read_csvzip(path text, entry text, from_row integer, delimiter text, encoding text) -> SETOF record
extern "C" Datum sc_read_csvzip_rows(PG_FUNCTION_ARGS) {
    const char *path = readable_path(fcinfo, "sc_read_csvzip");
    const char *entry = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const int from_row = PG_NARGS() > 2 ? PG_GETARG_INT32(2) : 1;
    if (from_row < 1) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_read_csvzip: rows are numbered from 1, not %d", from_row)));
    const char delimiter = PG_NARGS() > 3 ? delimiter_of(text_to_cstring(PG_GETARG_TEXT_PP(3)), "sc_read_csvzip", true) : '\0';
    const char *encoding = PG_NARGS() > 4 ? text_to_cstring(PG_GETARG_TEXT_PP(4)) : "";
    const column_plan plan = plan_columns(fcinfo, "sc_read_csvzip");
    read_rows data;
    read_csv_file("sc_read_csvzip", path, entry, encoding, delimiter, from_row, data);
    return put_rows(fcinfo, "sc_read_csvzip", plan, data);
}

// sc_read_csv(path text, from_row integer, delimiter text, encoding text) -> SETOF record
extern "C" Datum sc_read_csv_rows(PG_FUNCTION_ARGS) {
    const char *path = readable_path(fcinfo, "sc_read_csv");
    const int from_row = PG_NARGS() > 1 ? PG_GETARG_INT32(1) : 1;
    if (from_row < 1) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_read_csv: rows are numbered from 1, not %d", from_row)));
    const char delimiter = PG_NARGS() > 2 ? delimiter_of(text_to_cstring(PG_GETARG_TEXT_PP(2)), "sc_read_csv", true) : '\0';
    const char *encoding = PG_NARGS() > 3 ? text_to_cstring(PG_GETARG_TEXT_PP(3)) : "";
    const column_plan plan = plan_columns(fcinfo, "sc_read_csv");
    read_rows data;
    read_csv_file("sc_read_csv", path, nullptr, encoding, delimiter, from_row, data);
    return put_rows(fcinfo, "sc_read_csv", plan, data);
}
