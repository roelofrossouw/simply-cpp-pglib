-- sc_pglib 1.2 -> 1.3: Redis sets.
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.3'" to load this file. \quit

-- Adds one member, or several; returns how many were new.
CREATE FUNCTION sc_redis_sadd(server text, key text, member text) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_redis_sadd'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_redis_sadd(server text, key text, members text[]) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_redis_sadd_many'
    LANGUAGE C STRICT VOLATILE;

-- Removes one member, or several; returns how many were there.
CREATE FUNCTION sc_redis_srem(server text, key text, member text) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_redis_srem'
    LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sc_redis_srem(server text, key text, members text[]) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_redis_srem_many'
    LANGUAGE C STRICT VOLATILE;

-- How many members the set has; 0 when there's no such key.
CREATE FUNCTION sc_redis_scard(server text, key text) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_redis_scard'
    LANGUAGE C STRICT VOLATILE;

-- The members, sorted; {} when there's no such key.
CREATE FUNCTION sc_redis_smembers(server text, key text) RETURNS text[]
    AS 'MODULE_PATHNAME', 'sc_redis_smembers'
    LANGUAGE C STRICT VOLATILE;

-- Whether member is in the set.
CREATE FUNCTION sc_redis_sismember(server text, key text, member text) RETURNS boolean
    AS 'MODULE_PATHNAME', 'sc_redis_sismember'
    LANGUAGE C STRICT VOLATILE;
