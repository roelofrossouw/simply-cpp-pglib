#pragma once
// Where the file functions may read and write (sc_xlsx.cpp): the first argument, a path, as COPY
// allows it with some places open to all. A role with the privileges of role (pg_read_server_files
// to read, pg_write_server_files to write) may use any absolute path; any other role only the output
// folder and below (folders.h), and to read also the efile folders, with no symbolic link on the way.
// PostgreSQL code: raises (an ERROR naming function) when it isn't allowed.

extern "C" {
#include <postgres.h>
#include <fmgr.h>
}

namespace sc_pglib {
    const char *permitted_path(PG_FUNCTION_ARGS, const char *function, Oid role);
}
