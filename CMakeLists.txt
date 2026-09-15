# Copyright (C) 2018-2026 Adam Leszczynski (aleszczynski@bersler.com)
#
# This file is part of OpenLogReplicator.
#
# This program is free software: you can redistribute it and/or
# modify it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public
# License along with this program; see the file LICENSE;
# If not, see <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(OpenLogReplicator
        VERSION 2.0.0
        DESCRIPTION "OpenLogReplicator, Open Source Oracle database CDC"
        HOMEPAGE_URL "https://github.com/bersler/OpenLogReplicator"
)

set(SCHEMA_VERSION "2.0.0")
string(TIMESTAMP CMAKE_BUILD_TIMESTAMP "%Y-%m-%d %H:%M" UTC)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS FALSE)
if (OSX_ARCH)
    set(CMAKE_OSX_ARCHITECTURES ${OSX_ARCH})
endif ()
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_MACOSX_RPATH 1)
    set(MACOSX TRUE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g -O3 -Wall -Wextra -Wshadow -Wnon-virtual-dtor -pedantic -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -Wpedantic -Wmisleading-indentation  -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -DCTXASSERT=0")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -Wall -Wextra -Wshadow -Wnon-virtual-dtor -pedantic -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual -Wpedantic -Wmisleading-indentation -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -pedantic  -fno-omit-frame-pointer -fsanitize=address -fsanitize=shift -fsanitize=shift-exponent -fsanitize=shift-base -fsanitize=integer-divide-by-zero -fsanitize=unreachable -fsanitize=vla-bound -fsanitize=null -fsanitize=return -fsanitize=signed-integer-overflow -fsanitize=bounds -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fsanitize=nonnull-attribute -fsanitize=bool -fsanitize=enum -fsanitize=vptr -fsanitize=pointer-overflow -fsanitize=builtin -DCTXASSERT=1")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
configure_file(config.h.in ../config.h)

if (CPU_ARCH)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=${CPU_ARCH} -mtune=${CPU_ARCH}")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -march=${CPU_ARCH} -mtune=${CPU_ARCH}")
    add_compile_definitions(CPU_ARCH=${CPU_ARCH})
endif()

if (THREAD_INFO)
    add_compile_definitions(THREAD_INFO)
endif()

find_package(Threads REQUIRED)

option(WITH_TESTS "build unit tests" OFF)
if (WITH_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

# RapidJSON
if (WITH_RAPIDJSON)
    include_directories(SYSTEM ${WITH_RAPIDJSON}/include)
else ()
    message(FATAL_ERROR "plase provide WITH_RAPIDJSON parameter with path to rapidjson library")
endif ()

# Oracle, only dynamic
if (WITH_OCI)
    include_directories(SYSTEM ${WITH_OCI}/sdk/include)
    link_directories(${WITH_OCI})
    add_compile_definitions(LINK_LIBRARY_OCI)
endif ()

if (WITH_STATIC)
    add_compile_definitions(LINK_STATIC)
endif ()

# Protobuf, dynamic or static
if (WITH_PROTOBUF)
    include_directories(SYSTEM ${WITH_PROTOBUF}/include)
    add_compile_definitions(LINK_LIBRARY_PROTOBUF)

    if (WITH_STATIC)
        add_library(static_protobuf STATIC IMPORTED)
        set_target_properties(static_protobuf PROPERTIES IMPORTED_LOCATION ${WITH_PROTOBUF}/lib/libprotobuf.a)
    else ()
        link_directories(${WITH_PROTOBUF}/lib)
    endif ()

    # ZeroMQ, just dynamic
    if (WITH_ZEROMQ)
        include_directories(SYSTEM ${WITH_ZEROMQ}/include)
        add_compile_definitions(LINK_LIBRARY_ZEROMQ)
        if (MACOSX)
            link_directories(${WITH_ZEROMQ}/lib)
        else ()
            link_directories(${WITH_ZEROMQ}/lib/x86_64-linux-gnu)
        endif ()
    endif ()
endif ()

# Kafka, dynamic or static
if (WITH_RDKAFKA)
    include_directories(SYSTEM ${WITH_RDKAFKA}/include)
    add_compile_definitions(LINK_LIBRARY_RDKAFKA)

    if (WITH_STATIC)
        find_package(OpenSSL REQUIRED)
        add_library(static_rdkafka1 STATIC IMPORTED)
        add_library(static_rdkafka2 STATIC IMPORTED)
        set_target_properties(static_rdkafka1 PROPERTIES IMPORTED_LOCATION ${WITH_RDKAFKA}/lib/librdkafka.a)
        set_target_properties(static_rdkafka2 PROPERTIES IMPORTED_LOCATION ${WITH_RDKAFKA}/lib/librdkafka++.a)
        add_library(static_rdkafka INTERFACE IMPORTED)
        set_property(TARGET static_rdkafka PROPERTY INTERFACE_LINK_LIBRARIES static_rdkafka1 static_rdkafka2 OpenSSL::Crypto OpenSSL::SSL)
    else ()
        link_directories(${WITH_RDKAFKA}/lib)
    endif ()
endif ()

# Prometheus, only dynamic
if (WITH_PROMETHEUS)
    include_directories(SYSTEM ${WITH_PROMETHEUS}/include)
    link_directories(${WITH_PROMETHEUS}/lib)
    add_compile_definitions(LINK_LIBRARY_PROMETHEUS)
endif ()

add_executable(OpenLogReplicator ${SOURCE_FILES})

if (WITH_PROTOBUF)
    add_executable(StreamClient ${SOURCE_FILES})
endif ()

add_subdirectory(src)

target_link_libraries(OpenLogReplicator Threads::Threads)

if (WITH_OCI)
    target_link_libraries(OpenLogReplicator clntshcore nnz clntsh)
endif ()

if (WITH_RDKAFKA)
    if (WITH_STATIC)
        target_link_libraries(OpenLogReplicator static_rdkafka)
    else ()
        target_link_libraries(OpenLogReplicator rdkafka++ rdkafka)
    endif ()
endif ()

if (WITH_PROMETHEUS)
    target_link_libraries(OpenLogReplicator prometheus-cpp-core prometheus-cpp-pull)
endif ()

if (WITH_PROTOBUF)
    if (WITH_STATIC)
        target_link_libraries(OpenLogReplicator static_protobuf)
        target_link_libraries(StreamClient static_protobuf)
    else ()
        target_link_libraries(OpenLogReplicator protobuf)
        target_link_libraries(StreamClient protobuf)
    endif ()
    target_link_libraries(StreamClient Threads::Threads)

    if (WITH_PROMETHEUS)
        target_link_libraries(StreamClient prometheus-cpp-core prometheus-cpp-pull)
    endif ()

    if (WITH_ZEROMQ)
        target_link_libraries(OpenLogReplicator zmq)
        target_link_libraries(StreamClient zmq)
    endif ()
endif ()

target_include_directories(OpenLogReplicator PUBLIC "${PROJECT_BINARY_DIR}")

# Link a test or benchmark executable against every optional dependency that is compiled
# in. The Lib* targets are OBJECT libraries, so the OCI, Kafka, Prometheus, Protobuf and
# ZeroMQ references inside them must be resolved by each executable itself, exactly as
# OpenLogReplicator does above.
function(olr_link_dependencies target)
    target_link_libraries(${target} Threads::Threads)
    if (WITH_OCI)
        target_link_libraries(${target} clntshcore nnz clntsh)
    endif ()
    if (WITH_RDKAFKA)
        if (WITH_STATIC)
            target_link_libraries(${target} static_rdkafka)
        else ()
            target_link_libraries(${target} rdkafka++ rdkafka)
        endif ()
    endif ()
    if (WITH_PROMETHEUS)
        target_link_libraries(${target} prometheus-cpp-core prometheus-cpp-pull)
    endif ()
    if (WITH_PROTOBUF)
        # LibCommon carries the generated protobuf code and LibWriter the stream writer
        target_link_libraries(${target} LibStream)
        if (WITH_STATIC)
            target_link_libraries(${target} static_protobuf)
        else ()
            target_link_libraries(${target} protobuf)
        endif ()
        if (WITH_ZEROMQ)
            target_link_libraries(${target} zmq)
        endif ()
    endif ()
endfunction()

# Register a test: olr_add_test(<name> <executable> [args...]). The Instant Client and its
# bundled libaio must be on the loader path when OCI is compiled in.
function(olr_add_test name)
    add_test(NAME ${name} COMMAND ${ARGN})
    if (WITH_OCI)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${WITH_OCI}:${WITH_OCI}/libaio:$ENV{LD_LIBRARY_PATH}")
    endif ()
endfunction()

if (WITH_TESTS)
    add_executable(test_timezone tests/test_timezone.cpp)
    target_link_libraries(test_timezone LibCommon)
    olr_link_dependencies(test_timezone)
    olr_add_test(test_timezone test_timezone)

    add_executable(test_epoch_nano tests/test_epoch_nano.cpp)
    target_link_libraries(test_epoch_nano LibCommon)
    olr_link_dependencies(test_epoch_nano)
    olr_add_test(test_epoch_nano test_epoch_nano)
endif ()

# Microbenchmark for the redo-vector decoders (bench/ff-bench.cpp). Built with the
# tests so it does not rot; build a Release tree for meaningful timings, the Debug
# build carries -O0 and sanitizers. Run: ./ff_bench [iterations] [columns].
if (WITH_TESTS)
    add_executable(ff_bench bench/ff-bench.cpp)
    target_link_libraries(ff_bench
            LibCommon LibReplicator LibLocales LibBuilder LibParser LibReader LibMetadata LibState LibWriter)
    olr_link_dependencies(ff_bench)
    # smoke runs: the default 8 columns and the 1-column edge of the synthetic record
    olr_add_test(ff_bench_smoke ff_bench 1000 8)
    olr_add_test(ff_bench_1col ff_bench 1000 1)

    add_executable(test_reader_parallel tests/test_reader_parallel.cpp)
    target_link_libraries(test_reader_parallel LibReader LibCommon)
    olr_link_dependencies(test_reader_parallel)
    olr_add_test(test_reader_parallel test_reader_parallel)
endif ()
