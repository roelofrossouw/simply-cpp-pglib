-- sc_pglib 1.5 -> 1.6: a sheet's rows as a table, by sheet name or number; the sheets listed.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.6'" to load this file. \quit

-- One sheet's rows as rows, from from_row on (2 skips a row of headings), into the columns of the
-- call's column definition list, each named after its sheet column (a, b, ... aa) and of any type its
-- text converts to; a column named "row" gets the row number. Empty rows are left out, empty cells are
-- NULL:
--   SELECT * FROM sc_read_xlsx('/var/www/data/input/prices.xlsx', 'Prices', 2) AS x("row" int, a text, b numeric);
-- Replaces 1.5's sc_read_xlsx(path, sheet) returning json; sc_read_xlsx(path) still gives every sheet as json.
DROP FUNCTION sc_read_xlsx(text, text);

CREATE FUNCTION sc_read_xlsx(path text, sheet text, from_row integer DEFAULT 1) RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'sc_read_xlsx_rows'
    LANGUAGE C STRICT VOLATILE;

-- The same, the sheet by its place in the workbook: 1 is the first
CREATE FUNCTION sc_read_xlsx(path text, sheet integer, from_row integer DEFAULT 1) RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'sc_read_xlsx_rows'
    LANGUAGE C STRICT VOLATILE;

-- The workbook's sheets, in order: their numbers (from 1) and names
CREATE FUNCTION sc_xlsx_sheets(path text) RETURNS TABLE(sheet integer, name text)
    AS 'MODULE_PATHNAME', 'sc_xlsx_sheet_names'
    LANGUAGE C STRICT VOLATILE;
