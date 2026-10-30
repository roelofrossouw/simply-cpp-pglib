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

## Excel workbooks

`sc_xlsx` writes queries' rows as an `.xlsx` file on the database server
(`sc::office::excel`), as `COPY ... TO 'file'` writes a CSV one, and returns how
many rows it wrote:

```sql
SELECT sc_xlsx('/var/www/data/output/result.xlsx', 'select * from a');             -- the sheet "Sheet1"
SELECT sc_xlsx('/var/www/data/output/result.xlsx', 'select * from a', 'Results');  -- named
SELECT sc_xlsx('/var/www/data/output/result.xlsx',
               '{"Subscriptions": "select * from a", "Totals": "select * from b"}'::json);
```

| Function | Returns | Notes |
|---|---|---|
| `sc_xlsx(path text, query text)` | `bigint` | one sheet, "Sheet1" |
| `sc_xlsx(path text, query text, sheet text)` | `bigint` | one sheet, named |
| `sc_xlsx(path text, sheets json)` | `bigint` | a sheet per query, in the order written |
| `sc_xlsx(path text, sheets jsonb)` | `bigint` | the same; jsonb sorts its keys (shorter first) |
| `sc_xlsx(path text, query text, sheet text, formatting json)` | `bigint` | one sheet, formatted (below); since 1.11 |
| `sc_xlsx(path text, sheets json, formatting json)` | `bigint` | a sheet per query, `formatting` for all of them, a sheet's own over it |
| `sc_xlsx(path text, sheets jsonb, formatting json)` | `bigint` | the same |

- The rule is COPY's: only superusers and roles with the privileges of
  `pg_write_server_files` may, and the path is absolute. The file is written as
  the server's operating system user (replacing one that's there) and made
  readable by all (0644), as PostgreSQL's own files aren't.
- Each sheet has the columns' names in a bold first row that stays in view
  (frozen), then a row per row. Numbers (integers, `float`, `numeric`, `oid`) are
  numbers, booleans booleans; `date`, `timestamp`, `timestamptz` (in the
  session's time zone) and `time` are Excel dates and times shown as
  `yyyy-mm-dd`, `yyyy-mm-dd hh:mm:ss` and `hh:mm:ss`; `NULL` is an empty cell;
  anything else is its text. Columns are as wide as their contents (8 to 80
  characters).
- A sheet name is checked as Excel does (1 to 31 characters, none of `: \ / ?
  * [ ]`); a query that returns no rows set (not a `SELECT`), or fails, is an
  error, and no file is written.
- Give the sheets as `json` or `jsonb` explicitly: an untyped string is taken
  as a query.
- The rows are held in memory until the file is written.

### Formatting a sheet

`formatting` is a JSON object of how a sheet looks. Without it (or with `{}`) a sheet looks as
above: a bold header, frozen, dates in ISO, columns as wide as their contents.

```sql
SELECT sc_xlsx('/var/www/data/output/invoices.xlsx',
               'select number, customer, issued, amount, paid from invoices', 'Invoices',
               '{"freeze_panes": "B2",
                 "autofilter": true,
                 "style": {"font": "Arial", "font_size": 10},
                 "header_style": {"background": "#1f4e79", "color": "white", "border_bottom": "medium"},
                 "columns": {"amount": "currency", "customer": {"width": 40}},
                 "format": {"C": {"number_format": "dd mmm yyyy", "align": "center"},
                            "E2:E999": {"color": "#00b050", "bold": true},
                            "1": {"height": 24, "valign": "center"}}}');
```

The sheet's own settings:

| Key | Value | Default |
|---|---|---|
| `header` | `true` or `false`: the columns' names as the first row | `true` |
| `freeze_panes` | the first cell that scrolls: `"B2"` keeps row 1 and column A in view, `"A3"` two rows, `"B1"` one column; `false` freezes nothing | `"A2"` with a header, else nothing |
| `autofilter` | `true`: filter buttons on the header, over all the rows; a range (`"A1:F20"`) for that one; `false` | `false` |
| `auto_width` | `true`: columns as wide as their contents (8 to 80 characters); `false`: Excel's default width, except where a width is given | `true` |
| `date_format`, `datetime_format`, `time_format` | the number format of `date`, `timestamp`/`timestamptz` and `time` columns | `"yyyy-mm-dd"`, `"yyyy-mm-dd hh:mm:ss"`, `"hh:mm:ss"` |
| `currency_symbol` | the symbol the `currency` and `accounting` formats show, up to 10 characters | `"R "` |
| `style` | a style (below) for every cell written | |
| `header_style` | a style for the header, over the bold it has | |
| `columns` | `{"column name": style}`: the query's columns by name, their data (not the header) | |
| `format` | `{"place": style}`: cells, columns or rows by Excel's names | |

A **place** in `format` is a cell (`"B2"`), a range (`"F2:F999"`), whole columns (`"C"`, `"C:E"`)
or whole rows (`"1"`, `"2:5"`), in any case. Places count the header: with one, the data starts in
row 2. Only what's written is styled: a place reaching past the last row or column stops there, so
`"F2:F999"` and `"F"` (with its header) style column F however many rows there are. `columns`
follows the query's own names, so it keeps up when the columns move; it styles the data only.

A **style** is a number format on its own (`"currency"`), or an object of what it changes; only
what it names changes, so styles add up:

| Key | Value |
|---|---|
| `number_format` | a name below, or Excel's own code (`"0.0%"`, `"#,##0 \"kg\""`, `"[Red]-0.00;0.00"`) |
| `font` | the font's family: `"Arial"`, `"Courier New"` (1 to 31 characters); Calibri otherwise |
| `font_size` | in points, 1 to 409 (Excel's default is 11) |
| `bold`, `italic`, `strike` | `true` or `false` |
| `underline` | `true` (single), `false`, `"double"`, `"single_accounting"`, `"double_accounting"` |
| `color` | the text's colour: `"#c00000"`, `"#c00"` or a CSS name (`"red"`, `"darkblue"`, `"lightgray"`) |
| `background` | a solid fill, the same way; `null` for none |
| `border` | all four sides: a line (`"thin"`), `true` (thin), `false` (none), or `{"line": "medium", "color": "#999999"}` |
| `border_top`, `border_bottom`, `border_left`, `border_right` | one side, the same way |
| `align` | `"left"`, `"center"`, `"right"`, `"fill"`, `"justify"`, `"center_across"` (across this and the empty cells to its right), `"distributed"`, `"general"` (text left, numbers right) |
| `valign` | `"top"`, `"center"`, `"bottom"`, `"justify"`, `"distributed"` |
| `wrap` | `true`: text wraps onto more lines |
| `indent` | 0 to 250 steps (about three characters each) |
| `rotation` | degrees, -90 (reading down) to 90 (reading up) |
| `width` | columns only (`"C"`, `"C:E"`, or a name in `columns`): characters, 0 to 255, or `"auto"` |
| `height` | rows only (`"1"`, `"2:5"`): points, 0 to 409 |
| `hidden` | whole columns or rows: `true` hides them |

Lines are `thin`, `medium`, `thick`, `hair`, `dotted`, `dashed`, `double`, `dash_dot`,
`dash_dot_dot`, `slant_dash_dot`, `medium_dashed`, `medium_dash_dot`, `medium_dash_dot_dot` and
`none`.

The **number formats** by name:

| Name | Code | Shows |
|---|---|---|
| `general` | General | as Excel decides |
| `text` | `@` | as text |
| `integer` | `0` | 1235 |
| `thousands` | `#,##0` | 1,235 |
| `decimal` | `0.00` | 1234.50 |
| `number` | `#,##0.00` | 1,234.50 |
| `percent` | `0%` | 15% (of 0.15) |
| `currency` | `"R "#,##0.00;-"R "#,##0.00` | R 1,234.50 |
| `accounting` | Excel's accounting, with the symbol | R  1,234.50, the symbol lined up on the left |
| `date`, `datetime`, `time` | `yyyy-mm-dd`, `yyyy-mm-dd hh:mm:ss`, `hh:mm:ss` | 2026-10-30 |

Anything else is taken as Excel's code, so a misspelt name shows as text in the cell: check new
formats once in Excel. Separators and the currency's look follow the code, not the reader's
locale, except the `,` and `.` themselves, which Excel shows in the reader's own way.

**The order** is: what a sheet has by default (the bold header, date formats), then `style`, then
`header_style`, then `columns`, then `format`, each entry in the order written; a later one wins
where they meet. That's why `formatting` is `json`, which keeps the order, and not `jsonb`.

**Several sheets**: the `formatting` argument is for every sheet. A sheet whose value is an object
instead of a query has its query as `query` and its own keys beside it, over the common ones (its
`columns` and `format` entries are added after the common ones, so they win):

```sql
SELECT sc_xlsx('/var/www/data/output/month.xlsx',
               '{"Invoices": {"query": "select number, customer, amount from invoices",
                              "columns": {"amount": "currency"}},
                 "Payments": {"query": "select paid_on, reference, amount from payments",
                              "columns": {"amount": "currency", "reference": {"width": 30}},
                              "freeze_panes": "B2"},
                 "Notes": {"query": "select note from notes", "header": false, "freeze_panes": false}}'::json,
               '{"autofilter": true, "style": {"font": "Arial", "font_size": 10},
                 "header_style": {"background": "#ddebf7"}}');
```

A sheet's value can still be just its query, with only the common formatting. Mistakes (a key or
value that isn't known, a column the query doesn't have) are errors naming the sheet and the key,
and no file is written:

```
ERROR:  sc_xlsx: sheet "Payments": format.columns: the query has no column "amout" (it has paid_on, reference, amount)
```

`sc_read_xlsx` reads one back. A sheet's rows come back as rows, into the columns you name after the
sheet's column letters, each with the type you want; `row` is the row number:

```sql
-- The sheets, numbered
SELECT * FROM sc_xlsx_sheets('/var/www/data/input/prices.xlsx');   -- 1 | Prices, 2 | Notes

-- The first sheet from row 2 (under its headings): columns A, B and D
SELECT * FROM sc_read_xlsx('/var/www/data/input/prices.xlsx', 1, 2) AS x("row" int, a text, b numeric, d date);

-- By name (ignoring case), into a table
INSERT INTO prices (item, price)
SELECT a, b FROM sc_read_xlsx('/var/www/data/input/prices.xlsx', 'Prices', 2) AS x(a text, b numeric);

-- Every sheet as json: {"Prices": [["Item", "Price", "From"], ["Tea", 18.5, "2026-10-31"], [], ["Total", 18.5]], "Notes": [...]}
SELECT sc_read_xlsx('/var/www/data/input/prices.xlsx');
```

| Function | Returns | Notes |
|---|---|---|
| `sc_read_xlsx(path text, sheet text, from_row integer = 1)` | `SETOF record` | that sheet's rows from `from_row` on, empty ones left out; needs a column list `AS x(a text, ...)`: columns `a`, `b`, ... `aa` (any case) and `row`; an empty cell is `NULL`, a value that doesn't convert is an error naming the cell; since 1.6 (it was `json` before) |
| `sc_read_xlsx(path text, sheet integer, from_row integer = 1)` | `SETOF record` | the same, the sheet by its place (1 is the first) |
| `sc_xlsx_sheets(path text)` | `TABLE(sheet integer, name text)` | the sheets in the workbook's order |
| `sc_read_xlsx(path text)` | `json` | every sheet, in the workbook's order: `{"Sheet": [rows]}` |

A cell's value is a number, text or `true`/`false`; a number shown as a date or time reads as ISO text
(`2026-10-31`, `2026-10-31 14:05:00`, `14:05:00`), so a `date`, `timestamp` or `time` column takes it.

- Each sheet is an array of its rows from the first, each row an array of its cells from
  column A up to its last one set: plain arrays, no names repeated, so big sheets stay small.
  A row with nothing in it is `[]`, a cell with nothing in it `null`, and trailing empty
  rows are left out.
- A cell is a number (whole numbers without a fraction), text, `true`/`false` or `null`. A
  number shown with a date or time format is ISO text: `"2026-10-31"`, `"2026-10-31
  14:05:06"` or `"14:05:06"`. A formula gives the value Excel last calculated (`null` when
  it never was); an error such as `#DIV/0!` is its text.
- `json`, not `jsonb`, so the sheets keep their order; cast to `jsonb` to index into it.
- The rule is COPY's: only superusers and roles with the privileges of
  `pg_read_server_files` may, and the path is absolute.

## CSV files

`sc_csv` writes a query's rows as a CSV file on the database server, the columns' names first;
`sc_read_csv` reads one back as rows, as `sc_read_xlsx` reads a sheet (a CSV file has none). The
rules for paths and permissions are COPY's, as for `.xlsx`.

```sql
SELECT sc_csv('/var/www/data/output/result.csv', 'select * from a');          -- commas
SELECT sc_csv('/var/www/data/output/result.csv', 'select * from a', ';');     -- semicolons
SELECT * FROM sc_read_csv('/var/www/data/input/prices.csv', 2) AS x("row" int, a text, b numeric);
SELECT * FROM sc_read_csv('/var/www/data/input/prices.csv', delimiter => ';') AS x(a text, b text);

-- The same CSV in a ZIP archive (its one entry: result.csv here), and one read from an archive
SELECT sc_csvzip('/var/www/data/output/result.zip', 'select * from a');
SELECT * FROM sc_read_csvzip('/var/www/data/input/prices.zip', 'prices.csv', 2) AS x(a text, b numeric);
SELECT * FROM sc_read_csvzip('/var/www/data/input/efile.zip', 'Codes*.csv', 2) AS x(a text, b text);  -- a pattern
SELECT * FROM sc_read_csvzip('/var/www/data/input/cards.zip') AS x(a text, b text);  -- its only .csv

-- Several CSV files in one archive, a query each (json keeps their order, jsonb sorts them)
SELECT sc_csvzip('/var/www/data/output/export.zip',
                 '{"items.csv": "select * from items", "reports/prices.csv": "select * from prices"}'::json);

-- Gzipped (the path ends in .gz), read back as it is; another encoding; added to an archive
SELECT sc_csv('/var/www/data/output/dump/gl.csv.gz', 'select * from gl');
SELECT * FROM sc_read_csv('/var/www/data/input/efile.csv', 2, encoding => 'windows-1251') AS x(a text, b text);
SELECT sc_csvzip('/var/www/data/output/dump.zip', 'select * from more', 'more.csv', append => true);
```

| Function | Returns | |
|---|---|---|
| `sc_csv(path text, query text, delimiter text = ',')` | `bigint` | the rows written (the header aside), gzipped when `path` ends in `.gz` (since 1.12): numbers as numbers, booleans as `TRUE`/`FALSE`, dates and times as ISO text, `NULL` as an empty field, anything else as its text, quoted when it needs to be (RFC 4180) |
| `sc_read_csv(path text, from_row integer = 1, delimiter text = '', encoding text = '')` | `SETOF record` | the rows from `from_row` on, empty ones left out, into a column list `AS x(a text, ...)` as `sc_read_xlsx`'s; each field as written, converted to its column's type. The delimiter is found when empty: a comma, unless semicolons fit the file better (as Excel writes CSV where the decimal separator is a comma). `encoding` (since 1.12) is what the file is written in, as PostgreSQL names it (`WIN1251`, `windows-1251`, `LATIN1`; empty: UTF-8), converted to the database's and checked; a compressed file (gzip, bzip2, xz, zstd) is read uncompressed, whatever its name |
| `sc_csvzip(path text, query text, entry text = '', delimiter text = ',', append boolean = false)` | `bigint` | `sc_csv`'s file as the one entry of a ZIP archive at `path` (deflated, an existing one replaced): `entry`, or the archive's name with `.csv`. With `append` (since 1.12) an existing archive's other entries are kept, and one of the same name replaced |
| `sc_csvzip(path text, files json or jsonb, delimiter text = ',', append boolean = false)` | `bigint` | a CSV file per query, as `sc_xlsx` makes a sheet per query: `{"name.csv": "query", ...}`, names relative paths (`/` between folders); the rows of all files together. Give the files as `json` or `jsonb` explicitly: an untyped string is taken as a query |
| `sc_read_csvzip(path text, entry text = '', from_row integer = 1, delimiter text = '', encoding text = '')` | `SETOF record` | that entry of a ZIP archive read as `sc_read_csv` reads a file. `entry` (since 1.12) may be a shell pattern, `*` and `?` (`'CodesAndDescriptionsFull*.csv'`, matched against the entry's file name when it has no `/`), or `''` for the archive's only `.csv` entry; an error naming the entries when none or more than one matches |

Since extension 1.7.

## Files and folders

`sc_mkdir`, `sc_move` and `sc_list_files` make, move and list files and folders as `mkdir -p`,
`mv` and `find -type f` do, but only in `/var/www/data/output` and below. That place is compiled
in: no setting changes it. Database functions can then do so without running a program (`copy ...
to program 'mkdir -p ...'`, which needs `pg_execute_server_program`).

```sql
PERFORM sc_mkdir('/var/www/data/output/omart/2026/Oct/');              -- '/var/www/data/output/omart/2026/Oct'
PERFORM sc_move('/var/www/data/output/dsn/a.txt', '/var/www/data/output/dsn/processed/');
SELECT path FROM sc_list_files('/var/www/data/output/queue/123', '1 minute', 1000);
```

| Function | Returns | Notes |
|---|---|---|
| `sc_mkdir(path text)` | `text` | the folder, without a trailing slash |
| `sc_move("from" text, "to" text)` | `text` | where it is now (since 1.12) |
| `sc_list_files(folder text, older_than interval = null, max integer = null)` | `TABLE(path text, modified timestamptz, size bigint)` | since 1.12 |

- Every path is absolute, `/var/www/data/output` or in it, with no empty, `.` or `..` parts
  (one trailing slash is fine). Anything else is an error that names the path.
- Nothing on the way may be a symbolic link: each part is opened without following one, so
  no link, planted before or while it runs, can lead out of the folder.
- `sc_mkdir`: a folder that's there already is fine; a file in the way isn't. Folders it makes
  are `0755` whatever the server's umask (PostgreSQL's 0077 made `mkdir -p` give `0700`), and
  belong to the server's operating system user. Folders that are there keep their mode.
- `sc_move` moves a file or a folder: renamed to `to`, or into `to` keeping its name when `to`
  ends with a slash or is a folder that's there, as `mv` does. A file at `to` is replaced; `to`'s
  folder must be there (`sc_mkdir` it first). Across filesystems a file is copied, then removed.
- `sc_list_files` gives the files in a folder and the folders in it (links left out, as `find
  -type f` does), oldest first: those modified longer than `older_than` ago (`'1 minute'` for
  `find -mmin +1`), at most `max`.
- `EXECUTE` on each is revoked from `PUBLIC`: `GRANT EXECUTE ON FUNCTION sc_mkdir(text) TO ...`
  (and `sc_move(text, text)`, `sc_list_files(text, interval, integer)`) the roles that need it.

### The server's logs

`sc_remove_log(name)` removes one of the server's own log files, by its name in
`log_directory` as `pg_ls_logdir()` gives it, once it has been read (`copy ... from
'log/<name>'`): `true`, or `false` when it isn't there. Only a plain file name, never the file the
server writes to now (`current_logfiles`). Since 1.12; `EXECUTE` is revoked from `PUBLIC`.

## Redis

`sc_pglib` talks to Redis (standalone or Cluster) through `sc::redis`, mainly
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
| `sc_redis_hmget(server, key, fields text[])` | `jsonb`: `{"field": "value", ...}` for the requested fields, `null` for missing ones |
| `sc_redis_hgetall(server, key)` | `jsonb`: every field of the hash, `{}` when there's no such key |
| `sc_redis_sadd(server, key, member)`, `sc_redis_sadd(server, key, members text[])` | `bigint`: members added (not counting ones already there) |
| `sc_redis_srem(server, key, member)`, `sc_redis_srem(server, key, members text[])` | `bigint`: members removed |
| `sc_redis_scard(server, key)` | `bigint`: members in the set, `0` when there's no such key |
| `sc_redis_smembers(server, key)` | `text[]`: the members, sorted; `{}` when there's no such key |
| `sc_redis_sismember(server, key, member)` | `boolean`: whether it's a member |

```sql
SELECT sc_redis_hmget('cache', 'vehicle:1', array['name', 'colour']);  -- {"name": "first", "colour": null}
SELECT sc_redis_hgetall('cache', 'vehicle:1') ->> 'name';                 -- first
SELECT sc_redis_sadd('cache', 'online', array['vehicle:1', 'vehicle:2']);    -- 2
SELECT 'vehicle:1' = ANY(sc_redis_smembers('cache', 'online'));              -- true
```

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
  (base64), `src/sc_redis.cpp` (Redis), `src/sc_xlsx.cpp` (Excel workbooks and CSV files), `src/xlsx_format.cpp` (a sheet's formatting) and `src/sc_files.cpp` with `src/folders.cpp` (files, folders and the server's logs),
  linking `sc::sc-redis` and `sc::sc-core` statically, and hiredis and
  sc-office's shared library dynamically. `src/pg_glue.h` has the rules
  for mixing C++ and PostgreSQL: C++ exceptions never reach PostgreSQL, and
  nothing that can raise a PostgreSQL error runs while a C++ object is alive,
  because `ereport()` `longjmp()`s past destructors.
- `sql/sc_pglib.control` and the `sql/sc_pglib--*.sql` scripts are what `CREATE
  EXTENSION` reads: `sc_pglib--1.0.sql` (base64) and the updates `1.0--1.1`
  (Redis), `1.1--1.2` (`hmget`, `hgetall`), `1.2--1.3` (sets), `1.3--1.4` (`sc_xlsx`), `1.4--1.5` (`sc_read_xlsx`), `1.5--1.6` (sheets as rows, `sc_xlsx_sheets`) `1.6--1.7` (`sc_csv`, `sc_read_csv`), `1.7--1.8` (`sc_mkdir`), `1.8--1.9` (`sc_csvzip`, `sc_read_csvzip`), `1.9--1.10` (`sc_csvzip` with several files), `1.10--1.11` (`sc_xlsx` with formatting) and `1.11--1.12` (CSV gzipped, in an encoding and added to an archive; `sc_move`, `sc_list_files`, `sc_remove_log`). They and the module install into the server's own directories
  (`pg_config --sharedir`/extension and `--pkglibdir`).
- The extension version (`1.12`) is separate from the package version. A package
  update replaces the module in place; adding or changing SQL objects needs a new
  extension version with an update script (`sc_pglib--1.12--1.13.sql`), after which
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
