cmake_minimum_required(VERSION 3.22)
set(SC_MODULE pglib)
project(simply-cpp-${SC_MODULE} LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

# Options:
option(SC_PGLIB_BUILD_TESTS "Build simply-cpp-pglib tests" ${PROJECT_IS_TOP_LEVEL})
# Installing writes into the PostgreSQL server's own directories. On macOS those belong to
# Homebrew, and install.sh installs with sudo, so it is off there by default.
if (APPLE)
    option(SC_PGLIB_INSTALL "Install the extension into the PostgreSQL server's directories" OFF)
else ()
    option(SC_PGLIB_INSTALL "Install the extension into the PostgreSQL server's directories" ON)
endif ()

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

################
# Dependencies #
################
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.5.8)

## PostgreSQL server ##
# An extension is built for one PostgreSQL major version: the one whose server pg_config this
# uses. That is the newest one installed unless SC_PGLIB_PG_CONFIG names another. (The pg_config
# in /usr/bin or Homebrew's libpq is the client's and has no server headers.)
if (NOT SC_PGLIB_PG_CONFIG)
    file(GLOB sc_pglib_candidates
            /usr/lib/postgresql/*/bin/pg_config
            /opt/homebrew/opt/postgresql@*/bin/pg_config
            /usr/local/opt/postgresql@*/bin/pg_config)
    if (NOT sc_pglib_candidates AND NOT APPLE)
        message(STATUS "No PostgreSQL server headers found, installing postgresql-server-dev-all")
        execute_process(COMMAND sudo apt-get install -y postgresql-server-dev-all)
        file(GLOB sc_pglib_candidates /usr/lib/postgresql/*/bin/pg_config)
    endif ()
    if (NOT sc_pglib_candidates)
        message(FATAL_ERROR "No PostgreSQL server pg_config found. Install postgresql-server-dev-<version>"
                " (apt) or postgresql@<version> (brew), or pass -DSC_PGLIB_PG_CONFIG=/path/to/pg_config")
    endif ()
    list(SORT sc_pglib_candidates COMPARE NATURAL ORDER DESCENDING)
    list(GET sc_pglib_candidates 0 sc_pglib_newest)
    set(SC_PGLIB_PG_CONFIG "${sc_pglib_newest}" CACHE FILEPATH "The PostgreSQL server pg_config to build against")
endif ()

function(sc_pglib_pg_config option output)
    execute_process(COMMAND "${SC_PGLIB_PG_CONFIG}" ${option}
            OUTPUT_VARIABLE value OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set(${output} "${value}" PARENT_SCOPE)
endfunction()
sc_pglib_pg_config(--version SC_PGLIB_PG_VERSION)
sc_pglib_pg_config(--includedir-server SC_PGLIB_PG_INCLUDEDIR)
sc_pglib_pg_config(--pkglibdir SC_PGLIB_PG_PKGLIBDIR)
sc_pglib_pg_config(--sharedir SC_PGLIB_PG_SHAREDIR)
sc_pglib_pg_config(--bindir SC_PGLIB_PG_BINDIR)
string(REGEX MATCH "[0-9]+" SC_PGLIB_PG_MAJOR "${SC_PGLIB_PG_VERSION}")
if (NOT EXISTS "${SC_PGLIB_PG_INCLUDEDIR}/postgres.h")
    message(FATAL_ERROR "${SC_PGLIB_PG_CONFIG} has no server headers in ${SC_PGLIB_PG_INCLUDEDIR}")
endif ()
message(STATUS "Building for ${SC_PGLIB_PG_VERSION} (${SC_PGLIB_PG_CONFIG})")

#############
# Extension #
#############
# A module PostgreSQL loads with dlopen(), so no "lib" prefix and no link against the server:
# its symbols resolve at load time. macOS needs that said explicitly, and PostgreSQL 16 and
# later look for ".dylib" there.
add_library(sc_pglib MODULE src/sc_pglib.cpp)
target_include_directories(sc_pglib PRIVATE "${SC_PGLIB_PG_INCLUDEDIR}")
target_link_libraries(sc_pglib PRIVATE sc::sc-core)
set_target_properties(sc_pglib PROPERTIES PREFIX "")
if (APPLE)
    target_link_options(sc_pglib PRIVATE -undefined dynamic_lookup)
    if (SC_PGLIB_PG_MAJOR GREATER_EQUAL 16)
        set_target_properties(sc_pglib PROPERTIES SUFFIX ".dylib")
    endif ()
endif ()

set(SC_PGLIB_EXTENSION_FILES sql/sc_pglib.control sql/sc_pglib--1.0.sql)
if (SC_PGLIB_INSTALL)
    install(TARGETS sc_pglib LIBRARY DESTINATION "${SC_PGLIB_PG_PKGLIBDIR}" COMPONENT runtime)
    install(FILES ${SC_PGLIB_EXTENSION_FILES} DESTINATION "${SC_PGLIB_PG_SHAREDIR}/extension" COMPONENT runtime)
endif ()

#########
# Tests #
#########
if (SC_PGLIB_BUILD_TESTS)
    enable_testing()
    # Runs the extension in a throwaway PostgreSQL cluster. Skipped (77) where that isn't
    # possible: no server binaries, or running as root, which initdb refuses.
    add_test(NAME test-extension
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/extension.sh"
            "$<TARGET_FILE:sc_pglib>" "${CMAKE_CURRENT_SOURCE_DIR}/sql/sc_pglib--1.0.sql" "${SC_PGLIB_PG_BINDIR}")
    set_tests_properties(test-extension PROPERTIES SKIP_RETURN_CODE 77 LABELS database TIMEOUT 120)
endif ()

#############
# Packaging #
#############
package_sc_module(DEPENDS "postgresql-${SC_PGLIB_PG_MAJOR}")
