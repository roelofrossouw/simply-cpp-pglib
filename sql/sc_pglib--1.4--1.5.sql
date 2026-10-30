-- sc_pglib 1.4 -> 1.5: Excel workbooks (.xlsx) read.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.5'" to load this file. \quit

-- An .xlsx file on the server as json: {"Sheet": [[cell, ...], ...], ...}, a sheet's rows from the
-- first, each from column A to its last cell set; numbers, text, true/false or null, a date or time
-- as ISO text. As COPY from a file: superusers and pg_read_server_files only, an absolute path.
CREATE FUNCTION sc_read_xlsx(path text) RETURNS json
    AS 'MODULE_PATHNAME', 'sc_read_xlsx'
    LANGUAGE C STRICT VOLATILE;

-- One sheet's rows (its name ignoring case), as a json array.
CREATE FUNCTION sc_read_xlsx(path text, sheet text) RETURNS json
    AS 'MODULE_PATHNAME', 'sc_read_xlsx'
    LANGUAGE C STRICT VOLATILE;
