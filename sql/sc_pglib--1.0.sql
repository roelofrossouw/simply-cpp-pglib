-- sc_pglib 1.0: simply-cpp functions for PostgreSQL.
\echo Use "CREATE EXTENSION sc_pglib" to load this file. \quit

-- Base64 (sc::base64). A text argument is encoded as its bytes in the database encoding.
CREATE FUNCTION sc_base64_encode(data bytea) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_base64_encode'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION sc_base64_encode(data text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_base64_encode'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Raises an error for input that isn't valid base64.
CREATE FUNCTION sc_base64_decode(encoded text) RETURNS bytea
    AS 'MODULE_PATHNAME', 'sc_base64_decode'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
