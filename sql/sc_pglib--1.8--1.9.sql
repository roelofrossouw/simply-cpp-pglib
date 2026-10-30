-- sc_pglib 1.8 -> 1.9: CSV files in a ZIP archive, written and read.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.9'" to load this file. \quit

-- sc_csv's file, in a ZIP archive at path as its one entry (deflated): entry, or the archive's name
-- with .csv (/var/www/data/output/prices.zip holds prices.csv). An existing archive is replaced.
-- Returns how many rows were written. As COPY to a file: pg_write_server_files, an absolute path;
-- the archive is made readable by all (0644).
CREATE FUNCTION sc_csvzip(path text, query text, entry text DEFAULT '', delimiter text DEFAULT ',') RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csvzip_query'
    LANGUAGE C STRICT VOLATILE;

-- A CSV file in a ZIP archive, read as sc_read_csv reads one: its rows from from_row on, into the
-- columns of the call's column definition list (a, b, ... aa, and "row").
--   SELECT * FROM sc_read_csvzip('/var/www/data/input/prices.zip', 'prices.csv', 2) AS x(a text, b numeric);
CREATE FUNCTION sc_read_csvzip(path text, entry text, from_row integer DEFAULT 1, delimiter text DEFAULT '') RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'sc_read_csvzip_rows'
    LANGUAGE C STRICT VOLATILE;
