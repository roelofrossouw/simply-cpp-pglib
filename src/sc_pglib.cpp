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
    using conversion = std::string (*)(std::string_view);

    // Runs convert, C++ code, and returns its result as a new text or bytea. Any exception
    // becomes an ERROR, raised after the C++ objects are gone.
    struct varlena *converted(const conversion convert, const std::string_view data,
                              sc_pglib::pending_error &error) noexcept {
        try {
            return sc_pglib::new_varlena(convert(data), error);
        } catch (const std::exception &exception) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "%s", exception.what());
        } catch (...) {
            error.set(ERRCODE_INVALID_PARAMETER_VALUE, ERROR, "unexpected C++ exception");
        }
        return nullptr;
    }

    Datum convert_argument(FunctionCallInfo fcinfo, const conversion convert) {
        const std::string_view data = sc_pglib::varlena_bytes(PG_GETARG_VARLENA_PP(0));
        sc_pglib::pending_error error;
        struct varlena *result = converted(convert, data, error);
        error.raise();
        PG_RETURN_POINTER(result);
    }
}

// sc_base64_encode(bytea) and sc_base64_encode(text) -> text
extern "C" Datum sc_base64_encode(PG_FUNCTION_ARGS) {
    return convert_argument(fcinfo, sc::base64::encode);
}

// sc_base64_decode(text) -> bytea
extern "C" Datum sc_base64_decode(PG_FUNCTION_ARGS) {
    return convert_argument(fcinfo, sc::base64::decode);
}
