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

This first version is a proof of concept wrapping `sc::base64`:

| Function | Returns | Notes |
|---|---|---|
| `sc_base64_encode(data bytea)` | `text` | |
| `sc_base64_encode(data text)` | `text` | encodes the text's bytes in the database encoding |
| `sc_base64_decode(encoded text)` | `bytea` | invalid base64 raises an error |

All are `IMMUTABLE STRICT PARALLEL SAFE`: a `NULL` argument gives `NULL`.
`DROP EXTENSION sc_pglib` removes them again.

## How it fits together

- `src/sc_pglib.cpp` is a PostgreSQL loadable module (`sc_pglib.so`, or
  `sc_pglib.dylib` on macOS) using the version-1 calling convention. It links
  `sc::sc-core` statically. C++ exceptions never reach PostgreSQL: they become an
  `ERROR`, raised only after the C++ objects are gone, because `ereport()`
  `longjmp()`s past destructors.
- `sql/sc_pglib.control` and `sql/sc_pglib--1.0.sql` are what `CREATE EXTENSION`
  reads. They and the module install into the server's own directories
  (`pg_config --sharedir`/extension and `--pkglibdir`).
- The extension version (`1.0`) is separate from the package version. A package
  update replaces the module in place; adding or changing SQL functions needs a
  new extension version with an upgrade script (`sc_pglib--1.0--1.1.sql`), after
  which databases run `ALTER EXTENSION sc_pglib UPDATE`.

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
staged through `extension_control_path`; older servers run the extension script
directly. It is skipped where it can't run: without the server binaries
(`initdb`, `pg_ctl`), or as root.
