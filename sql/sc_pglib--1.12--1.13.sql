-- sc_pglib 1.12 -> 1.13: a Firebase Realtime Database (sc::firebase), with a service account.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.13'" to load this file. \quit

-- Settings are foreign servers of the sc_firebase wrapper, as for sc_redis:
--   CREATE SERVER app_db FOREIGN DATA WRAPPER sc_firebase
--       OPTIONS (url 'https://example-app.firebaseio.com', root 'devices');
--   CREATE USER MAPPING FOR app_role SERVER app_db OPTIONS (credentials_file '/etc/postgresql/firebase.json');
--   -- or OPTIONS (service_account '{"type": "service_account", ...}'), the key's JSON itself
-- Server options: url (required), root (a path every call's path is under; default the database's
-- root), timeout_ms (30000), on_error ('error' or 'warning', default 'error'). PostgreSQL shows a
-- user mapping's options only to its owner and superusers; a credentials_file must be readable by
-- the server's postgres user.
CREATE FUNCTION sc_firebase_validator(text[], oid) RETURNS void
    AS 'MODULE_PATHNAME', 'sc_firebase_validator'
    LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER sc_firebase VALIDATOR sc_firebase_validator;

-- Each takes the foreign server's name first, and needs USAGE on it. Paths are '/'-separated,
-- under the server's root. Writes happen at once (not transactional). With on_error 'warning' a
-- failure is a WARNING and the result is NULL (false for set, update and remove).
CREATE FUNCTION sc_firebase_get(server text, path text, shallow boolean DEFAULT false) RETURNS jsonb
    AS 'MODULE_PATHNAME', 'sc_firebase_get'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_firebase_keys(server text, path text) RETURNS text[]
    AS 'MODULE_PATHNAME', 'sc_firebase_keys'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_firebase_set(server text, path text, value jsonb) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_firebase_set'
    LANGUAGE C STRICT VOLATILE;

-- Each member of values is written under path; a member's name may be a path ('42/online'), so
-- one call changes several places, all or none; null removes.
CREATE FUNCTION sc_firebase_update(server text, path text, "values" jsonb) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_firebase_update'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_firebase_push(server text, path text, value jsonb) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_firebase_push'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_firebase_remove(server text, path text) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_firebase_remove'
    LANGUAGE C STRICT VOLATILE;

-- In one update of the paths' common parent: never in both places or neither. false when there's
-- nothing at "from", or (without overwrite) something at "to".
CREATE FUNCTION sc_firebase_move(server text, "from" text, "to" text, overwrite boolean DEFAULT false) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_firebase_move'
    LANGUAGE C STRICT VOLATILE;
