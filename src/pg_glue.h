#pragma once
// Shared by the extension's C++ files. PostgreSQL calls them through its version-1 calling
// convention, and two rules keep C++ and PostgreSQL apart:
//  - No C++ exception may reach PostgreSQL, so every call catches them all.
//  - PostgreSQL raises errors with ereport(ERROR), which longjmp()s out without running C++
//    destructors. So nothing that can raise one runs while a C++ object is alive: arguments and
//    catalog lookups come first, the C++ work runs in a function of its own whose objects are
//    gone when it returns, and its outcome (plain data, such as a pending_error) is reported
//    after that. Results are copied with an allocation that returns NULL instead of raising.
//
// Standard and simply-cpp headers go before this one: PostgreSQL's macros can upset them.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string_view>

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <utils/memutils.h>
#if PG_VERSION_NUM >= 160000
#include <varatt.h>
#endif
}

namespace sc_pglib {
    // The bytes of a text or bytea argument (both are varlenas).
    inline std::string_view varlena_bytes(const struct varlena *value) {
        return {VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value)};
    }

    // An error or warning to report once the C++ objects are gone. Plain data, so it survives
    // the C++ code that sets it.
    struct pending_error {
        char message[512] = {};
        int code = ERRCODE_INVALID_PARAMETER_VALUE;
        int level = ERROR;

        void set(const int error_code, const int error_level, const char *format, ...) {
            code = error_code;
            level = error_level;
            va_list arguments;
            va_start(arguments, format);
            vsnprintf(message, sizeof message, format, arguments); // PostgreSQL maps it to pg_vsnprintf
            va_end(arguments);
            if (!message[0]) std::strncpy(message, "unknown error", sizeof message - 1);
        }

        [[nodiscard]] bool any() const { return message[0] != '\0'; }

        // ereport()s it: ERROR longjmp()s out, WARNING returns. Only call this from code where no
        // C++ object with a destructor is alive.
        void raise() const {
            if (any()) ereport(level, (errcode(code), errmsg("%s", message)));
        }
    };

    // A new text or bytea (the same layout) holding bytes, or nullptr with error set when it is
    // too large or memory runs out. Never raises.
    inline struct varlena *new_varlena(const std::string_view bytes, pending_error &error) {
        const Size size = VARHDRSZ + bytes.size();
        if (!AllocSizeIsValid(size)) {
            error.set(ERRCODE_PROGRAM_LIMIT_EXCEEDED, ERROR, "result exceeds the maximum value size");
            return nullptr;
        }
        auto *result = static_cast<struct varlena *>(
            MemoryContextAllocExtended(CurrentMemoryContext, size, MCXT_ALLOC_NO_OOM));
        if (!result) {
            error.set(ERRCODE_OUT_OF_MEMORY, ERROR, "out of memory");
            return nullptr;
        }
        SET_VARSIZE(result, size);
        std::memcpy(VARDATA(result), bytes.data(), bytes.size());
        return result;
    }

    // A new NUL-terminated copy of bytes (for PostgreSQL input functions such as jsonb_in), or
    // nullptr with error set when memory runs out. Never raises.
    inline char *new_cstring(const std::string_view bytes, pending_error &error) {
        if (!AllocSizeIsValid(bytes.size() + 1)) {
            error.set(ERRCODE_PROGRAM_LIMIT_EXCEEDED, ERROR, "result exceeds the maximum value size");
            return nullptr;
        }
        auto *result = static_cast<char *>(
            MemoryContextAllocExtended(CurrentMemoryContext, bytes.size() + 1, MCXT_ALLOC_NO_OOM));
        if (!result) {
            error.set(ERRCODE_OUT_OF_MEMORY, ERROR, "out of memory");
            return nullptr;
        }
        std::memcpy(result, bytes.data(), bytes.size());
        result[bytes.size()] = '\0';
        return result;
    }
}
