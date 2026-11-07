-- sc_pglib 1.17 -> 1.18: what 1web's plperlu and plpython3u functions did, natively, so the database can
-- drop the untrusted languages (they run as the postgres user, for whoever calls them).
\echo Use "ALTER EXTENSION sc_pglib UPDATE TO '1.18'" to load this file. \quit

-- HTML character references as Perl's HTML::HTML5::Entities: html_encode() and html_decode_work().
--   SELECT sc_html_encode('Café <b> & €5');        -- Café &lt;b&gt; &amp; &euro;5
--   SELECT sc_html_decode('caf&eacute; &lt;3 &#169'); -- café <3 ©
CREATE FUNCTION sc_html_encode(text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_html_encode'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION sc_html_decode(text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_html_decode'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Every byte but A-Z a-z 0-9 - . _ ~ as %XX (UTF-8), as URI::Encode's uri_encode with encode_reserved.
--   SELECT sc_uri_encode('a b/é');   -- a%20b%2F%C3%A9
CREATE FUNCTION sc_uri_encode(text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_uri_encode'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- The address' name in reverse DNS (its PTR record, through the server's resolver); NULL when it has
-- none. IPv4 and IPv6.
--   SELECT sc_reverse_dns('8.8.8.8');   -- dns.google
CREATE FUNCTION sc_reverse_dns(inet) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_reverse_dns'
    LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

-- A bounce's fields, as correspondence.read_dsn() read them: every field of its delivery-status parts,
-- and Message-ID, To and Received of the returned message's headers. Read as sc_read_csv reads.
--   SELECT sc_read_dsn('/var/www/data/output/dsn/1234.eml') ->> 'Status';
CREATE FUNCTION sc_read_dsn(path text) RETURNS jsonb
    AS 'MODULE_PATHNAME', 'sc_read_dsn'
    LANGUAGE C VOLATILE STRICT;

-- A text file's content, as it is (in the database's encoding: an error when it isn't), and one
-- written, or appended to; the bytes written. Where sc_read_csv reads and sc_csv writes.
--   SELECT sc_read_text('/var/www/data/output/tamedia/tahome/data/json/x.json')::jsonb;
--   SELECT sc_write_text('/var/www/data/output/hr/mailparams.json', '{"a": 1}');
CREATE FUNCTION sc_read_text(path text) RETURNS text
    AS 'MODULE_PATHNAME', 'sc_read_text'
    LANGUAGE C VOLATILE STRICT;
CREATE FUNCTION sc_write_text(path text, content text, append boolean DEFAULT false) RETURNS bigint
    AS 'MODULE_PATHNAME', 'sc_write_text'
    LANGUAGE C VOLATILE;

-- An HTTP GET of a URL under one of sc_pglib.http_urls (postgresql.conf; the default holds what 1web's
-- Python functions fetched): params added to its query, headers sent; no redirects followed.
-- {status, content_type, body, json (the body parsed, else null)}; an error when there's no answer.
--   SELECT sc_http_get('https://csv.webfleet.com/extern', '{"action": "x"}') -> 'json';
CREATE FUNCTION sc_http_get(url text, params jsonb DEFAULT NULL, headers jsonb DEFAULT NULL, timeout_ms integer DEFAULT NULL) RETURNS jsonb
    AS 'MODULE_PATHNAME', 'sc_http_get'
    LANGUAGE C VOLATILE;

-- The file and network ones only for the roles they're granted to.
REVOKE EXECUTE ON FUNCTION sc_read_dsn(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION sc_read_text(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION sc_write_text(text, text, boolean) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION sc_http_get(text, jsonb, jsonb, integer) FROM PUBLIC;
