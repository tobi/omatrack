if(NOT DEFINED CLI OR NOT EXISTS "${CLI}")
  message(FATAL_ERROR "CLI executable is unavailable: ${CLI}")
endif()

function(run_cli expected_exit expected_pattern)
  execute_process(
    COMMAND "${CLI}" ${ARGN}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT actual_exit EQUAL expected_exit)
    message(FATAL_ERROR
      "${ARGN}: exit ${actual_exit}, expected ${expected_exit}\nstdout: ${stdout}\nstderr: ${stderr}")
  endif()
  string(CONCAT output "${stdout}" "${stderr}")
  if(NOT output MATCHES "${expected_pattern}")
    message(FATAL_ERROR
      "${ARGN}: output did not match '${expected_pattern}'\nstdout: ${stdout}\nstderr: ${stderr}")
  endif()
endfunction()

run_cli(2 "usage:" )
run_cli(2 "invalid arguments" unknown)
run_cli(2 "invalid arguments" parse)
run_cli(1 "omatrack-cli:" parse "${CMAKE_CURRENT_BINARY_DIR}/missing-session.pds")
run_cli(2 "invalid arguments" unify missing.pds)
