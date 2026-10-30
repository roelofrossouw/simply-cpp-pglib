#!/bin/bash
# Loads the built extension into a throwaway PostgreSQL cluster and checks its functions,
# without installing anything. PostgreSQL 18 and later can find an extension outside their own
# directories (extension_control_path), so there it is staged in the work directory and loaded
# with a real CREATE EXTENSION. Older servers run the extension script directly instead, with
# MODULE_PATHNAME replaced by the library's path.
#
# Usage: extension.sh <sc_pglib library> <sc_pglib--1.0.sql> <PostgreSQL bindir>
# Exits 77 (skipped) where a cluster can't be started.
set -euo pipefail

library=$1
script=$2
bindir=$3
sql_dir=$(dirname "$script")

skip() {
    echo "SKIPPED: $*"
    exit 77
}
[ "$(id -u)" != 0 ] || skip "initdb refuses to run as root"
for tool in initdb pg_ctl psql; do
    [ -x "$bindir/$tool" ] || skip "no $bindir/$tool (install the PostgreSQL server to run this test)"
done

# A fixed locale: on macOS the server won't start without one in its environment ("postmaster
# became multithreaded during startup"), and it keeps results the same everywhere.
export LC_ALL=C

work=$(mktemp -d "${TMPDIR:-/tmp}/sc-pglib-test.XXXXXX")
cleanup() {
    "$bindir/pg_ctl" -D "$work/data" -m immediate stop >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT

major=$("$bindir/postgres" --version | grep -oE '[0-9]+' | head -1)
# Only a Unix socket in the work directory, no TCP, so nothing clashes with a real server.
server_options="-k $work -c listen_addresses=''"
if [ "$major" -ge 18 ]; then
    mkdir -p "$work/share/extension" "$work/lib"
    cp "$sql_dir"/sc_pglib.control "$sql_dir"/sc_pglib--*.sql "$work/share/extension/"
    cp "$library" "$work/lib/"
    server_options+=" -c extension_control_path='$work/share:\$system' -c dynamic_library_path='$work/lib:\$libdir'"
fi

"$bindir/initdb" -D "$work/data" -A trust -U postgres -E UTF8 --locale=C --no-sync >"$work/initdb.log"
if ! "$bindir/pg_ctl" -D "$work/data" -l "$work/server.log" -w -o "$server_options" start >/dev/null; then
    echo "PostgreSQL did not start; server log:"
    cat "$work/server.log"
    exit 1
fi

psql_run() {
    "$bindir/psql" -h "$work" -U postgres -d postgres -X -q -A -t -v ON_ERROR_STOP=1 "$@"
}
if [ "$major" -ge 18 ]; then
    echo "PostgreSQL $major: CREATE EXTENSION sc_pglib"
    psql_run -c "create extension sc_pglib"
    check_extension=1
else
    echo "PostgreSQL $major: running the extension scripts directly (CREATE EXTENSION of an uninstalled extension needs 18)"
    for step in sc_pglib--1.0.sql sc_pglib--1.0--1.1.sql sc_pglib--1.1--1.2.sql sc_pglib--1.2--1.3.sql sc_pglib--1.3--1.4.sql sc_pglib--1.4--1.5.sql sc_pglib--1.5--1.6.sql; do
        sed -e '/^\\echo/d' -e "s|MODULE_PATHNAME|$library|g" "$sql_dir/$step" | psql_run
    done
    check_extension=0
fi

failures=0
# check <expected output> <sql>; expected may start with "~" to mean "contains".
check() {
    local expected=$1 sql=$2 actual
    actual=$(psql_run -c "$sql" 2>&1) || true
    if [[ $expected == "~"* && $actual == *"${expected:1}"* ]] || [ "$actual" == "$expected" ]; then
        echo "ok: $sql"
    else
        echo "FAILED: $sql"
        echo "  expected: $expected"
        echo "  actual:   $actual"
        failures=$((failures + 1))
    fi
}

check "SGVsbG8gV29ybGQh" "select sc_base64_encode('Hello World!')"
check "SGVsbG8gV29ybGQh" "select sc_base64_encode('Hello World!'::bytea)"
check "Hello World!" "select convert_from(sc_base64_decode('SGVsbG8gV29ybGQh'), 'UTF8')"
check '\x00ff10e9' "select sc_base64_decode(sc_base64_encode('\x00ff10e9'::bytea))"
check "t" "select sc_base64_encode(convert_to('simply-cpp', 'UTF8')) = encode(convert_to('simply-cpp', 'UTF8'), 'base64')"
check "" "select sc_base64_encode('')"
check "t" "select sc_base64_encode(null::text) is null and sc_base64_decode(null) is null"
check "1000000" "select length(sc_base64_decode(sc_base64_encode(repeat('x', 1000000))))"
check "~ERROR:  Invalid base64 (size)" "select sc_base64_decode('SGVsbG8')"
check "~ERROR:  Invalid base64 character" "select sc_base64_decode('!!!!')"
check "alive" "select 'alive'"

# --- Redis ---
# session <sql on stdin>: runs it in one session (so it shares the cached Redis clients), with
# psql variables from the remaining arguments, and prints everything it printed.
session() {
    psql_run "$@" 2>&1 || true
}
# expect <description> <expected> <actual>; expected may start with "~" to mean "contains".
expect() {
    local description=$1 expected=$2 actual=$3
    if [[ $expected == "~"* && $actual == *"${expected:1}"* ]] || [ "$actual" == "$expected" ]; then
        echo "ok: $description"
    else
        echo "FAILED: $description"
        echo "  expected: $expected"
        echo "  actual:   $actual"
        failures=$((failures + 1))
    fi
}

check "~needs the servers option" "create server bad foreign data wrapper sc_redis"
check "~invalid option \"server\"" "create server bad foreign data wrapper sc_redis options (server 'x')"
check "~invalid servers option" "create server bad foreign data wrapper sc_redis options (servers 'redis1:nope')"
check "~on_error must be" "create server bad foreign data wrapper sc_redis options (servers 'x', on_error 'maybe')"
check "~timeout_ms must be" "create server bad foreign data wrapper sc_redis options (servers 'x', timeout_ms '0')"
check "~only valid option is password" "create server ok foreign data wrapper sc_redis options (servers 'x'); create user mapping for public server ok options (username 'u')"
check "~server \"missing\" does not exist" "select sc_redis_get('missing', 'key')"
check "~is not a sc_redis server" "create foreign data wrapper other; create server elsewhere foreign data wrapper other; select sc_redis_get('elsewhere', 'key')"

# 10.255.255.1 isn't routed: connects time out. In warning mode the first call warns and returns
# false; later calls in the session skip it without waiting, until retry_interval_ms passes.
psql_run -c "create server nowhere foreign data wrapper sc_redis options (servers '10.255.255.1:6379', connect_timeout_ms '200', retry_interval_ms '60000')"
output=$(session <<'SQL'
select sc_redis_set('nowhere', 'a', '1');
select clock_timestamp() as skipped_from \gset
select sc_redis_set('nowhere', 'b', '2');
select sc_redis_get('nowhere', 'a') is null;
select clock_timestamp() - :'skipped_from'::timestamptz < interval '50 ms';
SQL
)
expect "unreachable server warns once" "1" "$(grep -c 'is unavailable, skipping it for 60000 ms' <<<"$output")"
expect "unreachable server: false, false, NULL, then skipped quickly" "f f t t" "$(grep -vE 'WARNING' <<<"$output" | tr '\n' ' ' | sed 's/ $//')"
check "~ERROR:  Redis server \"strict\" is unavailable" "create server strict foreign data wrapper sc_redis options (servers '10.255.255.1:6379', connect_timeout_ms '200', on_error 'error'); select sc_redis_get('strict', 'a')"
check "~fields must not contain NULL" "select sc_redis_hmget('nowhere', 'key', array['a', null])"
check "~permission denied for foreign server nowhere" "create role sc_pglib_tester; set role sc_pglib_tester; select sc_redis_get('nowhere', 'a')"

# Excel workbooks: written by the server into the work directory.
sheet_xml() { unzip -p "$1" "xl/worksheets/sheet$2.xml"; }
check "3" "select sc_xlsx('$work/one.xlsx', 'select n, n * 1.5 as half, n % 2 = 0 as even, date ''2026-10-30'' + n as day, ''row '' || n as label, null::text as nothing from generate_series(1, 3) n')"
check "644" "select '$(stat -f %Lp "$work/one.xlsx" 2>/dev/null || stat -c %a "$work/one.xlsx" 2>/dev/null)'"
if sheet_xml "$work/one.xlsx" 1 | grep -q '>label</t></is>' && sheet_xml "$work/one.xlsx" 1 | grep -q '<c r="D2" s="2"><v>46326</v>' &&
   sheet_xml "$work/one.xlsx" 1 | grep -q '>row 3</t></is>' && sheet_xml "$work/one.xlsx" 1 | grep -q 'ySplit="1"'; then
    echo "ok: one.xlsx holds the header, a date as 46326 (2026-10-31), the text and the frozen header"
else
    echo "FAILED: one.xlsx's sheet"; sheet_xml "$work/one.xlsx" 1 | head -c 2000; echo; failures=$((failures + 1))
fi
check "2" "select sc_xlsx('$work/named.xlsx', 'select 1 union all select 2', 'Numbers')"
unzip -p "$work/named.xlsx" xl/workbook.xml | grep -q 'name="Numbers"' && echo "ok: the sheet is named Numbers" || { echo "FAILED: named.xlsx's sheet name"; failures=$((failures + 1)); }
check "5" "select sc_xlsx('$work/two.xlsx', '{\"Zeta\": \"select 1\", \"Alpha\": \"select g from generate_series(1, 4) g\"}'::json)"
unzip -p "$work/two.xlsx" xl/workbook.xml | grep -o 'sheet name="[A-Za-z]*"' | tr '\n' ' ' | grep -q 'Zeta.*Alpha' && echo "ok: json keeps the sheets' order" || { echo "FAILED: two.xlsx's order"; failures=$((failures + 1)); }
check "5" "select sc_xlsx('$work/three.xlsx', '{\"Zeta\": \"select 1\", \"Alpha\": \"select g from generate_series(1, 4) g\"}'::jsonb)"
check "0" "select sc_xlsx('$work/empty.xlsx', 'select 1 as one where false')"
check "~the path must be absolute" "select sc_xlsx('relative.xlsx', 'select 1')"
check "~isn't a SELECT" "select sc_xlsx('$work/x.xlsx', 'create temp table sc_pglib_t (a int)')"
check "~sheet" "select sc_xlsx('$work/x.xlsx', 'select 1', 'bad/name')"
check "~its query isn't text" "select sc_xlsx('$work/x.xlsx', '{\"A\": 1}'::jsonb)"
check "~relation \"no_such_table\" does not exist" "select sc_xlsx('$work/x.xlsx', 'select * from no_such_table')"
# Read back: each sheet its rows, a date as ISO text, an empty cell null, the sheets in order.
check '{"Sheet1":[["n","half","even","day","label","nothing"],[1,1.5,false,"2026-10-31","row 1"],[2,3,true,"2026-11-01","row 2"],[3,4.5,false,"2026-11-02","row 3"]]}' "select sc_read_xlsx('$work/one.xlsx')"
check 'Zeta,Alpha' "select string_agg(k, ',') from json_object_keys(sc_read_xlsx('$work/two.xlsx')) k"
# One sheet's rows as rows: by name (any case) or number, columns by their letters, "row" the row number.
check '4' "select count(*) - 1 from sc_read_xlsx('$work/two.xlsx', 'alpha') as x(a text)"
check 'g,1,2,3,4' "select string_agg(a, ',') from sc_read_xlsx('$work/two.xlsx', 2) as x(a text)"
check '1:Zeta,2:Alpha' "select string_agg(sheet || ':' || name, ',' order by sheet) from sc_xlsx_sheets('$work/two.xlsx')"
check 'row 1,row 2,row 3' "select string_agg(e, ',') from sc_read_xlsx('$work/one.xlsx', 1) as x(\"row\" int, e text) where \"row\" > 1"
check '6|9.0|1|2026-11-02' "select sum(a), sum(b), count(*) filter (where c), max(d) from sc_read_xlsx('$work/one.xlsx', 'Sheet1', 2) as x(a int, b numeric, c boolean, d date)"
check '2|row 1|1' "select \"row\", e, a from sc_read_xlsx('$work/one.xlsx', 1, 2) as x(e text, \"row\" int, a int) where \"row\" = 2"
check '3' "select count(*) from sc_read_xlsx('$work/one.xlsx', 1, 2) as x(a int)"
check "~rows are numbered from 1" "select * from sc_read_xlsx('$work/one.xlsx', 1, 0) as x(a text)"
check '||' "select f, g, aa from sc_read_xlsx('$work/one.xlsx', 1) as x(\"row\" int, f text, g text, aa text) where \"row\" = 2"
check "~cell E2" "select * from sc_read_xlsx('$work/one.xlsx', 1, 2) as x(e int)"
check "~numbered from 1" "select * from sc_read_xlsx('$work/one.xlsx', 0) as x(a text)"
check "~there's no sheet 3 (the workbook has 2)" "select * from sc_read_xlsx('$work/two.xlsx', 3) as x(a text)"
check "~isn't a sheet column" "select * from sc_read_xlsx('$work/one.xlsx', 1) as x(price numeric)"
check "~column definition list is required" "select * from sc_read_xlsx('$work/one.xlsx', 1)"
check "1" "select sc_xlsx('$work/times.xlsx', 'select timestamp ''2026-10-30 14:05:06'' as at, time ''14:05:06'' as t, true as yes, null::int as gap, 7 as seven')"
check '{"Sheet1":[["at","t","yes","gap","seven"],["2026-10-30 14:05:06","14:05:06",true,null,7]]}' "select sc_read_xlsx('$work/times.xlsx')"
check '2026-10-30 14:05:06|14:05:06|t||7' "select * from sc_read_xlsx('$work/times.xlsx', 'Sheet1', 2) as x(a timestamp, b time, c boolean, d int, e int)"
check 'one' "select * from sc_read_xlsx('$work/empty.xlsx', 1) as x(a text)"
check "~there's no sheet \"Nope\"" "select * from sc_read_xlsx('$work/one.xlsx', 'Nope') as x(a text)"
check "~the path must be absolute" "select sc_read_xlsx('one.xlsx')"
check "~sc_read_xlsx: \"$work/missing.xlsx\"" "select sc_read_xlsx('$work/missing.xlsx')"
check "" "create role sc_pglib_writer"
check "~permission denied to read a file" "set role sc_pglib_writer; select sc_read_xlsx('$work/one.xlsx')"
check "~permission denied to read a file" "set role sc_pglib_writer; select * from sc_read_xlsx('$work/one.xlsx', 1) as x(a text)"
check "~permission denied to read a file" "set role sc_pglib_writer; select * from sc_xlsx_sheets('$work/one.xlsx')"
check "~permission denied to write a file" "set role sc_pglib_writer; select sc_xlsx('$work/x.xlsx', 'select 1')"
check "f" "select '$( [ -e "$work/x.xlsx" ] && echo t || echo f)'"

if [ -n "${SC_REDIS_DEMO_SERVER:-}" ]; then
    prefix="sc-tmp:sc-pglib-test:$$"
    psql_run -v servers="$SC_REDIS_DEMO_SERVER" <<'SQL'
create server cache foreign data wrapper sc_redis options (servers :'servers');
SQL
    output=$(session -v prefix="$prefix" <<'SQL'
select sc_redis_set('cache', :'prefix' || ':string', 'Hello World!');
select sc_redis_get('cache', :'prefix' || ':string');
select sc_redis_get('cache', :'prefix' || ':missing') is null;
select sc_redis_hset('cache', :'prefix' || ':hash', 'name', 'simply-cpp');
select sc_redis_hget('cache', :'prefix' || ':hash', 'name');
select sc_redis_hget('cache', :'prefix' || ':hash', 'missing') is null;
select sc_redis_del('cache', :'prefix' || ':string');
select sc_redis_get('cache', :'prefix' || ':string') is null;
SQL
    )
    expect "Redis set/get/hset/hget/del" "t Hello World! t t simply-cpp t 1 t" "$(tr '\n' ' ' <<<"$output" | sed 's/ $//')"
    output=$(session -v prefix="$prefix" <<'SQL'
select sc_redis_hset('cache', :'prefix' || ':hash', 'name', 'simply-cpp');
select sc_redis_hset('cache', :'prefix' || ':hash', 'kind', 'demo');
select sc_redis_hset('cache', :'prefix' || ':hash', 'odd', E'say "hi"\nnext');
select sc_redis_hmget('cache', :'prefix' || ':hash', array['name', 'missing']);
select sc_redis_hgetall('cache', :'prefix' || ':hash');
select sc_redis_hgetall('cache', :'prefix' || ':hash') ->> 'odd' = E'say "hi"\nnext';
select sc_redis_hgetall('cache', :'prefix' || ':no-such-hash');
select sc_redis_hmget('cache', :'prefix' || ':hash', array[]::text[]);
SQL
    )
    expect "Redis hmget/hgetall as jsonb" \
        't t t {"name": "simply-cpp", "missing": null} {"odd": "say \"hi\"\nnext", "kind": "demo", "name": "simply-cpp"} t {} {}' \
        "$(tr '\n' ' ' <<<"$output" | sed 's/ $//')"
    output=$(session -v prefix="$prefix" <<'SQL'
select sc_redis_sadd('cache', :'prefix' || ':set', 'red');
select sc_redis_sadd('cache', :'prefix' || ':set', array['green', 'blue', 'red']);
select sc_redis_sadd('cache', :'prefix' || ':set', array[]::text[]);
select sc_redis_scard('cache', :'prefix' || ':set');
select sc_redis_smembers('cache', :'prefix' || ':set');
select sc_redis_sismember('cache', :'prefix' || ':set', 'green');
select sc_redis_sismember('cache', :'prefix' || ':set', 'purple');
select 'red' = any(sc_redis_smembers('cache', :'prefix' || ':set'));
select sc_redis_srem('cache', :'prefix' || ':set', array['green', 'purple']);
select sc_redis_srem('cache', :'prefix' || ':set', 'blue');
select sc_redis_smembers('cache', :'prefix' || ':set');
select sc_redis_scard('cache', :'prefix' || ':no-such-set');
select sc_redis_smembers('cache', :'prefix' || ':no-such-set');
select sc_redis_sismember('cache', :'prefix' || ':no-such-set', 'red');
select sc_redis_del('cache', :'prefix' || ':set');
SQL
    )
    expect "Redis sets: sadd/srem (one or several), scard, smembers, sismember" \
        '1 2 0 3 {blue,green,red} t f t 1 1 {red} 0 {} f 1' \
        "$(tr '\n' ' ' <<<"$output" | sed 's/ $//')"
    check "~members must not contain NULL" "select sc_redis_sadd('cache', 'key', array['a', null])"
    expect "a Redis error reply (WRONGTYPE) is a warning" "~WRONGTYPE" \
        "$(session -v prefix="$prefix" <<<"select sc_redis_get('cache', :'prefix' || ':hash');")"

    # The intended use: a trigger keeps Redis up to date. Not transactional: a rolled back
    # insert still reached Redis.
    output=$(session -v prefix="$prefix" <<'SQL'
create table vehicle (id int primary key, name text);
create function vehicle_to_redis() returns trigger language plpgsql as $$
begin
    perform sc_redis_hset('cache', current_setting('test.prefix') || ':vehicle:' || new.id, 'name', new.name);
    return new;
end $$;
create trigger vehicle_to_redis after insert or update on vehicle for each row execute function vehicle_to_redis();
set test.prefix = :'prefix';
insert into vehicle values (1, 'first');
update vehicle set name = 'renamed' where id = 1;
select sc_redis_hget('cache', :'prefix' || ':vehicle:1', 'name');
begin;
insert into vehicle values (2, 'rolled back');
rollback;
select sc_redis_hget('cache', :'prefix' || ':vehicle:2', 'name');
select sc_redis_del('cache', :'prefix' || ':vehicle:1') + sc_redis_del('cache', :'prefix' || ':vehicle:2')
     + sc_redis_del('cache', :'prefix' || ':hash');
SQL
    )
    expect "a trigger updates Redis, even for a rolled back insert" "renamed rolled back 3" \
        "$(tr '\n' ' ' <<<"$output" | sed 's/ $//')"
else
    echo "skipped: live Redis checks (set SC_REDIS_DEMO_SERVER)"
fi

if [ -n "${SC_REDIS_TEST_PASSWORD:-}" ]; then
    # The password-protected server sc-redis's own authenticated test uses.
    psql_run -v password="$SC_REDIS_TEST_PASSWORD" <<'SQL'
create server protected foreign data wrapper sc_redis options (servers 'vms:20006');
create user mapping for public server protected options (password :'password');
create server protected_wrong foreign data wrapper sc_redis options (servers 'vms:20006');
create user mapping for public server protected_wrong options (password 'wrong');
SQL
    prefix="sc-tmp:sc-pglib-test:$$"
    expect "a user mapping's password is used" "t value 1" "$(session -v prefix="$prefix" <<'SQL' | tr '\n' ' ' | sed 's/ $//'
select sc_redis_set('protected', :'prefix' || ':auth', 'value');
select sc_redis_get('protected', :'prefix' || ':auth');
select sc_redis_del('protected', :'prefix' || ':auth');
SQL
)"
    expect "a wrong password makes the server unavailable" "~is unavailable" "$(session <<<"select sc_redis_get('protected_wrong', 'key')")"
else
    echo "skipped: password checks (set SC_REDIS_TEST_PASSWORD)"
fi

if [ "$check_extension" = 1 ]; then
    check "sc_pglib|1.6" "select extname, extversion from pg_extension where extname = 'sc_pglib'"
    check "" "set client_min_messages = warning; drop extension sc_pglib cascade"
    check "f" "select exists (select 1 from pg_proc where proname like 'sc_base64%' or proname like 'sc_redis%' or proname in ('sc_xlsx', 'sc_read_xlsx', 'sc_xlsx_sheets'))"
    # Updating from 1.0, as an existing database does.
    check "" "create extension sc_pglib version '1.0'"
    check "f" "select exists (select 1 from pg_proc where proname = 'sc_redis_get')"
    check "" "alter extension sc_pglib update"
    check "sc_pglib|1.6" "select extname, extversion from pg_extension where extname = 'sc_pglib'"
    check "t" "select exists (select 1 from pg_foreign_data_wrapper where fdwname = 'sc_redis')"
    check "t" "select exists (select 1 from pg_proc where proname = 'sc_redis_hgetall')"
    check "2" "select count(*) from pg_proc where proname = 'sc_redis_sadd'"
    check "4" "select count(*) from pg_proc where proname = 'sc_xlsx'"
    check "3" "select count(*) from pg_proc where proname = 'sc_read_xlsx'"
    check "2" "select count(*) from pg_proc where proname = 'sc_read_xlsx' and pronargdefaults = 1"
    check "t" "select pg_get_function_result(oid) = 'SETOF record' from pg_proc where proname = 'sc_read_xlsx' and pronargs = 3 and proargtypes[1] = 'text'::regtype"
    check "1" "select count(*) from pg_proc where proname = 'sc_xlsx_sheets'"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed; server log:"
    tail -20 "$work/server.log"
    exit 1
fi
echo "PASSED: all checks"
