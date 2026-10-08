// The sc_pglib module: its PostgreSQL magic block and the base64 functions. The rules for
// mixing C++ and PostgreSQL are in pg_glue.h; the Redis functions are in sc_redis.cpp.

#include <base64.h>

#include <exception>
#include <string>
#include <string_view>

#include "pg_glue.h"

extern "C" {
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(sc_base64_encode);
PG_FUNCTION_INFO_V1(sc_base64_decode);
}

namespace {
    // Runs work, C++ code returning a std::string, and returns its result as a new text or bytea.
    // Any exception becomes an ERROR, raised after the C++ objects are gone.
    template<typename Work>
    struct varlena *varlena_result(Work work, sc_pglib::pending_error &error) noexcept {
        try {
            return sc_pglib::new_varlena(work(), error);
        } catch (const std::exception &exception) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s", exception.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "unexpected C++ exception");
        }
        return nullptr;
    }
}

// sc_base64_encode(bytea) and sc_base64_encode(text) -> text
extern "C" Datum sc_base64_encode(PG_FUNCTION_ARGS) {
    const std::string_view data = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(0));
    sc_pglib::pending_error error;
    struct varlena *result = varlena_result([data] { return sc::base64::encode(data); }, error);
    error.raise();
    PG_RETURN_POINTER(result);
}

// sc_base64_decode(text) -> bytea
extern "C" Datum sc_base64_decode(PG_FUNCTION_ARGS) {
    const std::string_view encoded = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(0));
    sc_pglib::pending_error error;
    struct varlena *result = varlena_result([encoded] { return sc::base64::decode(encoded); }, error);
    error.raise();
    PG_RETURN_POINTER(result);
}
