// Files and folders in /var/www/data/output and below (compiled in, no setting can change it), so
// database functions can make, move and list them without running a program (folders.h has the
// rules: nothing on the way may be a symbolic link):
//
//   sc_mkdir(path)                              mkdir -p; the folder, without a trailing slash
//   sc_move(from, to)                           mv; where it is now
//   sc_list_files(folder [, older_than, max])   find -type f, oldest first: path, modified, size
//
// and sc_remove_log(name): one of the server's own finished log files removed, as load_logs()
// needs once it has read one.
//
// EXECUTE on each is revoked from PUBLIC: grant it to the roles that need it.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <vector>

#include "folders.h"
#include "pg_glue.h"

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <datatype/timestamp.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <postmaster/syslogger.h>
#include <storage/fd.h>
#include <utils/builtins.h>
#include <utils/timestamp.h>

PG_FUNCTION_INFO_V1(sc_mkdir);
PG_FUNCTION_INFO_V1(sc_move);
PG_FUNCTION_INFO_V1(sc_list_files);
PG_FUNCTION_INFO_V1(sc_remove_log);
}

namespace {
    constexpr const char *output_root = "/var/www/data/output";

    struct varlena *made(const char *path, sc_pglib::pending_error &error) noexcept {
        try {
            return sc_pglib::new_varlena(sc_pglib::make_folders(output_root, path), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_mkdir: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_mkdir: unexpected C++ exception");
        }
        return nullptr;
    }

    struct varlena *moved(const char *from, const char *to, sc_pglib::pending_error &error) noexcept {
        try {
            return sc_pglib::new_varlena(sc_pglib::move_path(output_root, from, to), error);
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_move: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_move: unexpected C++ exception");
        }
        return nullptr;
    }

    // One file listed, as plain data in PostgreSQL's memory.
    struct file_row {
        char *path = nullptr;
        TimestampTz modified = 0;
        int64 size = 0;
    };

    // The files in folder modified before cutoff (none: all), oldest first, at most max (-1: all),
    // into rows (allocated in the current context). Never raises.
    int listed(const char *folder, const bool has_cutoff, const int64 cutoff_seconds, const int max, file_row *&rows,
               sc_pglib::pending_error &error) noexcept {
        try {
            auto files = sc_pglib::list_files(output_root, folder);
            std::vector<const sc_pglib::listed_file *> kept;
            for (const auto &each: files) {
                if (max >= 0 && static_cast<int>(kept.size()) >= max) break;
                if (has_cutoff && each.seconds >= cutoff_seconds) continue;
                kept.push_back(&each);
            }
            rows = static_cast<file_row *>(sc_pglib::alloc_no_oom(sizeof(file_row) * std::max<std::size_t>(kept.size(), 1), error));
            if (!rows) return 0;
            for (std::size_t i = 0; i < kept.size(); ++i) {
                rows[i].path = sc_pglib::new_cstring(kept[i]->path, error);
                if (!rows[i].path) return 0;
                // PostgreSQL's epoch is 2000-01-01.
                rows[i].modified = (kept[i]->seconds - (static_cast<int64>(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY)) * USECS_PER_SEC +
                                   kept[i]->nanoseconds / 1000;
                rows[i].size = kept[i]->size;
            }
            return static_cast<int>(kept.size());
        } catch (const std::exception &problem) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_list_files: %s", problem.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "sc_list_files: unexpected C++ exception");
        }
        return 0;
    }
}

// sc_mkdir(path text) -> text
extern "C" Datum sc_mkdir(PG_FUNCTION_ARGS) {
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
    sc_pglib::pending_error error;
    struct varlena *result = made(path, error);
    error.raise();
    PG_RETURN_TEXT_P(result);
}

// sc_move(from text, to text) -> text
extern "C" Datum sc_move(PG_FUNCTION_ARGS) {
    const char *from = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *to = text_to_cstring(PG_GETARG_TEXT_PP(1));
    sc_pglib::pending_error error;
    struct varlena *result = moved(from, to, error);
    error.raise();
    PG_RETURN_TEXT_P(result);
}

// sc_list_files(folder text, older_than interval, max integer) -> table(path text, modified timestamptz, size bigint)
extern "C" Datum sc_list_files(PG_FUNCTION_ARGS) {
    if (PG_ARGISNULL(0)) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("sc_list_files: the folder is null")));
    const char *folder = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool has_cutoff = false;
    int64 cutoff = 0;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1)) {
        // As epoch counts an interval: a month is 30 days.
        const Interval *older = PG_GETARG_INTERVAL_P(1);
        const double seconds = static_cast<double>(older->time) / USECS_PER_SEC + static_cast<double>(older->day) * SECS_PER_DAY +
                               static_cast<double>(older->month) * DAYS_PER_MONTH * SECS_PER_DAY;
        has_cutoff = true;
        cutoff = static_cast<int64>(std::floor(static_cast<double>(time(nullptr)) - seconds));
    }
    int max = -1;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2)) {
        max = PG_GETARG_INT32(2);
        if (max < 0) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_list_files: max is 0 or more, not %d", max)));
    }

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *result = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    file_row *rows = nullptr;
    sc_pglib::pending_error error;
    const int count = listed(folder, has_cutoff, cutoff, max, rows, error);
    error.raise();
    for (int i = 0; i < count; ++i) {
        Datum values[3] = {CStringGetTextDatum(rows[i].path), TimestampTzGetDatum(rows[i].modified), Int64GetDatum(rows[i].size)};
        bool nulls[3] = {false, false, false};
        tuplestore_putvalues(result->setResult, result->setDesc, values, nulls);
    }
    return static_cast<Datum>(0);
}

// sc_remove_log(name text) -> boolean: false when it isn't there. PostgreSQL code throughout (no C++
// objects), so it raises as it goes.
extern "C" Datum sc_remove_log(PG_FUNCTION_ARGS) {
    const char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (!*name || std::strchr(name, '/') || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_remove_log: \"%s\" isn't a file name in the log folder", name)));

    // The files the server writes to now (current_logfiles: "stderr log/postgresql-....log" a line).
    if (FILE *current = AllocateFile(LOG_METAINFO_DATAFILE, "r")) {
        char line[MAXPGPATH + 32];
        bool in_use = false;
        while (!in_use && fgets(line, sizeof line, current)) {
            line[strcspn(line, "\r\n")] = '\0';
            const char *path = std::strchr(line, ' ');
            if (!path) continue;
            const char *base = std::strrchr(path + 1, '/');
            in_use = std::strcmp(base ? base + 1 : path + 1, name) == 0;
        }
        FreeFile(current);
        if (in_use) ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("sc_remove_log: \"%s\" is the log file the server writes to now", name)));
    }

    // log_directory, relative to the data directory unless it's absolute.
    const char *folder = is_absolute_path(Log_directory) ? Log_directory : psprintf("%s/%s", DataDir, Log_directory);
    const int folder_fd = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder_fd < 0) ereport(ERROR, (errcode_for_file_access(), errmsg("sc_remove_log: the log folder \"%s\" can't be opened: %m", folder)));
    struct stat seen{};
    if (fstatat(folder_fd, name, &seen, AT_SYMLINK_NOFOLLOW) != 0) {
        const int problem = errno;
        close(folder_fd);
        if (problem == ENOENT) PG_RETURN_BOOL(false);
        errno = problem;
        ereport(ERROR, (errcode_for_file_access(), errmsg("sc_remove_log: \"%s\": %m", name)));
    }
    if (!S_ISREG(seen.st_mode)) {
        close(folder_fd);
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sc_remove_log: \"%s\" isn't a plain file", name)));
    }
    if (unlinkat(folder_fd, name, 0) != 0) {
        const int problem = errno;
        close(folder_fd);
        errno = problem;
        ereport(ERROR, (errcode_for_file_access(), errmsg("sc_remove_log: \"%s\" couldn't be removed: %m", name)));
    }
    close(folder_fd);
    PG_RETURN_BOOL(true);
}
