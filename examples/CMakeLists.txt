# Examples. Each one links the installed-style public target and uses only the
# public headers.
function(ups_control_add_example name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE ups_control)
  ups_control_apply_strict_options(${name})
endfunction()

ups_control_add_example(example_lifecycle lifecycle.cpp)
ups_control_add_example(example_bypass_refusal bypass_refusal.cpp)
ups_control_add_example(example_stale_evidence stale_evidence.cpp)
ups_control_add_example(example_restart restart_without_duplicate.cpp)
