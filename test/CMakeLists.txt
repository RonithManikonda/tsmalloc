# One executable per area. Tests may include internal headers from src/.
function(ts_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE tsmalloc)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

ts_add_test(test_basic)
ts_add_test(test_os)
