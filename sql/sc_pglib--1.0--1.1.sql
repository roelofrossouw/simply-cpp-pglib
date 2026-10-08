-- sc_pglib 1.0 -> 1.1: Redis (sc::redis).
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.1'" to load this file. \quit

-- Connection settings are foreign servers of the sc_redis wrapper, as for dblink:
--   CREATE SERVER cache FOREIGN DATA WRAPPER sc_redis OPTIONS (servers 'redis1;redis2:6380');
--   CREATE USER MAPPING FOR PUBLIC SERVER cache OPTIONS (password '...');  -- only with a password
-- Server options: servers (required), connect_timeout_ms (1000), timeout_ms (1000),
-- retry_interval_ms (10000), on_error ('warning' or 'error', default 'warning').
CREATE FUNCTION sc_redis_validator(text[], oid) RETURNS void
    AS 'MODULE_PATHNAME', 'sc_redis_validator'
    LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER sc_redis VALIDATOR sc_redis_validator;

-- Each takes the foreign server's name first, and needs USAGE on it. Writes happen at once (not
-- transactional). With on_error 'warning' a failure is a WARNING and the result is false or NULL.
CREATE FUNCTION sc_redis_set(server text, key text, value text) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_redis_set'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_redis_get(server text, key text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_redis_get'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_redis_del(server text, key text) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_redis_del'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_redis_hset(server text, key text, field text, value text) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_redis_hset'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_redis_hget(server text, key text, field text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_redis_hget'
    LANGUAGE C STRICT VOLATILE;
