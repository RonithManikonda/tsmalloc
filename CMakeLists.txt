cmake_minimum_required(VERSION 3.20)
project(tsmalloc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(TS_BUILD_TESTS "Build unit + stress tests" ON)
option(TS_BUILD_BENCH "Build benchmarks" ON)
option(TS_STATS "Collect allocator counters (slows every malloc and free)" OFF)
option(TS_DEBUG "Check every free for double frees, bad pointers and overruns" OFF)
set(TS_SANITIZER "none" CACHE STRING "Sanitizer: none|address|thread|undefined")
set_property(CACHE TS_SANITIZER PROPERTY STRINGS none address thread undefined)

# Everything is built with -pthread: the whole point is to be fast in
# multithreaded programs, so we never measure or test a single-threaded build.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)

if(NOT TS_SANITIZER STREQUAL "none")
  add_compile_options(-fsanitize=${TS_SANITIZER} -g -O1 -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${TS_SANITIZER})
endif()

set(TS_SOURCES
  src/tsmalloc.c
  src/os.c
  src/size_class.c
  src/segment.c
  src/page.c
  src/heap.c
  src/huge.c
  src/stats.c
  src/registry.c)
# Absolute, because test/ builds a second copy of the library from its own directory.
list(TRANSFORM TS_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/)

function(ts_add_library name)
  add_library(${name} STATIC ${TS_SOURCES})
  target_include_directories(${name}
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(${name} PUBLIC Threads::Threads)
endfunction()

# Applies to the override libraries as well, so a whole program can be
# run with the checks on.
if(TS_DEBUG)
  add_compile_definitions(TS_DEBUG=1)
endif()

ts_add_library(tsmalloc)
if(TS_STATS)
  target_compile_definitions(tsmalloc PUBLIC TS_STATS=1)
endif()

# libtsmalloc.dylib: DYLD_INSERT_LIBRARIES it into a program to replace malloc.
# Not with sanitizers, whose runtimes replace malloc themselves.
if(APPLE AND TS_SANITIZER STREQUAL "none")
  add_library(tsmalloc_override SHARED ${TS_SOURCES} src/override_macos.c)
  set_target_properties(tsmalloc_override PROPERTIES OUTPUT_NAME tsmalloc)
  target_include_directories(tsmalloc_override
    PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(tsmalloc_override PUBLIC Threads::Threads)
endif()

# libtsmalloc.so for LD_PRELOAD, and libtsmalloc_override.a to link malloc in.
# -fno-builtin so the compiler can't turn the body of malloc or calloc into a
# call to itself.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TS_SANITIZER STREQUAL "none")
  set_source_files_properties(src/override_linux.c PROPERTIES COMPILE_OPTIONS -fno-builtin)
  add_library(tsmalloc_override SHARED ${TS_SOURCES} src/override_linux.c)
  set_target_properties(tsmalloc_override PROPERTIES OUTPUT_NAME tsmalloc)
  add_library(tsmalloc_override_static STATIC ${TS_SOURCES} src/override_linux.c)
  set_target_properties(tsmalloc_override_static PROPERTIES OUTPUT_NAME tsmalloc_override)
  foreach(lib tsmalloc_override tsmalloc_override_static)
    target_include_directories(${lib}
      PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${lib} PUBLIC Threads::Threads)
  endforeach()
endif()

if(APPLE)
  set(TS_PRELOAD_VAR DYLD_INSERT_LIBRARIES)
else()
  set(TS_PRELOAD_VAR LD_PRELOAD)
endif()

if(TS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(TS_BUILD_BENCH)
  add_subdirectory(bench)
endif()
