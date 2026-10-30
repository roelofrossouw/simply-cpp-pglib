-- sc_pglib 1.6 -> 1.7: CSV files written from a query and read as rows.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.7'" to load this file. \quit

-- A query's rows as a CSV file on the server, the columns' names first: numbers as numbers,
-- booleans as TRUE/FALSE, dates and times as ISO text, NULL as an empty field; returns how many
-- rows were written. As COPY to a file: superusers and pg_write_server_files only, an absolute
-- path; the file is made readable by all (0644).
CREATE FUNCTION sc_csv(path text, query text, delimiter text DEFAULT ',') RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csv_query'
    LANGUAGE C STRICT VOLATILE;

-- A CSV file's rows as rows, from from_row on (2 skips a row of headings), into the columns of the
-- call's column definition list, each named after its column (a, b, ... aa) and of any type its
-- text converts to; a column named "row" gets the row number. Empty rows are left out, empty fields
-- are NULL. The delimiter is found when not given: a comma, unless semicolons fit the file better.
--   SELECT * FROM sc_read_csv('/var/www/data/input/prices.csv', 2) AS x("row" int, a text, b numeric);
CREATE FUNCTION sc_read_csv(path text, from_row integer DEFAULT 1, delimiter text DEFAULT '') RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'sc_read_csv_rows'
    LANGUAGE C STRICT VOLATILE;
