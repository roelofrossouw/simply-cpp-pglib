-- sc_pglib 1.13 -> 1.14: Firebase foreign tables (read-only): a path and its key levels as rows.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.14'" to load this file. \quit

-- A foreign table of an sc_firebase server maps a path under the server's root and the key levels
-- under it to rows: every object node at the last level is a row, the keys on the way its key
-- columns, its fields the other columns (by name, or the column option field: 'gps/lat').
--   CREATE FOREIGN TABLE device (account_id text, imei text, lat float8, lng float8, online int)
--       SERVER app_db OPTIONS (path '', keys 'account_id,imei');
--   SELECT * FROM device WHERE account_id = '42';   -- fetches <root>/42 only
-- A WHERE setting the leading key columns with = (a constant or a parameter) narrows what's
-- fetched; otherwise the whole path is read once per scan. json and jsonb columns get the field
-- as JSON; an empty string is NULL in a column that isn't text-like. Writing comes later.
CREATE FUNCTION sc_firebase_fdw_handler() RETURNS fdw_handler
    AS 'MODULE_PATHNAME', 'sc_firebase_fdw_handler'
    LANGUAGE C STRICT;

ALTER FOREIGN DATA WRAPPER sc_firebase HANDLER sc_firebase_fdw_handler;
