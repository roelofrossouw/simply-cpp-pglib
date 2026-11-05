-- sc_pglib 1.16 -> 1.17: CSV files gzipped, written and read by name of their own.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.17'" to load this file. \quit

-- sc_csv's file gzipped, whatever path is called (sc_csv gzips only a path ending in .gz).
--   SELECT sc_csvgz('/var/www/data/output/gl.csv.gz', 'select * from gl');
CREATE FUNCTION sc_csvgz(path text, query text, delimiter text DEFAULT ',') RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csvgz_query'
    LANGUAGE C STRICT VOLATILE;

-- A gzipped CSV file read as sc_read_csv reads one: an error when it isn't gzipped (sc_read_csv takes
-- any file, compressed or not).
--   SELECT * FROM sc_read_csvgz('/var/www/data/input/prices.csv.gz', 2) AS x(a text, b numeric);
CREATE FUNCTION sc_read_csvgz(path text, from_row integer DEFAULT 1, delimiter text DEFAULT '', encoding text DEFAULT '') RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'sc_read_csvgz_rows'
    LANGUAGE C STRICT VOLATILE;
