cmake_minimum_required (VERSION 3.16)
project (cubrid-importdb CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
  set (CMAKE_BUILD_TYPE RelWithDebInfo)
endif ()

list (APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package (CUBRID REQUIRED)

message (STATUS "CUBRID include : ${CUBRID_INCLUDE_DIR}")
message (STATUS "CUBRID library : ${CUBRID_CS_LIBRARY}")
message (STATUS "CUBRID bin     : ${CUBRID_BIN_DIR}")

# ---- the upstream contract check (what both CI lanes run)
add_executable (contract_check contract/contract_check.cpp)
target_link_libraries (contract_check PRIVATE CUBRID::cs)
target_compile_options (contract_check PRIVATE -Wall -Wextra -Wno-unused-parameter)

enable_testing ()
# The contract needs a live database, so the test wrapper creates one. It is
# skipped (not failed) when no server can be started -- see contract/run.sh.
add_test (NAME contract COMMAND ${CMAKE_SOURCE_DIR}/contract/run.sh)
set_tests_properties (contract PROPERTIES SKIP_RETURN_CODE 77)

install (TARGETS contract_check RUNTIME DESTINATION bin)
