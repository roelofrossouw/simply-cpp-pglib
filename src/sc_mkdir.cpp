// sc_mkdir(path): mkdir -p, only in /var/www/data/output and below (compiled in, no setting can
// change it), so a database function can make the folder it writes a file into without running a
// program. Nothing on the way may be a symbolic link (folders.h); a folder made is 0755 and the
// server's operating system user's. Returns the folder, without a trailing slash. EXECUTE is
// revoked from PUBLIC: grant it to the roles that need it.

#include <exception>
#include <string>

#include "folders.h"
#include "pg_glue.h"

extern "C" {
#include <utils/builtins.h>

PG_FUNCTION_INFO_V1(sc_mkdir);
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
}

// sc_mkdir(path text) -> text
extern "C" Datum sc_mkdir(PG_FUNCTION_ARGS) {
    const char *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
    sc_pglib::pending_error error;
    struct varlena *result = made(path, error);
    error.raise();
    PG_RETURN_TEXT_P(result);
}
