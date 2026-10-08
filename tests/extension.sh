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
    cp "$(dirname "$script")/sc_pglib.control" "$script" "$work/share/extension/"
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
    echo "PostgreSQL $major: running the extension script directly (CREATE EXTENSION of an uninstalled extension needs 18)"
    sed -e '/^\\echo/d' -e "s|MODULE_PATHNAME|$library|g" "$script" | psql_run
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
if [ "$check_extension" = 1 ]; then
    check "sc_pglib|1.0" "select extname, extversion from pg_extension where extname = 'sc_pglib'"
    check "" "drop extension sc_pglib"
    check "f" "select exists (select 1 from pg_proc where proname like 'sc_base64%')"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed; server log:"
    tail -20 "$work/server.log"
    exit 1
fi
echo "PASSED: all checks"
