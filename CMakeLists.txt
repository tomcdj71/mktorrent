cmake_minimum_required(VERSION 3.20)
project(mktorrent
    VERSION 1.5.0
    DESCRIPTION "A simple command-line utility to create BitTorrent metainfo files, written in C"
    HOMEPAGE_URL "https://github.com/tomcdj71/mktorrent"
    LANGUAGES C
)
include_directories(${PROJECT_SOURCE_DIR}/include)
# Default to Release if no build type is specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Options
option(MKTORRENT_OPENSSL "Use OpenSSL's SHA-1 implementation" ON)
option(MKTORRENT_PTHREADS "Enable POSIX threads for faster hashing" ON)
option(MKTORRENT_LONG_OPTIONS "Enable GNU-style long options" ON)
option(MKTORRENT_NO_HASH_CHECK "Disable hash-size consistency check" OFF)
option(MKTORRENT_USE_GITREV_VERSION "Include git commit in version string" OFF)
option(MKTORRENT_TEST "Enable test suite" OFF)
option(MKTORRENT_TEST_PERFORMANCE "Enable performance tests" OFF)
# MKTORRENT_IO_URING: auto-ON on Linux when liburing is found (override with -DMKTORRENT_IO_URING=OFF)
if(NOT DEFINED MKTORRENT_IO_URING)
    set(MKTORRENT_IO_URING_AUTO ON)
else()
    set(MKTORRENT_IO_URING_AUTO OFF)
endif()
option(MKTORRENT_IO_URING "Enable opt-in --io-uring reader (requires liburing)" OFF)

# MKTORRENT_ISAL: auto-ON on Linux+pthreads when libisal_crypto is found (override OFF/ON)
if(NOT DEFINED MKTORRENT_ISAL)
    set(MKTORRENT_ISAL_AUTO ON)
else()
    set(MKTORRENT_ISAL_AUTO OFF)
endif()
option(MKTORRENT_ISAL "Enable optional ISA-L Crypto SHA-1 for piece hashing (requires libisal_crypto + pthreads)" OFF)

set(MKTORRENT_MAX_OPENFD 100 CACHE STRING "Max open files during scan")
set(MKTORRENT_PROGRESS_PERIOD 200000 CACHE STRING "Progress update interval in μs")

# Git revision
if(MKTORRENT_USE_GITREV_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_REV
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(MKTORRENT_VERSION "${PROJECT_VERSION}-${GIT_REV}")
    else()
        set(MKTORRENT_VERSION "${PROJECT_VERSION}")
    endif()
else()
    set(MKTORRENT_VERSION "${PROJECT_VERSION}")
endif()

# Dependencies
if(MKTORRENT_OPENSSL)
    find_package(OpenSSL REQUIRED)
endif()

if(MKTORRENT_PTHREADS)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
endif()

find_package(PkgConfig QUIET)

# Optional liburing (Linux) for --io-uring
set(MKTORRENT_HAVE_IO_URING OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND MKTORRENT_PTHREADS)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBURING QUIET liburing)
    endif()
    if(MKTORRENT_IO_URING_AUTO AND LIBURING_FOUND)
        set(MKTORRENT_IO_URING ON CACHE BOOL "Enable opt-in --io-uring reader (requires liburing)" FORCE)
    endif()
    if(MKTORRENT_IO_URING)
        if(NOT LIBURING_FOUND)
            message(FATAL_ERROR "MKTORRENT_IO_URING=ON but liburing was not found (install liburing-dev)")
        endif()
        set(MKTORRENT_HAVE_IO_URING ON)
        message(STATUS "io_uring support: ON (liburing ${LIBURING_VERSION})")
    else()
        message(STATUS "io_uring support: OFF")
    endif()
elseif(MKTORRENT_IO_URING)
    message(FATAL_ERROR "MKTORRENT_IO_URING requires Linux and MKTORRENT_PTHREADS")
endif()

# Optional isa-l_crypto (Linux + pthreads) for piece SHA-1
set(MKTORRENT_HAVE_ISAL OFF)
set(MKTORRENT_ISAL_LINK_LIB "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND MKTORRENT_PTHREADS)
    if(PkgConfig_FOUND)
        pkg_check_modules(ISAL_CRYPTO QUIET libisal_crypto)
    endif()
    if(MKTORRENT_ISAL_AUTO AND ISAL_CRYPTO_FOUND)
        set(MKTORRENT_ISAL ON CACHE BOOL
            "Enable optional ISA-L Crypto SHA-1 for piece hashing (requires libisal_crypto + pthreads)" FORCE)
    endif()
    if(MKTORRENT_ISAL)
        if(NOT ISAL_CRYPTO_FOUND)
            message(FATAL_ERROR
                "MKTORRENT_ISAL=ON but libisal_crypto was not found (install isa-l_crypto / set PKG_CONFIG_PATH)")
        endif()
        # Prefer static archive when present so packages need no runtime libisal_crypto.
        find_library(MKTORRENT_ISAL_CRYPTO_STATIC
            NAMES libisal_crypto.a isal_crypto
            PATHS ${ISAL_CRYPTO_LIBRARY_DIRS}
            NO_DEFAULT_PATH)
        if(NOT MKTORRENT_ISAL_CRYPTO_STATIC)
            find_library(MKTORRENT_ISAL_CRYPTO_STATIC NAMES libisal_crypto.a)
        endif()
        if(MKTORRENT_ISAL_CRYPTO_STATIC AND MKTORRENT_ISAL_CRYPTO_STATIC MATCHES "\\.(a)$")
            set(MKTORRENT_ISAL_LINK_LIB ${MKTORRENT_ISAL_CRYPTO_STATIC})
            message(STATUS "ISA-L Crypto SHA-1: ON (static ${MKTORRENT_ISAL_CRYPTO_STATIC})")
        else()
            set(MKTORRENT_ISAL_LINK_LIB ${ISAL_CRYPTO_LIBRARIES})
            message(STATUS "ISA-L Crypto SHA-1: ON (libisal_crypto ${ISAL_CRYPTO_VERSION})")
        endif()
        set(MKTORRENT_HAVE_ISAL ON)
    else()
        message(STATUS "ISA-L Crypto SHA-1: OFF")
    endif()
elseif(MKTORRENT_ISAL)
    message(FATAL_ERROR "MKTORRENT_ISAL requires Linux and MKTORRENT_PTHREADS")
endif()

# Source files
add_executable(mktorrent
    src/main.c
    src/ftw.c
    src/init.c
    src/ll.c
    src/msg.c
    src/output.c
    src/sha1_backend.c
    $<$<BOOL:${MKTORRENT_PTHREADS}>:src/hash_pthreads.c>
    $<$<BOOL:${MKTORRENT_HAVE_IO_URING}>:src/iouring_read.c>
    $<$<NOT:$<BOOL:${MKTORRENT_PTHREADS}>>:src/hash.c>
    $<$<NOT:$<BOOL:${MKTORRENT_OPENSSL}>>:src/sha1.c>
)

target_compile_features(mktorrent PRIVATE c_std_99)
target_compile_options(mktorrent PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(mktorrent PRIVATE
    _FILE_OFFSET_BITS=64
    VERSION="${MKTORRENT_VERSION}"
    BUILD_CFG="${CMAKE_BUILD_TYPE}"
    $<$<BOOL:${MKTORRENT_PTHREADS}>:USE_PTHREADS>
    $<$<BOOL:${MKTORRENT_OPENSSL}>:USE_OPENSSL>
    $<$<BOOL:${MKTORRENT_LONG_OPTIONS}>:USE_LONG_OPTIONS>
    $<$<BOOL:${MKTORRENT_NO_HASH_CHECK}>:NO_HASH_CHECK>
    $<$<BOOL:${MKTORRENT_HAVE_IO_URING}>:USE_IO_URING>
    $<$<BOOL:${MKTORRENT_HAVE_ISAL}>:USE_ISAL>
    MAX_OPENFD=${MKTORRENT_MAX_OPENFD}
    PROGRESS_PERIOD=${MKTORRENT_PROGRESS_PERIOD}
)

if(MKTORRENT_HAVE_IO_URING)
    target_include_directories(mktorrent PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(mktorrent PRIVATE ${LIBURING_LIBRARIES})
endif()

if(MKTORRENT_HAVE_ISAL)
    target_include_directories(mktorrent PRIVATE ${ISAL_CRYPTO_INCLUDE_DIRS})
    target_link_libraries(mktorrent PRIVATE ${MKTORRENT_ISAL_LINK_LIB})
    if(ISAL_CRYPTO_LIBRARY_DIRS)
        target_link_directories(mktorrent PRIVATE ${ISAL_CRYPTO_LIBRARY_DIRS})
    endif()
endif()

target_link_libraries(mktorrent PRIVATE
    $<$<BOOL:${MKTORRENT_OPENSSL}>:OpenSSL::Crypto>
    $<$<BOOL:${MKTORRENT_PTHREADS}>:Threads::Threads>
)

# Install
include(GNUInstallDirs)
install(TARGETS mktorrent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES man/mktorrent.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# Test suite
if(MKTORRENT_TEST)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(TEST_ENV_VARS
        MKTORRENT_BINARY=$<TARGET_FILE:mktorrent>
        PERFORMANCE_TESTS=$<IF:$<BOOL:${MKTORRENT_TEST_PERFORMANCE}>,1,0>
        PERFORMANCE_LARGE_TESTS=$<IF:$<BOOL:${MKTORRENT_TEST_PERFORMANCE}>,1,0>
    )
    
    add_custom_target(test
        COMMAND ${CMAKE_COMMAND} -E env
            ${TEST_ENV_VARS}
            MKTORRENT=$<TARGET_FILE:mktorrent>
            /bin/bash ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
        DEPENDS mktorrent
        COMMENT "Running MKTorrent test suite"
    )
    
    add_custom_target(test-data
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/tests/data
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/tests/results
        COMMENT "Creating test data directories"
    )
    
    add_dependencies(test test-data)
endif()
