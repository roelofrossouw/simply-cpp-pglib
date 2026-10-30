-- sc_pglib 1.7 -> 1.8: folders made under /var/www/data/output.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.8'" to load this file. \quit

-- mkdir -p, only in /var/www/data/output and below (compiled in): an absolute path without empty,
-- "." or ".." parts and no symbolic link on the way; a folder that's there already is fine. Folders
-- made are 0755. Returns the folder without a trailing slash.
CREATE FUNCTION sc_mkdir(path text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_mkdir'
    LANGUAGE C STRICT VOLATILE;

-- Only the roles it's granted to.
REVOKE ALL ON FUNCTION sc_mkdir(text) FROM PUBLIC;
