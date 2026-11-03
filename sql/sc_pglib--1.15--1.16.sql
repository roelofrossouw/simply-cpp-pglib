-- sc_pglib 1.15 -> 1.16: 1web called by name (sc_oneweb), replacing libpgru's fetch_curl.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.16'" to load this file. \quit

-- 1web runs its function name (^[a-z][a-z0-9_]{0,63}$) with params and answers its data: POST <url><name>, the
-- params as the JSON body, signed with the key both sides share (HMAC-SHA256 in X-OneWeb-Signature). The url
-- and key come from the settings file sc_pglib.oneweb_config (default /etc/sc-pglib/oneweb.ini), the section
-- [database <current_database()>] or else [default]: never from the database, which may be a restored copy
-- of another. timeout_ms (1000 to 600000) is this call's limit instead of the file's. 1web's {"ok": false,
-- "error": ...}, another status, an answer that isn't 1web's, a timeout or a failed connection is an ERROR
-- naming the call, or a WARNING and NULL with on_error = warning. A cancel stops the transfer.
CREATE FUNCTION sc_oneweb(name text, params jsonb DEFAULT '{}', timeout_ms integer DEFAULT NULL) RETURNS jsonb
    AS 'MODULE_PATHNAME', 'sc_oneweb'
    LANGUAGE C VOLATILE PARALLEL UNSAFE;

-- The settings sc_oneweb() would use in this database, without the key: the file, the section, the url,
-- where the key comes from and how long it is, the limits and on_error. An ERROR when it isn't set up.
CREATE FUNCTION sc_oneweb_config() RETURNS jsonb
    AS 'MODULE_PATHNAME', 'sc_oneweb_config'
    LANGUAGE C STABLE PARALLEL UNSAFE;

-- Only the owner (a superuser): 1web grants SECURITY DEFINER wrappers of its own.
REVOKE EXECUTE ON FUNCTION sc_oneweb(text, jsonb, integer) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION sc_oneweb_config() FROM PUBLIC;
