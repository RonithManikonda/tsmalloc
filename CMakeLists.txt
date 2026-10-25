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

add_library(tsmalloc STATIC
  src/tsmalloc.c
  src/os.c
  src/size_class.c)
target_include_directories(tsmalloc
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsmalloc PUBLIC Threads::Threads)

if(TS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
