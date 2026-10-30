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
//
// Each sheet has the columns' names in a bold first row, kept in view (frozen), then a row per
// row: numbers (integers, float, numeric, oid) as numbers, booleans as booleans, date, timestamp,
// timestamptz (in the session's time zone) and time as Excel's dates and times with a matching
// format, NULL as an empty cell, anything else as its text. Columns are as wide as their contents
// (up to 80 characters). Returns how many rows were written, all sheets together.
//
// It works in three steps, as pg_glue.h has it: the arguments read and checked (C++), the queries
// run through SPI into plain data (PostgreSQL, which may raise), then the workbook made and
// written (C++).

#include <excel.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

#include "pg_glue.h"

extern "C" {
#include <catalog/pg_authid.h>
#include <catalog/pg_type.h>
#include <executor/spi.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/builtins.h>
#include <utils/date.h>
#include <utils/jsonb.h>
#include <utils/timestamp.h>

PG_FUNCTION_INFO_V1(sc_xlsx_query);
PG_FUNCTION_INFO_V1(sc_xlsx_sheets);
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
        int columns = 0;
        const char **headers = nullptr;
        uint64 rows = 0;
        cell_data *cells = nullptr; // rows * columns
    };

    // The sheets and their queries from the arguments, checked as Excel checks sheet names, into
    // sheets (allocated in the current context). Never raises.
    int read_sheets(const std::string_view single_query, const std::string_view single_sheet, const char *sheets_json, sheet_data *&sheets,
                    sc_pglib::pending_error &error) noexcept {
        try {
            std::vector<std::pair<std::string, std::string>> wanted;
            if (sheets_json) {
                const auto parsed = nlohmann::ordered_json::parse(sheets_json);
                if (!parsed.is_object() || parsed.empty())
                    throw std::invalid_argument(R"(sheets are a JSON object of sheet names and their queries: {"Sheet": "select ..."})");
                for (const auto &[name, query]: parsed.items()) {
                    if (!query.is_string()) throw std::invalid_argument("sheet \"" + name + "\": its query isn't text");
                    wanted.emplace_back(name, query.get<std::string>());
                }
            } else {
                wanted.emplace_back(std::string{single_sheet}, std::string{single_query});
            }
            sc::office::excel::workbook names; // checks each name (length, characters, used already)
            for (const auto &[name, query]: wanted) {
                (void) names.add(name);
                if (query.find_first_not_of(" \t\r\n") == std::string::npos) throw std::invalid_argument("sheet \"" + name + "\": its query is empty");
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

    // Runs a sheet's query and keeps its rows as plain data in outer. PostgreSQL code: may raise.
    void run_query(sheet_data &sheet, MemoryContext outer) {
        SPI_execute(sheet.query, false, 0);
        if (!SPI_tuptable)
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_xlsx: sheet \"%s\": its query returns no rows (it isn't a SELECT)", sheet.name)));
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

    // The workbook made from the sheets and written to path, then made readable by all. Never raises.
    void write_workbook(const char *path, const sheet_data *sheets, const int count, sc_pglib::pending_error &error) noexcept {
        try {
            using namespace sc::office;
            excel::workbook book;
            for (int s = 0; s < count; ++s) {
                const auto &data = sheets[s];
                auto &sheet = book.add(data.name);
                std::vector<std::size_t> widths(data.columns, 0);
                for (int c = 0; c < data.columns; ++c) {
                    auto &cell = sheet[{0, static_cast<std::size_t>(c)}];
                    cell = std::string{data.headers[c]};
                    cell.style.font.bold = true;
                    widths[c] = shown_length(data.headers[c]);
                }
                for (uint64 r = 0; r < data.rows; ++r) {
                    for (int c = 0; c < data.columns; ++c) {
                        const auto &from = data.cells[r * data.columns + c];
                        if (from.is == kind::empty) continue;
                        auto &cell = sheet[{static_cast<std::size_t>(r + 1), static_cast<std::size_t>(c)}];
                        std::size_t width = 0;
                        switch (from.is) {
                            case kind::number:
                                cell = from.number;
                                switch (from.as) {
                                    case shown::date: cell.style.number_format = "yyyy-mm-dd"; width = 10; break;
                                    case shown::date_time: cell.style.number_format = "yyyy-mm-dd hh:mm:ss"; width = 19; break;
                                    case shown::time: cell.style.number_format = "hh:mm:ss"; width = 8; break;
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
                for (int c = 0; c < data.columns; ++c) sheet.width(static_cast<std::size_t>(c), static_cast<double>(std::clamp<std::size_t>(widths[c] + 2, 8, 80)));
                if (data.columns > 0) sheet.freeze("A2");
            }
            excel::write(book, path);
            if (::chmod(path, 0644) != 0) error.set(ERRCODE_IO_ERROR, WARNING, "sc_xlsx: could not make \"%s\" readable by all: %m", path);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_xlsx: \"%s\": %s", path, problem.what());
        } catch (...) {
            error.set(ERRCODE_IO_ERROR, ERROR, "sc_xlsx: unexpected C++ exception");
        }
    }

    Datum make_xlsx(FunctionCallInfo fcinfo, const char *sheets_json, const std::string_view query, const std::string_view sheet_name) {
        if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("sc_xlsx: permission denied to write a file"),
                            errdetail("Only roles with the privileges of the \"pg_write_server_files\" role may, as with COPY to a file.")));
        const char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
        if (path[0] != '/') ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("sc_xlsx: the path must be absolute: \"%s\"", path)));

        sc_pglib::pending_error error;
        sheet_data *sheets = nullptr;
        const int count = read_sheets(query, sheet_name, sheets_json, sheets, error);
        error.raise();

        const MemoryContext outer = CurrentMemoryContext;
        if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("sc_xlsx: SPI_connect failed")));
        for (int s = 0; s < count; ++s) run_query(sheets[s], outer);
        SPI_finish();

        int64 rows = 0;
        for (int s = 0; s < count; ++s) rows += static_cast<int64>(sheets[s].rows);
        write_workbook(path, sheets, count, error);
        error.raise();
        PG_RETURN_INT64(rows);
    }
}

// sc_xlsx(path text, query text [, sheet text]) -> bigint
extern "C" Datum sc_xlsx_query(PG_FUNCTION_ARGS) {
    const char *query = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char *sheet = PG_NARGS() > 2 ? text_to_cstring(PG_GETARG_TEXT_PP(2)) : "Sheet1";
    return make_xlsx(fcinfo, nullptr, query, sheet);
}

// sc_xlsx(path text, sheets json or jsonb) -> bigint: json is text already; jsonb is written out.
extern "C" Datum sc_xlsx_sheets(PG_FUNCTION_ARGS) {
    const char *json = nullptr;
    if (get_fn_expr_argtype(fcinfo->flinfo, 1) == JSONBOID) {
        Jsonb *sheets = PG_GETARG_JSONB_P(1);
        json = JsonbToCString(nullptr, &sheets->root, VARSIZE(sheets));
    } else {
        json = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }
    return make_xlsx(fcinfo, json, {}, {});
}
