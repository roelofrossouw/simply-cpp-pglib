-- sc_pglib 1.9 -> 1.10: a ZIP archive of several CSV files, one per query.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.10'" to load this file. \quit

-- A CSV file per query in a ZIP archive at path, as sc_xlsx makes a sheet per query: files is a JSON
-- object of names (relative paths, '/' between folders) and their queries. json keeps the files in the
-- order written, jsonb sorts its keys (shorter first). Returns how many rows were written, all files
-- together. Give the files as json or jsonb explicitly: an untyped string is taken as a query.
--   SELECT sc_csvzip('/var/www/data/output/export.zip', '{"items.csv": "select * from items", "prices.csv": "select * from prices"}'::json);
CREATE FUNCTION sc_csvzip(path text, files json, delimiter text DEFAULT ',') RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csvzip_files'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_csvzip(path text, files jsonb, delimiter text DEFAULT ',') RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csvzip_files'
    LANGUAGE C STRICT VOLATILE;
