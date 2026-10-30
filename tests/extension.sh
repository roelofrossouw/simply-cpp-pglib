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
    for step in sc_pglib--1.0.sql sc_pglib--1.0--1.1.sql sc_pglib--1.1--1.2.sql sc_pglib--1.2--1.3.sql sc_pglib--1.3--1.4.sql sc_pglib--1.4--1.5.sql sc_pglib--1.5--1.6.sql sc_pglib--1.6--1.7.sql sc_pglib--1.7--1.8.sql sc_pglib--1.8--1.9.sql sc_pglib--1.9--1.10.sql sc_pglib--1.10--1.11.sql sc_pglib--1.11--1.12.sql sc_pglib--1.12--1.13.sql sc_pglib--1.13--1.14.sql; do
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
# A format: panes, an AutoFilter, a font for the sheet, the header's colours, number formats and
# widths by column name, a hidden column, bold bordered cells and a row's height.
check "3" "select sc_xlsx('$work/styled.xlsx', 'select n as id, n * 10.5 as price, ''item '' || n as name, date ''2026-10-30'' + n as day from generate_series(1, 3) n', 'Prices', '{\"freeze_panes\": \"B2\", \"autofilter\": true, \"date_format\": \"dd mmm yyyy\", \"style\": {\"font\": \"Arial\", \"font_size\": 10}, \"header_style\": {\"background\": \"#1f4e79\", \"color\": \"white\"}, \"columns\": {\"price\": \"currency\", \"name\": {\"width\": 30}}, \"format\": {\"A\": {\"hidden\": true}, \"c2:c3\": {\"bold\": true, \"border\": \"thin\"}, \"4\": {\"height\": 20}}}')"
styled=$(sheet_xml "$work/styled.xlsx" 1)
styles=$(unzip -p "$work/styled.xlsx" xl/styles.xml)
if grep -q 'xSplit="1" ySplit="1" topLeftCell="B2"' <<<"$styled" && grep -q '<autoFilter ref="A1:D4"/>' <<<"$styled" &&
   grep -q '<col min="1" max="1" width="8" customWidth="1" hidden="1"/>' <<<"$styled" && grep -q '<col min="3" max="3" width="30" customWidth="1"/>' <<<"$styled" &&
   grep -q '<row r="4" ht="20" customHeight="1">' <<<"$styled" && grep -q '<sz val="10"/><color rgb="FFFFFFFF"/><name val="Arial"/>' <<<"$styles" &&
   grep -q 'formatCode="&quot;R &quot;#,##0.00;-&quot;R &quot;#,##0.00"' <<<"$styles" && grep -q 'formatCode="dd mmm yyyy"' <<<"$styles" &&
   grep -q '<fgColor rgb="FF1F4E79"/>' <<<"$styles" && grep -q '<left style="thin">' <<<"$styles" &&
   unzip -p "$work/styled.xlsx" xl/workbook.xml | grep -q "_xlnm._FilterDatabase\" localSheetId=\"0\" hidden=\"1\">'Prices'!\$A\$1:\$D\$4<"; then
    echo "ok: styled.xlsx has its panes, AutoFilter, widths, hidden column, row height, font, colours, borders and formats"
else
    echo "FAILED: styled.xlsx"; echo "$styled" | head -c 3000; echo; echo "$styles" | head -c 3000; echo; failures=$((failures + 1))
fi
check '{"Prices":[["id","price","name","day"],[1,10.5,"item 1","2026-10-31"],[2,21,"item 2","2026-11-01"],[3,31.5,"item 3","2026-11-02"]]}' "select sc_read_xlsx('$work/styled.xlsx')"
# Several sheets: a format for all of them, a sheet's own keys over it (no header, nothing frozen).
check "2" "select sc_xlsx('$work/tabs.xlsx', '{\"One\": \"select 1 as a\", \"Two\": {\"query\": \"select 2 as b\", \"header\": false, \"freeze_panes\": false, \"format\": {\"A1\": \"0.00\"}}}'::json, '{\"autofilter\": true, \"style\": {\"font\": \"Courier New\"}, \"format\": {\"A\": {\"width\": 12}}}')"
if sheet_xml "$work/tabs.xlsx" 1 | grep -q 'ySplit="1"' && sheet_xml "$work/tabs.xlsx" 1 | grep -q '<autoFilter ref="A1:A2"/>' &&
   ! sheet_xml "$work/tabs.xlsx" 2 | grep -q 'sheetViews' && sheet_xml "$work/tabs.xlsx" 2 | grep -q '<autoFilter ref="A1"/>' &&
   sheet_xml "$work/tabs.xlsx" 2 | grep -q '<col min="1" max="1" width="12" customWidth="1"/>' &&
   unzip -p "$work/tabs.xlsx" xl/styles.xml | grep -q '<xf numFmtId="2" ' && unzip -p "$work/tabs.xlsx" xl/styles.xml | grep -q 'name val="Courier New"'; then
    echo "ok: tabs.xlsx's sheets share the format, the second with its own"
else
    echo "FAILED: tabs.xlsx"; sheet_xml "$work/tabs.xlsx" 2 | head -c 2000; echo; failures=$((failures + 1))
fi
check '{"One":[["a"],[1]],"Two":[[2]]}' "select sc_read_xlsx('$work/tabs.xlsx')"
check "1" "select sc_xlsx('$work/tabsb.xlsx', '{\"One\": {\"query\": \"select 1 as a\", \"columns\": {\"a\": \"percent\"}}}'::jsonb, '{}')"
check "~isn't a sheet's: header, freeze_panes" "select sc_xlsx('$work/x.xlsx', 'select 1', 'S', '{\"colour\": \"red\"}')"
check "~sheet \"S\": format.format.A1: \"colour\" isn't something to change" "select sc_xlsx('$work/x.xlsx', 'select 1', 'S', '{\"format\": {\"A1\": {\"colour\": \"red\"}}}')"
check "~only columns have a width" "select sc_xlsx('$work/x.xlsx', 'select 1', 'S', '{\"format\": {\"A1\": {\"width\": 3}}}')"
check "~\"A1:\" isn't a cell" "select sc_xlsx('$work/x.xlsx', 'select 1', 'S', '{\"format\": {\"A1:\": \"text\"}}')"
check "~isn't a colour" "select sc_xlsx('$work/x.xlsx', 'select 1', 'S', '{\"style\": {\"color\": \"#12\"}}')"
check "~isn't a line" "select sc_xlsx('$work/x.xlsx', 'select 1', 'S', '{\"style\": {\"border\": \"wide\"}}')"
check "~the query has no column \"nope\" (it has one)" "select sc_xlsx('$work/x.xlsx', 'select 1 as one', 'S', '{\"columns\": {\"nope\": \"currency\"}}')"
check "~its object has no \"query\" text" "select sc_xlsx('$work/x.xlsx', '{\"A\": {\"freeze_panes\": \"B2\"}}'::json, '{}')"
check "~sheet \"B\": format.header: true or false" "select sc_xlsx('$work/x.xlsx', '{\"A\": \"select 1\", \"B\": {\"query\": \"select 1\", \"header\": 1}}'::json, '{}')"
# CSV: a query's rows with its columns' names first, read back as rows (no sheet), a delimiter
# given or found.
check "3" "select sc_csv('$work/one.csv', 'select n, n * 1.5 as half, n % 2 = 0 as even, date ''2026-10-30'' + n as day, ''row, '' || n as label, null::text as nothing, ''007'' as code from generate_series(1, 3) n')"
check "644" "select '$(stat -f %Lp "$work/one.csv" 2>/dev/null || stat -c %a "$work/one.csv" 2>/dev/null)'"
if head -2 "$work/one.csv" | tr -d '\r' | tr '\n' '|' | grep -q '^n,half,even,day,label,nothing,code|1,1.5,FALSE,2026-10-31,"row, 1",,007|$'; then
    echo "ok: one.csv holds the header, numbers, TRUE/FALSE, an ISO date, a quoted comma, an empty field and 007"
else
    echo "FAILED: one.csv"; head -3 "$work/one.csv"; failures=$((failures + 1))
fi
check 'row, 1|row, 2|row, 3' "select string_agg(e, '|') from sc_read_csv('$work/one.csv', 2) as x(e text)"
check '6|9.0|1|2026-11-02|007' "select sum(a), sum(b), count(*) filter (where c), max(d), min(g) from sc_read_csv('$work/one.csv', 2) as x(a int, b numeric, c boolean, d date, g text)"
check '2|1' "select \"row\", a from sc_read_csv('$work/one.csv', 2) as x(\"row\" int, a int) where \"row\" = 2"
check '4' "select count(*) from sc_read_csv('$work/one.csv') as x(a text)"
check "2" "select sc_csv('$work/semi.csv', 'select 1.5 as price union all select 2.25', ';')"
check '3.75' "select sum(a) from sc_read_csv('$work/semi.csv', 2) as x(a numeric)"
printf 'name;price\r\nBread;12,50\r\nMilk;20,00\r\n' > "$work/european.csv"
check 'Bread=12,50|Milk=20,00' "select string_agg(a || '=' || b, '|') from sc_read_csv('$work/european.csv', 2) as x(a text, b text)"
check 'Bread;12' "select a from sc_read_csv('$work/european.csv', 2, ',') as x(a text) limit 1"
check "~cell D2" "select * from sc_read_csv('$work/one.csv', 2) as x(d int)"
check "~rows are numbered from 1" "select * from sc_read_csv('$work/one.csv', 0) as x(a text)"
check "~the delimiter is one character" "select * from sc_read_csv('$work/one.csv', 1, ';;') as x(a text)"
check "~the delimiter is one character" "select sc_csv('$work/x.csv', 'select 1', '')"
check "~the path must be absolute" "select sc_csv('relative.csv', 'select 1')"
check "~sc_csv: its query returns no rows" "select sc_csv('$work/x.csv', 'create temp table sc_pglib_c (a int)')"
check "~sc_read_csv: \"$work/missing.csv\"" "select * from sc_read_csv('$work/missing.csv') as x(a text)"
# CSV in a ZIP archive: one entry (the archive's name with .csv, or as given), read back the same
check "3" "select sc_csvzip('$work/one.zip', 'select n, n * 1000000 as big, ''row, '' || n as label from generate_series(1, 3) n')"
unzip -l "$work/one.zip" | grep -q ' one.csv$' && echo "ok: one.zip holds one.csv" || { echo "FAILED: one.zip's entry"; failures=$((failures + 1)); }
check '6|6000000|row, 1' "select sum(a), sum(b), min(c) from sc_read_csvzip('$work/one.zip', 'one.csv', 2) as x(a int, b bigint, c text)"
check "2" "select sc_csvzip('$work/two.zip', 'select 1.5 as price union all select 2.25', 'prices.csv', ';')"
check '3.75' "select sum(a) from sc_read_csvzip('$work/two.zip', 'prices.csv', 2) as x(a numeric)"
check '3.75' "select sum(a) from sc_read_csvzip('$work/two.zip', 'prices.csv', 2, ';') as x(a numeric)"
check "~there's no \"nope.csv\" in it (it has prices.csv)" "select * from sc_read_csvzip('$work/two.zip', 'nope.csv') as x(a text)"
check "~sc_read_csvzip: \"$work/one.csv\"" "select * from sc_read_csvzip('$work/one.csv', 'one.csv') as x(a text)"
check "~the path must be absolute" "select sc_csvzip('relative.zip', 'select 1')"
check "~rows are numbered from 1" "select * from sc_read_csvzip('$work/one.zip', 'one.csv', 0) as x(a text)"
# Several files: a CSV per query, json keeping their order, jsonb sorting; an untyped string is a query
check "7" "select sc_csvzip('$work/many.zip', '{\"zeta.csv\": \"select 1 as one\", \"data/alpha.csv\": \"select g from generate_series(1, 4) g\", \"b.csv\": \"select 1 union all select 2\"}'::json)"
unzip -Z1 "$work/many.zip" | tr '\n' ' ' | grep -q '^zeta.csv data/alpha.csv b.csv $' && echo "ok: json keeps the files' order" || { echo "FAILED: many.zip's entries: $(unzip -Z1 "$work/many.zip" | tr '\n' ' ')"; failures=$((failures + 1)); }
check '10' "select sum(a) from sc_read_csvzip('$work/many.zip', 'data/alpha.csv', 2) as x(a int)"
check "2" "select sc_csvzip('$work/sorted.zip', '{\"zeta.csv\": \"select 1\", \"b.csv\": \"select 2\"}'::jsonb, ';')"
unzip -Z1 "$work/sorted.zip" | tr '\n' ' ' | grep -q '^b.csv zeta.csv $' && echo "ok: jsonb sorts the files" || { echo "FAILED: sorted.zip's entries"; failures=$((failures + 1)); }
check "1" "select sc_csvzip('$work/text.zip', 'select 1')"
check "~a relative path" "select sc_csvzip('$work/x.zip', '{\"/etc/x.csv\": \"select 1\"}'::json)"
check "~has an empty" "select sc_csvzip('$work/x.zip', '{\"a/../x.csv\": \"select 1\"}'::json)"
check "~its query isn't text" "select sc_csvzip('$work/x.zip', '{\"a.csv\": 1}'::jsonb)"
check "~a JSON object of names" "select sc_csvzip('$work/x.zip', '[]'::json)"
check "~sc_csvzip: file \"bad.csv\": its query returns no rows" "select sc_csvzip('$work/x.zip', '{\"bad.csv\": \"create temp table sc_pglib_z (a int)\"}'::json)"
check "~relation \"no_such_table\" does not exist" "select sc_csvzip('$work/x.zip', '{\"a.csv\": \"select * from no_such_table\"}'::json)"
# CSV gzipped when the path ends in .gz, and read back uncompressed whatever its name
check "2" "select sc_csv('$work/dump.csv.gz', 'select 1 as a, ''x'' as b union all select 2, ''y''')"
if gzip -t "$work/dump.csv.gz" 2>/dev/null && [ "$(gzip -dc "$work/dump.csv.gz" | head -1 | tr -d '\r')" = "a,b" ]; then echo "ok: dump.csv.gz is gzip"; else echo "FAILED: dump.csv.gz isn't gzip"; failures=$((failures + 1)); fi
check "2|y" "select a || '|' || b from sc_read_csv('$work/dump.csv.gz', 2) as x(a text, b text) where a = '2'"
# An encoding converted to the database's: Привет in windows-1251
printf 'word\n\317\360\350\342\345\362\n' > "$work/cyrillic.csv"
check "Привет" "select a from sc_read_csv('$work/cyrillic.csv', 2, ',', 'windows-1251') as x(a text)"
check "Привет" "select a from sc_read_csv('$work/cyrillic.csv', 2, ',', 'WIN1251') as x(a text)"
check "~invalid byte sequence for encoding \"UTF8\"" "select a from sc_read_csv('$work/cyrillic.csv', 2) as x(a text)"
check "~isn't an encoding PostgreSQL knows" "select a from sc_read_csv('$work/cyrillic.csv', 2, ',', 'klingon') as x(a text)"
# Added to an archive: the others kept, one of the same name replaced
check "1" "select sc_csvzip('$work/added.zip', 'select 1 as one', 'a.csv')"
check "1" "select sc_csvzip('$work/added.zip', 'select 2 as two', 'b.csv', ',', true)"
check "1" "select sc_csvzip('$work/added.zip', 'select 3 as three', 'a.csv', ',', true)"
check "3|2" "select (select a from sc_read_csvzip('$work/added.zip', 'a.csv', 2) as x(a text)) || '|' || (select a from sc_read_csvzip('$work/added.zip', 'b.csv', 2) as x(a text))"
check "1" "select sc_csvzip('$work/added.zip', 'select 4 as four', 'c.csv')"
check "~there's no \"a.csv\" in it (it has c.csv)" "select * from sc_read_csvzip('$work/added.zip', 'a.csv') as x(a text)"
# sc_move and sc_list_files: only under /var/www/data/output (their moving and listing is tested by test-folders)
check "~isn't /var/www/data/output or a folder in it" "select sc_move('$work/one.xlsx', '/var/www/data/output/x')"
check "~isn't /var/www/data/output or a folder in it" "select sc_move('/var/www/data/output/x', '$work/x')"
check "~\"..\" part" "select * from sc_list_files('/var/www/data/output/../x')"
# sc_remove_log: a finished log in log_directory; never a path
mkdir -p "$work/data/log" && echo old > "$work/data/log/old.csv"
check "t" "select sc_remove_log('old.csv')"
check "f" "select sc_remove_log('old.csv')"
[ ! -e "$work/data/log/old.csv" ] && echo "ok: old.csv is gone" || { echo "FAILED: old.csv is still there"; failures=$((failures + 1)); }
check "~isn't a file name in the log folder" "select sc_remove_log('../postgresql.conf')"
check "~isn't a file name in the log folder" "select sc_remove_log('..')"
# An entry by pattern, or '' for the only .csv one
check "3" "select sc_csvzip('$work/efile.zip', '{\"data/CodesAndDescriptionsFull20261030.csv\": \"select ''codes'' as a\", \"OptionName20261030.csv\": \"select ''options'' as a\", \"readme.txt\": \"select ''text'' as a\"}'::json)"
check "codes" "select a from sc_read_csvzip('$work/efile.zip', 'CodesAndDescriptionsFull*.csv', 2) as x(a text)"
check "options" "select a from sc_read_csvzip('$work/efile.zip', 'Option?ame*', 2) as x(a text)"
check "~more than one entry is \"*.csv\" (it has data/CodesAndDescriptionsFull20261030.csv, OptionName20261030.csv, readme.txt)" "select * from sc_read_csvzip('$work/efile.zip', '*.csv') as x(a text)"
check "~there's no \"Nope*\" in it" "select * from sc_read_csvzip('$work/efile.zip', 'Nope*') as x(a text)"
check "~more than one entry is a .csv entry" "select * from sc_read_csvzip('$work/efile.zip', '') as x(a text)"
check "2" "select sc_csvzip('$work/fuel.zip', '{\"cards.CSV\": \"select 1 as one\", \"notes.txt\": \"select 2 as two\"}'::json)"
check "1" "select a from sc_read_csvzip('$work/fuel.zip', from_row => 2) as x(a text)"
check "1" "select a from sc_read_csvzip('$work/fuel.zip', '', 2) as x(a text)"
check "" "create role sc_pglib_writer"
# sc_mkdir: only under /var/www/data/output (its making is tested by test-folders), revoked from PUBLIC
check "~isn't /var/www/data/output or a folder in it" "select sc_mkdir('$work/x')"
check "~\"..\" part" "select sc_mkdir('/var/www/data/output/a/../../x')"
check "~isn't an absolute path" "select sc_mkdir('output/x')"
check "f" "select has_function_privilege('sc_pglib_writer', 'sc_mkdir(text)', 'execute')"
check "false|false|false" "select has_function_privilege('sc_pglib_writer', 'sc_move(text, text)', 'execute') || '|' || has_function_privilege('sc_pglib_writer', 'sc_list_files(text, interval, integer)', 'execute') || '|' || has_function_privilege('sc_pglib_writer', 'sc_remove_log(text)', 'execute')"
check "~permission denied to read a file" "set role sc_pglib_writer; select sc_read_xlsx('$work/one.xlsx')"
check "~permission denied to read a file" "set role sc_pglib_writer; select * from sc_read_xlsx('$work/one.xlsx', 1) as x(a text)"
check "~permission denied to read a file" "set role sc_pglib_writer; select * from sc_xlsx_sheets('$work/one.xlsx')"
check "~permission denied to write a file" "set role sc_pglib_writer; select sc_xlsx('$work/x.xlsx', 'select 1')"
check "~permission denied to read a file" "set role sc_pglib_writer; select * from sc_read_csv('$work/one.csv') as x(a text)"
check "~permission denied to write a file" "set role sc_pglib_writer; select sc_csv('$work/x.csv', 'select 1')"
check "~permission denied to read a file" "set role sc_pglib_writer; select * from sc_read_csvzip('$work/one.zip', 'one.csv') as x(a text)"
check "~permission denied to write a file" "set role sc_pglib_writer; select sc_csvzip('$work/x.zip', 'select 1')"
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

# --- Firebase ---
# Without the network: the options are checked, a missing user mapping is named, and a key whose
# token_uri is a closed local port fails at once, as an ERROR, or a WARNING and NULL.
check "~needs the url option" "create server fb_bad foreign data wrapper sc_firebase"
check "~url must start with https://" "create server fb_bad foreign data wrapper sc_firebase options (url 'http://example.com')"
check "~invalid option \"server\"" "create server fb_bad foreign data wrapper sc_firebase options (url 'https://example-app.firebaseio.com', server 'x')"
check "~on_error must be" "create server fb_bad foreign data wrapper sc_firebase options (url 'https://example-app.firebaseio.com', on_error 'maybe')"
check "~timeout_ms must be" "create server fb_bad foreign data wrapper sc_firebase options (url 'https://example-app.firebaseio.com', timeout_ms '5')"
check "" "create server fb foreign data wrapper sc_firebase options (url 'https://example-app.firebaseio.com', root 'devices')"
check "~no user mapping with service_account or credentials_file" "select sc_firebase_get('fb', 'x')"
check "~Valid options are service_account" "create user mapping for current_user server fb options (password 'x')"
check "~not both" "create user mapping for current_user server fb options (service_account '{}', credentials_file '/x')"
check "~is not a sc_firebase server" "create server fb_redis foreign data wrapper sc_redis options (servers 'x'); select sc_firebase_get('fb_redis', 'x')"
if command -v openssl >/dev/null; then
    key=$(openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 2>/dev/null | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')
    account='{"type": "service_account", "client_email": "test@example-app.iam.gserviceaccount.com", "token_uri": "https://127.0.0.1:1/token", "private_key": '"$key"'}'
    account_sql=${account//\'/\'\'}
    check "" "create user mapping for current_user server fb options (service_account '$account_sql')"
    check "~Firebase server \"fb\": sc::firebase: access token: no answer" "select sc_firebase_get('fb', '42')"
    check "" "create server fb_quiet foreign data wrapper sc_firebase options (url 'https://example-app.firebaseio.com', on_error 'warning'); create user mapping for current_user server fb_quiet options (service_account '$account_sql')"
    expect "on_error warning: a WARNING and NULL" "~WARNING:  Firebase server \"fb_quiet\"" "$(session <<<"select sc_firebase_get('fb_quiet', '42') is null")"
    check "~WARNING" "select sc_firebase_set('fb_quiet', '42', '{\"online\": 1}')"
    check "~WARNING" "select sc_firebase_move('fb_quiet', 'a', 'b')"
    # Foreign tables: the path fetched narrows for leading keys set with = (EXPLAIN shows it, no request).
    check "" "create foreign table fb_device (account_id text, imei text, lat float8, online int, sat int options (field 'gps/sat'), raw jsonb options (field 'gps')) server fb options (path 'all', keys 'account_id, imei')"
    check "~invalid option \"table\"" "create foreign table fb_x (a text) server fb options (table 'x')"
    check "~invalid option \"column\"" "create foreign table fb_x (a text options (column 'b')) server fb"
    check "~keys names \"nope\", which isn't a column" "create foreign table fb_y (a text) server fb options (keys 'nope'); explain select * from fb_y"
    check "~Firebase path: devices/all" "explain (costs off) select * from fb_device"
    check "~Firebase path: devices/all/{account_id}" "explain (costs off) select * from fb_device where account_id = '42'"
    check "~Firebase path: devices/all/{account_id}/{imei}" "explain (costs off) select lat from fb_device where imei = '3500' and '42' = account_id"
    check "~Firebase path: devices/all" "explain (costs off) select * from fb_device where imei = '3500'"
    expect "a parameter narrows it too" "~Firebase path: devices/all/{account_id}" "$(session <<<"set plan_cache_mode = force_generic_plan; prepare p(text) as select * from fb_device where account_id = \$1; explain (costs off) execute p('42')")"
    check "~Firebase server \"fb\": sc::firebase: access token: no answer" "select * from fb_device where account_id = '42'"
    # Writing: checked before anything is sent; then one update at the statement's end (no answer here).
    check "~has no keys option" "create foreign table fb_nokeys (a text) server fb; update fb_nokeys set a = 'x'"
    check "~key column \"imei\" can't be NULL" "insert into fb_device (account_id, imei, lat) values ('42', null, 1)"
    check "~Firebase keys can't be empty or hold" "insert into fb_device (account_id, imei, lat) values ('42', 'a/b', 1)"
    check "~Firebase path: devices/all/{account_id}" "explain (costs off) update fb_device set online = 0 where account_id = '42'"
    check "~Firebase path: devices/all/{account_id}/{imei}" "explain (costs off) delete from fb_device where account_id = '42' and imei = '1'"
    check "~Firebase server \"fb\": sc::firebase: access token: no answer" "insert into fb_device (account_id, imei, lat, online) values ('42', '350000000000009', -26.5, 1)"
    check "" "create foreign table fb_quiet_device (account_id text, imei text) server fb_quiet options (keys 'account_id,imei')"
    expect "on_error warning: a WARNING and no rows" "~WARNING:  Firebase server \"fb_quiet\"" "$(session <<<"select count(*) from fb_quiet_device")"
    expect "on_error warning: an INSERT warns" "~WARNING:  Firebase server \"fb_quiet\"" "$(session <<<"insert into fb_quiet_device values ('42', '350000000000009')")"
else
    echo "skipped: Firebase key checks (no openssl)"
fi

if [ "$check_extension" = 1 ]; then
    check "sc_pglib|1.14" "select extname, extversion from pg_extension where extname = 'sc_pglib'"
    check "" "set client_min_messages = warning; drop extension sc_pglib cascade"
    check "f" "select exists (select 1 from pg_proc where proname like 'sc_base64%' or proname like 'sc_redis%' or proname like 'sc_firebase%' or proname in ('sc_xlsx', 'sc_read_xlsx', 'sc_mkdir', 'sc_xlsx_sheets', 'sc_csv', 'sc_read_csv', 'sc_csvzip', 'sc_read_csvzip', 'sc_move', 'sc_list_files', 'sc_remove_log'))"
    # Updating from 1.0, as an existing database does.
    check "" "create extension sc_pglib version '1.0'"
    check "f" "select exists (select 1 from pg_proc where proname = 'sc_redis_get')"
    # 1.14 gives sc_firebase a handler, which PostgreSQL warns about (no foreign tables exist yet).
    check "~changing the foreign-data wrapper handler" "alter extension sc_pglib update"
    check "sc_pglib|1.14" "select extname, extversion from pg_extension where extname = 'sc_pglib'"
    check "t" "select exists (select 1 from pg_foreign_data_wrapper where fdwname = 'sc_redis')"
    check "t" "select exists (select 1 from pg_foreign_data_wrapper where fdwname = 'sc_firebase')"
    check "7" "select count(*) from pg_proc where proname like 'sc_firebase_%' and proname not in ('sc_firebase_validator', 'sc_firebase_fdw_handler')"
    check "t" "select exists (select 1 from pg_proc where proname = 'sc_redis_hgetall')"
    check "2" "select count(*) from pg_proc where proname = 'sc_redis_sadd'"
    check "7" "select count(*) from pg_proc where proname = 'sc_xlsx'"
    check "3" "select count(*) from pg_proc where proname = 'sc_read_xlsx'"
    check "2" "select count(*) from pg_proc where proname = 'sc_read_xlsx' and pronargdefaults = 1"
    check "t" "select pg_get_function_result(oid) = 'SETOF record' from pg_proc where proname = 'sc_read_xlsx' and pronargs = 3 and proargtypes[1] = 'text'::regtype"
    check "1" "select count(*) from pg_proc where proname = 'sc_xlsx_sheets'"
    check "4" "select count(*) from pg_proc where proname in ('sc_csvzip', 'sc_read_csvzip')"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed; server log:"
    tail -20 "$work/server.log"
    exit 1
fi
echo "PASSED: all checks"
