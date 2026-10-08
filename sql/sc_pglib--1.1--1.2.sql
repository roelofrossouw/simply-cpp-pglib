-- sc_pglib 1.1 -> 1.2: several Redis hash fields at once, as jsonb.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.2'" to load this file. \quit

-- {"field": "value", ...} for the requested fields, null for fields that don't exist.
CREATE FUNCTION sc_redis_hmget(server text, key text, fields text[]) RETURNS jsonb
    AS 'MODULE_PATHNAME', 'sc_redis_hmget'
    LANGUAGE C STRICT VOLATILE;

-- Every field of the hash; {} when there's no such key.
CREATE FUNCTION sc_redis_hgetall(server text, key text) RETURNS jsonb
    AS 'MODULE_PATHNAME', 'sc_redis_hgetall'
    LANGUAGE C STRICT VOLATILE;
