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
    for step in sc_pglib--1.0.sql sc_pglib--1.0--1.1.sql sc_pglib--1.1--1.2.sql; do
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
    check "sc_pglib|1.2" "select extname, extversion from pg_extension where extname = 'sc_pglib'"
    check "" "set client_min_messages = warning; drop extension sc_pglib cascade"
    check "f" "select exists (select 1 from pg_proc where proname like 'sc_base64%' or proname like 'sc_redis%')"
    # Updating from 1.0, as an existing database does.
    check "" "create extension sc_pglib version '1.0'"
    check "f" "select exists (select 1 from pg_proc where proname = 'sc_redis_get')"
    check "" "alter extension sc_pglib update"
    check "sc_pglib|1.2" "select extname, extversion from pg_extension where extname = 'sc_pglib'"
    check "t" "select exists (select 1 from pg_foreign_data_wrapper where fdwname = 'sc_redis')"
    check "t" "select exists (select 1 from pg_proc where proname = 'sc_redis_hgetall')"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed; server log:"
    tail -20 "$work/server.log"
    exit 1
fi
echo "PASSED: all checks"
