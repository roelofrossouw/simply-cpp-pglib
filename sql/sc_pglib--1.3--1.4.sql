-- sc_pglib 1.3 -> 1.4: Excel workbooks (.xlsx) written from queries.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.4'" to load this file. \quit

-- A query's rows as one sheet of an .xlsx file on the server (the sheet "Sheet1" unless named);
-- returns how many rows were written. As COPY to a file: superusers and pg_write_server_files only,
-- an absolute path; the file is made readable by all (0644).
CREATE FUNCTION sc_xlsx(path text, query text) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_xlsx_query'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_xlsx(path text, query text, sheet text) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_xlsx_query'
    LANGUAGE C STRICT VOLATILE;

-- A sheet per query: {"Sheet": "select ...", ...}. json keeps the sheets in the order written;
-- jsonb sorts its keys (shorter first).
CREATE FUNCTION sc_xlsx(path text, sheets json) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_xlsx_sheets'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_xlsx(path text, sheets jsonb) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_xlsx_sheets'
    LANGUAGE C STRICT VOLATILE;
