// PostgreSQL loads this module and calls these functions through its version-1 calling
// convention. Two rules keep C++ and PostgreSQL apart:
//  - No C++ exception may reach PostgreSQL, so each function catches them all.
//  - PostgreSQL raises errors with ereport(ERROR), which longjmp()s out without running C++
//    destructors. So nothing that can raise one runs while a C++ object is alive: arguments
//    are fetched (and detoasted) first, and the result is copied with an allocation that
//    returns NULL instead of raising. Errors are reported once the C++ objects are gone.

// Standard headers before PostgreSQL's, whose macros can upset them.
#include <base64.h>

#include <cstring>
#include <exception>
#include <string>
#include <string_view>

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <utils/memutils.h>
#if PG_VERSION_NUM >= 160000
#include <varatt.h>
#endif

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(sc_base64_encode);
PG_FUNCTION_INFO_V1(sc_base64_decode);
}

namespace {
    // The bytes of a text or bytea argument (both are varlenas).
    std::string_view varlena_bytes(const struct varlena *value) {
        return {VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value)};
    }

    // Runs work, C++ code returning a std::string, and returns its result as a new text or bytea
    // (the same layout). Any exception becomes an ERROR, raised after the C++ objects are gone.
    template<typename Work>
    Datum varlena_result(Work work) {
        char message[256] = {};
        int code = ERRCODE_INVALID_PARAMETER_VALUE;
        struct varlena *result = nullptr;
        try {
            const std::string value = work();
            const Size size = VARHDRSZ + value.size();
            if (!AllocSizeIsValid(size)) {
                code = ERRCODE_PROGRAM_LIMIT_EXCEEDED;
                std::strncpy(message, "result exceeds the maximum value size", sizeof message - 1);
            } else if (!(result = static_cast<struct varlena *>(
                MemoryContextAllocExtended(CurrentMemoryContext, size, MCXT_ALLOC_NO_OOM)))) {
                code = ERRCODE_OUT_OF_MEMORY;
                std::strncpy(message, "out of memory", sizeof message - 1);
            } else {
                SET_VARSIZE(result, size);
                std::memcpy(VARDATA(result), value.data(), value.size());
            }
        } catch (const std::exception &error) {
            std::strncpy(message, error.what(), sizeof message - 1);
        } catch (...) {
            std::strncpy(message, "unexpected C++ exception", sizeof message - 1);
        }

        if (message[0]) ereport(ERROR, (errcode(code), errmsg("%s", message)));
        PG_RETURN_POINTER(result);
    }
}

// sc_base64_encode(bytea) and sc_base64_encode(text) -> text
extern "C" Datum sc_base64_encode(PG_FUNCTION_ARGS) {
    const std::string_view data = varlena_bytes(PG_GETARG_VARLENA_PP(0));
    return varlena_result([data] { return sc::base64::encode(data); });
}

// sc_base64_decode(text) -> bytea
extern "C" Datum sc_base64_decode(PG_FUNCTION_ARGS) {
    const std::string_view encoded = varlena_bytes(PG_GETARG_VARLENA_PP(0));
    return varlena_result([encoded] { return sc::base64::decode(encoded); });
}
