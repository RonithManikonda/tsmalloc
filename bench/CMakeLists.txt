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
endfunction()

ts_add_bench(threadtest --iters=200)
ts_add_bench(larson --iters=20000)
ts_add_bench(xmalloc --iters=20000)
