# Benchmarks. Always built with -pthread (via the tsmalloc target) and meant
# to be run from a Release build: see scripts/bench.sh once it exists.
function(ts_add_bench name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE tsmalloc)
  target_compile_definitions(${name} PRIVATE _POSIX_C_SOURCE=200809L)
endfunction()
