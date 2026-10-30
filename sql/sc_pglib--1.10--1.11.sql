-- sc_pglib 1.10 -> 1.11: Excel workbooks written with a format (README.md, "Formatting a sheet").
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.11'" to load this file. \quit

-- One sheet, formatted: formatting is a JSON object of how the sheet looks, such as
--   '{"freeze_panes": "B2", "autofilter": true, "columns": {"price": "currency"}, "format": {"A": {"width": 30}}}'
-- Its entries are applied in the order written, so it is json, not jsonb.
CREATE FUNCTION sc_xlsx(path text, query text, sheet text, formatting json) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_xlsx_query'
    LANGUAGE C STRICT VOLATILE;

-- A sheet per query with a format for every sheet; a sheet may have its own over it, its query as
-- {"query": "select ...", "freeze_panes": "A3", ...}.
CREATE FUNCTION sc_xlsx(path text, sheets json, formatting json) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_xlsx_sheets'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_xlsx(path text, sheets jsonb, formatting json) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_xlsx_sheets'
    LANGUAGE C STRICT VOLATILE;
