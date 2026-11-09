# One executable per area. Tests may include internal headers from src/.
function(ts_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE tsmalloc)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

ts_add_test(test_basic)
ts_add_test(test_os)
ts_add_test(test_internal)
ts_add_test(test_size_class)
ts_add_test(test_large)
ts_add_test(test_huge)
ts_add_test(test_stats)
# The same test against a copy of the library with the counters compiled in,
# so both modes are checked whatever TS_STATS is set to here.
ts_add_library(tsmalloc_stats)
target_compile_definitions(tsmalloc_stats PUBLIC TS_STATS=1)
add_executable(test_stats_on test_stats.c)
target_link_libraries(test_stats_on PRIVATE tsmalloc_stats)
target_include_directories(test_stats_on PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_stats_on COMMAND test_stats_on)
ts_add_test(test_registry)
ts_add_test(test_purge)

# A program using plain malloc, run with tsmalloc injected.
if(TARGET tsmalloc_override)
  add_executable(override_smoke override_smoke.c)
  target_link_libraries(override_smoke PRIVATE Threads::Threads)
  add_dependencies(override_smoke tsmalloc_override)
  add_test(NAME override_smoke COMMAND override_smoke)
  set_tests_properties(override_smoke PROPERTIES
    ENVIRONMENT DYLD_INSERT_LIBRARIES=$<TARGET_FILE:tsmalloc_override>)

  # A real program with threads. It has to be outside SIP's reach:
  # the system's /usr/bin/python3 would silently run without tsmalloc.
  find_program(TS_BREW_PYTHON python3 PATHS /opt/homebrew/bin /usr/local/bin NO_DEFAULT_PATH)
  if(TS_BREW_PYTHON)
    add_test(NAME override_python COMMAND ${TS_BREW_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/override_smoke.py)
    set_tests_properties(override_python PROPERTIES
      ENVIRONMENT DYLD_INSERT_LIBRARIES=$<TARGET_FILE:tsmalloc_override>)
  endif()
endif()
# Same binary with a delay in the environment, to check the variable is read.
add_test(NAME test_purge_env COMMAND test_purge)
set_tests_properties(test_purge_env PROPERTIES ENVIRONMENT TSMALLOC_PURGE_DELAY=7)
ts_add_test(test_segment)
ts_add_test(test_page)
ts_add_test(test_heap)
ts_add_test(test_realloc)
ts_add_test(test_align)
ts_add_test(test_threads)
ts_add_test(test_tlheap)
ts_add_test(test_abandon)
ts_add_test(test_bench)
target_compile_definitions(test_bench PRIVATE _POSIX_C_SOURCE=200809L)

# Long-running multithreaded stress; `ctest -L stress` runs just these, and
# TS_TEST_SCALE=<n> in the environment makes them n times longer.
ts_add_test(test_stress)
set_tests_properties(test_stress PROPERTIES LABELS stress)
# The harshest purge setting: every page is decommitted the moment it is free.
add_test(NAME test_stress_purge COMMAND test_stress)
set_tests_properties(test_stress_purge PROPERTIES LABELS stress ENVIRONMENT TSMALLOC_PURGE_DELAY=0)
