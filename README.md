# simply-cpp-pglib

`sc-pglib` packages simply-cpp functions as a PostgreSQL extension. Install the
package on the database server, then enable it per database.

## Install

On the database server, register the simply-cpp apt repository and install the
package:

```bash
curl -fsSL https://apt.roelof.co.za/setup.sh | bash
apt install simply-cpp-pglib
```

Then create the extension in each database that should have it. That takes a
superuser, such as `postgres`:

```bash
sudo -u postgres psql -d mydb -c "CREATE EXTENSION sc_pglib;"
```

The package is built for one PostgreSQL version per Ubuntu release and depends on
it, so the server must run that version:

| Ubuntu | PostgreSQL | Package depends on |
|---|---|---|
| noble (24.04) | 16 | `postgresql-16` |
| resolute (26.04) | 18 | `postgresql-18` |
| jammy (22.04) | 16, from PGDG | `postgresql-16` (from [PGDG](https://apt.postgresql.org)) |

jammy is the exception: Ubuntu's own PostgreSQL there is 14, but the jammy
package targets PostgreSQL 16 from the PGDG repository, which is what our jammy
database servers run. A server with another PostgreSQL version needs a build
against that version; see Building.

## Usage

```sql
CREATE EXTENSION sc_pglib;

SELECT sc_base64_encode('Hello World!');                              -- SGVsbG8gV29ybGQh
SELECT sc_base64_encode('\x00ff10e9'::bytea);                         -- AP8Q6Q==
SELECT convert_from(sc_base64_decode('SGVsbG8gV29ybGQh'), 'UTF8');    -- Hello World!
```

The base64 functions wrap `sc::base64`:

| Function | Returns | Notes |
|---|---|---|
| `sc_base64_encode(data bytea)` | `text` | |
| `sc_base64_encode(data text)` | `text` | encodes the text's bytes in the database encoding |
| `sc_base64_decode(encoded text)` | `bytea` | invalid base64 raises an error |

All are `IMMUTABLE STRICT PARALLEL SAFE`: a `NULL` argument gives `NULL`.
`DROP EXTENSION sc_pglib` removes them again.

## Redis

`sc_pglib` 1.1 talks to Redis (standalone or Cluster) through `sc::redis`, mainly
so triggers can keep Redis up to date for other programs to read. Connection
settings are a foreign server of the `sc_redis` wrapper, as `dblink` does it:

```sql
CREATE SERVER cache FOREIGN DATA WRAPPER sc_redis OPTIONS (servers 'redis1;redis2:6380');
-- Only for a Redis that needs a password; per role, or FOR PUBLIC:
CREATE USER MAPPING FOR PUBLIC SERVER cache OPTIONS (password 'secret');
-- Other roles need USAGE on the server:
GRANT USAGE ON FOREIGN SERVER cache TO app;
```

| Server option | Default | |
|---|---|---|
| `servers` | required | one or more `host[:port]`, separated by `;` (port 6379 by default) |
| `connect_timeout_ms` | `1000` | |
| `timeout_ms` | `1000` | how long a command waits for its reply |
| `retry_interval_ms` | `10000` | after Redis was unreachable, how long calls skip it |
| `on_error` | `warning` | `warning`: a failure is a WARNING and the call returns `false`/`NULL`; `error`: it raises an ERROR |

| Function | Returns |
|---|---|
| `sc_redis_set(server, key, value)` | `boolean`: stored |
| `sc_redis_get(server, key)` | `text`, `NULL` when there's no such key |
| `sc_redis_del(server, key)` | `bigint`: keys removed |
| `sc_redis_hset(server, key, field, value)` | `boolean`: stored |
| `sc_redis_hget(server, key, field)` | `text`, `NULL` when there's no such field |

A trigger keeping a hash per row up to date:

```sql
CREATE FUNCTION vehicle_to_redis() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        PERFORM sc_redis_del('cache', 'vehicle:' || OLD.id);
        RETURN OLD;
    END IF;
    PERFORM sc_redis_hset('cache', 'vehicle:' || NEW.id, 'name', NEW.name);
    RETURN NEW;
END $$;

CREATE TRIGGER vehicle_to_redis AFTER INSERT OR UPDATE OR DELETE ON vehicle
    FOR EACH ROW EXECUTE FUNCTION vehicle_to_redis();
```

How it behaves:

- **One connection per session.** Each database session (backend process) connects
  on its first call to a server and keeps that connection, so a call costs one
  round trip. A connection that broke while idle is replaced transparently.
  Changing the server's or user mapping's options makes the next call reconnect.
- **Not transactional.** A write happens at once and stays even if the
  transaction rolls back.
- **Never stuck on an outage.** Calls time out after `timeout_ms`. When Redis
  can't be reached, the call warns once and the server is skipped (no waiting)
  for `retry_interval_ms`, so a bulk update doesn't wait on every row. With the
  default `on_error 'warning'` a trigger never fails the statement that fired it.
- Arguments are `STRICT`: a `NULL` key or value makes the call return `NULL`
  without doing anything.
- Existing databases get these with `ALTER EXTENSION sc_pglib UPDATE`.

## How it fits together

- The module (`sc_pglib.so`, or `sc_pglib.dylib` on macOS) is a PostgreSQL
  loadable module using the version-1 calling convention: `src/sc_pglib.cpp`
  (base64) and `src/sc_redis.cpp` (Redis), linking `sc::sc-redis` and
  `sc::sc-core` statically and hiredis dynamically. `src/pg_glue.h` has the rules
  for mixing C++ and PostgreSQL: C++ exceptions never reach PostgreSQL, and
  nothing that can raise a PostgreSQL error runs while a C++ object is alive,
  because `ereport()` `longjmp()`s past destructors.
- `sql/sc_pglib.control` and the `sql/sc_pglib--*.sql` scripts are what `CREATE
  EXTENSION` reads: `sc_pglib--1.0.sql` (base64) and the `1.0--1.1` update
  (Redis). They and the module install into the server's own directories
  (`pg_config --sharedir`/extension and `--pkglibdir`).
- The extension version (`1.1`) is separate from the package version. A package
  update replaces the module in place; adding or changing SQL objects needs a new
  extension version with an update script (`sc_pglib--1.1--1.2.sql`), after which
  databases run `ALTER EXTENSION sc_pglib UPDATE`.

## Building

An extension is built for one PostgreSQL major version: the one whose *server*
`pg_config` is used. On Ubuntu that's fixed per release (`SC_PGLIB_PG_MAJOR`,
see the table above), and `postgresql-server-dev-<version>` is installed if it's
missing; jammy's build server has the PGDG repository for it, pinned so it never
upgrades Ubuntu packages on its own. Elsewhere (macOS) it's the newest server
found, such as Homebrew's `postgresql@<version>`. Pick another with
`-DSC_PGLIB_PG_MAJOR=<version>` or `-DSC_PGLIB_PG_CONFIG=/path/to/pg_config`. The
`.deb` depends on `postgresql-<version>` to match.

On macOS, installing is off by default (`SC_PGLIB_INSTALL`), since the server's
directories belong to Homebrew and `install.sh` installs with `sudo`.

## Testing

`test-extension` starts a throwaway PostgreSQL cluster (Unix socket only, in a
temporary directory) and checks the functions, without installing anything. With
PostgreSQL 18 or later it loads the extension with a real `CREATE EXTENSION`,
staged through `extension_control_path`, and also checks updating from 1.0;
older servers run the extension scripts directly. It is skipped where it can't
run: without the server binaries (`initdb`, `pg_ctl`), or as root.

The Redis checks always cover option validation, an unreachable server (one
warning, then fast skips; `on_error 'error'`) and permissions. With
`SC_REDIS_DEMO_SERVER` set they also run against that Redis, including a trigger,
and with `SC_REDIS_TEST_PASSWORD` a password-protected server through a user
mapping (both come from `scripts/test.env` in the suite).
