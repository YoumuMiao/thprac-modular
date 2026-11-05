execute_process(COMMAND "${LINKER}" /dump /imports "${BINARY}"
    RESULT_VARIABLE status OUTPUT_VARIABLE imports ERROR_VARIABLE error)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Cannot inspect imports of ${BINARY}: ${error}")
endif()
if(imports MATCHES "GetSystemTimePreciseAsFileTime")
    message(FATAL_ERROR "Win7-incompatible time import in ${BINARY}")
endif()
message(STATUS "Compatibility import check passed for ${BINARY}")
