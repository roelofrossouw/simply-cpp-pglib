-- sc_pglib 1.11 -> 1.12: CSV read in an encoding and from compressed files, written gzipped and added
-- to an archive; files moved and listed in /var/www/data/output; the server's finished logs removed.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.12'" to load this file. \quit

-- The readers take an encoding (a PostgreSQL name such as 'WIN1251' or 'windows-1251'; '' is UTF-8),
-- converted to the database's. A compressed file (gzip, bzip2, xz, zstd) is read uncompressed.
DROP FUNCTION sc_read_csv(text, integer, text);
CREATE FUNCTION sc_read_csv(path text, from_row integer DEFAULT 1, delimiter text DEFAULT '', encoding text DEFAULT '') RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'sc_read_csv_rows'
    LANGUAGE C STRICT VOLATILE;

DROP FUNCTION sc_read_csvzip(text, text, integer, text);
CREATE FUNCTION sc_read_csvzip(path text, entry text, from_row integer DEFAULT 1, delimiter text DEFAULT '', encoding text DEFAULT '') RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'sc_read_csvzip_rows'
    LANGUAGE C STRICT VOLATILE;

-- sc_csv writes a path ending in .gz gzipped (nothing to change here). sc_csvzip takes append: the
-- archive's other entries kept, one of the same name replaced.
DROP FUNCTION sc_csvzip(text, text, text, text);
CREATE FUNCTION sc_csvzip(path text, query text, entry text DEFAULT '', delimiter text DEFAULT ',', append boolean DEFAULT false) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csvzip_query'
    LANGUAGE C STRICT VOLATILE;

DROP FUNCTION sc_csvzip(text, json, text);
CREATE FUNCTION sc_csvzip(path text, files json, delimiter text DEFAULT ',', append boolean DEFAULT false) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csvzip_files'
    LANGUAGE C STRICT VOLATILE;

DROP FUNCTION sc_csvzip(text, jsonb, text);
CREATE FUNCTION sc_csvzip(path text, files jsonb, delimiter text DEFAULT ',', append boolean DEFAULT false) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_csvzip_files'
    LANGUAGE C STRICT VOLATILE;

-- mv in /var/www/data/output and below (as sc_mkdir: no symbolic link on the way): a file or folder
-- renamed, or moved into a folder when to ends with a slash or is one. Returns where it is now.
CREATE FUNCTION sc_move("from" text, "to" text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_move'
    LANGUAGE C STRICT VOLATILE;

-- find -type f in a folder in /var/www/data/output and the folders in it, oldest first: those
-- modified longer than older_than ago (null: all), at most max (null: all).
CREATE FUNCTION sc_list_files(folder text, older_than interval DEFAULT NULL, max integer DEFAULT NULL)
    RETURNS TABLE(path text, modified timestamptz, size bigint)
    AS 'MODULE_PATHNAME', 'sc_list_files'
    LANGUAGE C VOLATILE;

-- One of the server's log files, by its name in log_directory (as pg_ls_logdir() gives it), removed;
-- never the one it writes to now. false when it isn't there.
CREATE FUNCTION sc_remove_log(name text) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_remove_log'
    LANGUAGE C STRICT VOLATILE;

-- Only the roles they're granted to.
REVOKE ALL ON FUNCTION sc_move(text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION sc_list_files(text, interval, integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION sc_remove_log(text) FROM PUBLIC;
