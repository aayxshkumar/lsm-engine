cmake_minimum_required(VERSION 3.24)
project(lsm_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LSM_BUILD_TESTS "Build unit and crash tests" ON)
# e.g. -DLSM_SANITIZERS=address,undefined
set(LSM_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= list")

if(LSM_SANITIZERS)
  add_compile_options(-fsanitize=${LSM_SANITIZERS} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${LSM_SANITIZERS})
endif()

set(LSM_WARNINGS -Wall -Wextra -Wpedantic -Werror)

add_library(lsm
  src/crc32c.cpp
  src/posix_fs.cpp
  src/sim_fs.cpp
  src/wal.cpp
  src/iterator.cpp
  src/memtable.cpp
  src/bloom.cpp
  src/sstable.cpp
  src/manifest.cpp
  src/db.cpp
  src/crash_harness.cpp)
target_include_directories(lsm PUBLIC include)
target_compile_options(lsm PRIVATE ${LSM_WARNINGS})

add_executable(lsm_crash tools/lsm_crash.cpp)
target_link_libraries(lsm_crash PRIVATE lsm)
target_compile_options(lsm_crash PRIVATE ${LSM_WARNINGS})

add_executable(lsm_bench tools/lsm_bench.cpp)
target_link_libraries(lsm_bench PRIVATE lsm)
target_compile_options(lsm_bench PRIVATE ${LSM_WARNINGS})

if(LSM_BUILD_TESTS)
  include(FetchContent)
  # Vendored tarball when present (offline machines), else GitHub; same hash.
  set(_gtest_tarball ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest-1.15.2.tar.gz)
  if(EXISTS ${_gtest_tarball})
    set(_gtest_url ${_gtest_tarball})
  else()
    set(_gtest_url https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  endif()
  FetchContent_Declare(googletest
    URL ${_gtest_url}
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lsm_tests
    tests/coding_test.cpp
    tests/sim_fs_test.cpp
    tests/wal_test.cpp
    tests/bloom_test.cpp
    tests/sstable_test.cpp
    tests/db_test.cpp
    tests/crash_test.cpp)
  target_link_libraries(lsm_tests PRIVATE lsm GTest::gtest_main)
  target_compile_options(lsm_tests PRIVATE ${LSM_WARNINGS})
  include(GoogleTest)
  gtest_discover_tests(lsm_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
endif()
