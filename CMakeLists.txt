cmake_minimum_required (VERSION 3.16)
project (cubrid-importdb CXX)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
  set (CMAKE_BUILD_TYPE RelWithDebInfo)
endif ()
list (APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
#  Two build modes, because two questions are being asked.
#
#  utility (default)  -- build the real `cubrid-importdb`. Needs a CUBRID SOURCE
#                        tree and a configured BUILD tree, because importdb uses
#                        internal client headers (that is the whole reason this
#                        is a source distribution and not a binary one) and the
#                        build tree carries generated + 3rdparty headers.
#
#  contract           -- build only contract/contract_check, which needs nothing
#                        but an INSTALL. It is the cheap probe that says whether
#                        an install still behaves the way the utility assumes.
# ---------------------------------------------------------------------------
option (BUILD_UTILITY  "build cubrid-importdb (needs CUBRID_SOURCE_DIR + CUBRID_BUILD_DIR)" ON)
option (BUILD_CONTRACT "build the contract check (needs an installed CUBRID)"               ON)

# ---------------------------------------------------------------- the utility
if (BUILD_UTILITY)
  if (NOT CUBRID_SOURCE_DIR OR NOT CUBRID_BUILD_DIR)
    message (FATAL_ERROR
      "BUILD_UTILITY needs -DCUBRID_SOURCE_DIR=<engine source> and "
      "-DCUBRID_BUILD_DIR=<configured engine build>. Turn it off with "
      "-DBUILD_UTILITY=OFF to build only the contract check.")
  endif ()
  # Has the split landed upstream yet? If the engine's utility.h still declares
  # importdb's message ids and options, this repo must NOT declare them again.
  set (IMPORTDB_ENGINE_HAS_DECLS OFF)
  if (EXISTS "${CUBRID_SOURCE_DIR}/src/executables/utility.h")
    file (READ "${CUBRID_SOURCE_DIR}/src/executables/utility.h" _uh)
    if (_uh MATCHES "MSGCAT_UTIL_SET_IMPORTDB")
      set (IMPORTDB_ENGINE_HAS_DECLS ON)
      message (STATUS "engine tree still declares importdb in utility.h"
                      " -- this repo's own declarations stand down (split not landed yet)")
    endif ()
  endif ()
  if (NOT EXISTS "${CUBRID_SOURCE_DIR}/src/executables/util_support.c")
    message (FATAL_ERROR "no util_support.c under ${CUBRID_SOURCE_DIR} -- is that a CUBRID source tree?")
  endif ()

  include (EngineFlags)

  file (GLOB IMPORTDB_SOURCES "${CMAKE_SOURCE_DIR}/src/import_*.cpp")
  add_executable (cubrid-importdb
    ${IMPORTDB_SOURCES}
    src/importdb_main.cpp
    src/importdb_messages.cpp
    # util_parse_argument lives in the engine's executables, not in libcubridcs,
    # so the utility compiles the engine's own helper rather than copying it.
    "${CUBRID_SOURCE_DIR}/src/executables/util_support.c")

  target_include_directories (cubrid-importdb PRIVATE
    "${CMAKE_SOURCE_DIR}/include"      # the utility's own declarations, first
    "${CMAKE_SOURCE_DIR}/src"
    ${CUBRID_ENGINE_INCLUDES})
  if (IMPORTDB_ENGINE_HAS_DECLS)
    target_compile_definitions (cubrid-importdb PRIVATE IMPORTDB_ENGINE_HAS_DECLS)
  endif ()

  # the engine builds its .c files as C++; do the same for the helper we borrow
  set_source_files_properties ("${CUBRID_SOURCE_DIR}/src/executables/util_support.c"
    PROPERTIES LANGUAGE CXX)

  target_compile_definitions (cubrid-importdb PRIVATE
    ${CUBRID_ENGINE_DEFINES}
    # renames the declaration in message_catalog.h AND every call site together,
    # which is why the importdb sources need no edits at all
    msgcat_message=importdb_msgcat_message)
  # the utility's own ids/options must be visible before the engine's utility.h
  target_compile_options (cubrid-importdb PRIVATE
    -include importdb_utility_ext.h)

  # ---- libstdc++ ABI: the released and nightly CUBRID binaries are built with
  # the PRE-C++11 std::string ABI, so a modern GCC (which defaults to the
  # __cxx11 one) cannot link the engine's C++ interfaces -- everything that
  # passes a std::string, i.e. the cubload:: entry points import_load.cpp uses.
  # Detect it from the library instead of guessing, and match.
  find_library (CUBRID_CS_PROBE NAMES cubridcs
    HINTS "${CUBRID_BUILD_DIR}/cs" "${CUBRID_BUILD_DIR}/lib" "$ENV{CUBRID}/lib"
    NO_DEFAULT_PATH)
  set (CUBRID_OLD_CXX_ABI OFF)
  if (CUBRID_CS_PROBE AND UNIX)
    find_program (NM_EXECUTABLE nm)
    if (NM_EXECUTABLE)
      execute_process (COMMAND ${NM_EXECUTABLE} -D --defined-only "${CUBRID_CS_PROBE}"
                       OUTPUT_VARIABLE _syms ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
      # loaddb_install_class takes a std::string &; its mangling says which ABI
      if (_syms MATCHES "_Z20loaddb_install_classRKN7cubload5batchERbRSs")
        set (CUBRID_OLD_CXX_ABI ON)
      elseif (_syms MATCHES "_Z20loaddb_install_classRKN7cubload5batchERbRNSt7__cxx11")
        set (CUBRID_OLD_CXX_ABI OFF)
      else ()
        message (WARNING
          "could not read the libstdc++ ABI from ${CUBRID_CS_PROBE}"
          " -- if the link fails on cubload:: symbols, try -DFORCE_OLD_CXX_ABI=ON")
      endif ()
    endif ()
  endif ()
  if (FORCE_OLD_CXX_ABI)
    set (CUBRID_OLD_CXX_ABI ON)
  endif ()
  if (CUBRID_OLD_CXX_ABI)
    message (STATUS "engine library uses the pre-C++11 std::string ABI -- matching it")
    add_compile_definitions (_GLIBCXX_USE_CXX11_ABI=0)
  else ()
    message (STATUS "engine library uses the __cxx11 std::string ABI")
  endif ()

  find_library (CUBRID_CS_FROM_BUILD NAMES cubridcs
    HINTS "${CUBRID_BUILD_DIR}/cs" "${CUBRID_BUILD_DIR}/lib" "$ENV{CUBRID}/lib"
    NO_DEFAULT_PATH)
  if (NOT CUBRID_CS_FROM_BUILD)
    message (FATAL_ERROR "libcubridcs not found under ${CUBRID_BUILD_DIR} or \$CUBRID/lib")
  endif ()
  target_link_libraries (cubrid-importdb PRIVATE "${CUBRID_CS_FROM_BUILD}")
  message (STATUS "cubrid-importdb links ${CUBRID_CS_FROM_BUILD}")
  install (TARGETS cubrid-importdb RUNTIME DESTINATION bin)
endif ()

# -------------------------------------------------------- the contract check
if (BUILD_CONTRACT)
  find_package (CUBRID REQUIRED)
  add_executable (contract_check contract/contract_check.cpp)
  target_link_libraries (contract_check PRIVATE CUBRID::cs)
  target_compile_options (contract_check PRIVATE -Wall -Wextra -Wno-unused-parameter)
  install (TARGETS contract_check RUNTIME DESTINATION bin)
endif ()

enable_testing ()
if (BUILD_CONTRACT)
  add_test (NAME contract COMMAND ${CMAKE_SOURCE_DIR}/contract/run.sh)
  set_tests_properties (contract PROPERTIES SKIP_RETURN_CODE 77)
endif ()
if (BUILD_UTILITY)
  add_test (NAME smoke COMMAND ${CMAKE_SOURCE_DIR}/tools/smoke.sh $<TARGET_FILE:cubrid-importdb>)
  set_tests_properties (smoke PROPERTIES SKIP_RETURN_CODE 77)
endif ()
