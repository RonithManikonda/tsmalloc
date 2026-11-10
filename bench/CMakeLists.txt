# Benchmarks. Always built with -pthread (via the tsmalloc target) and meant
# to be run from a Release build: see scripts/bench.sh once it exists.
#
# Each program also gets a tiny ctest run (label `bench`) with both
# allocators, so a bench that crashes or deadlocks shows up in the normal
# test run and under the sanitizers. Extra arguments are passed to it.
function(ts_add_bench name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE tsmalloc)
  target_compile_definitions(${name} PRIVATE _POSIX_C_SOURCE=200809L)
  if(TS_BUILD_TESTS)
    foreach(alloc system ts)
      add_test(NAME ${name}_smoke_${alloc}
               COMMAND ${name} --alloc=${alloc} --threads=4 ${ARGN})
      set_tests_properties(${name}_smoke_${alloc} PROPERTIES LABELS bench)
    endforeach()
  endif()

  # The same program with no tsmalloc in it, for measuring the override.
  add_executable(${name}_plain ${name}.c)
  target_link_libraries(${name}_plain PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  target_compile_definitions(${name}_plain PRIVATE
    BENCH_PLAIN _POSIX_C_SOURCE=200809L _GNU_SOURCE _DARWIN_C_SOURCE)
  if(TS_BUILD_TESTS AND TARGET tsmalloc_override)
    add_dependencies(${name}_plain tsmalloc_override)
    add_test(NAME ${name}_smoke_override COMMAND ${name}_plain --threads=4 ${ARGN})
    set_tests_properties(${name}_smoke_override PROPERTIES
      LABELS bench
      ENVIRONMENT ${TS_PRELOAD_VAR}=$<TARGET_FILE:tsmalloc_override>
      PASS_REGULAR_EXPRESSION "alloc=ts")
  endif()
endfunction()

ts_add_bench(threadtest --iters=200)
ts_add_bench(larson --iters=20000)
ts_add_bench(xmalloc --iters=20000)
ts_add_bench(cache_scratch --iters=50)
