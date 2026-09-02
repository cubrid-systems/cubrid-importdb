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
# ---------------------------------------------------------------------------
#  libstdc++ ABI
#
#  The published CUBRID binaries (releases and the nightly daily build) are built
#  for maximum portability: an old glibc target, a statically linked libstdc++,
#  and a compiler configured to the gcc4-compatible std::string ABI. A modern GCC
#  defaults to the __cxx11 ABI, so every engine C++ interface that passes a
#  std::string then fails to link -- concretely the cubload:: entry points that
#  import_load.cpp's serial data path uses.
#
#  Do not key this off a compiler version or a distro: the build root can change
#  under us and the guess would fail silently. Ask the LIBRARY. loaddb_install_class
#  takes a std::string &, so its mangling states the ABI:
#
#     ...ERbRSs                        the pre-C++11 (gcc4) ABI
#     ...ERbRNSt7__cxx1112basic_string the __cxx11 ABI
#
#  The plain db_* API is extern "C" and immune; only the C++ interfaces care.
# ---------------------------------------------------------------------------
function (cubrid_detect_cxx_abi out_var lib)
  set (${out_var} OFF PARENT_SCOPE)
  if (NOT UNIX OR NOT lib OR NOT EXISTS "${lib}")
    return ()
  endif ()
  find_program (NM_EXECUTABLE nm)
  if (NOT NM_EXECUTABLE)
    message (WARNING "nm not found -- cannot read the engine's libstdc++ ABI;"
                     " use -DFORCE_OLD_CXX_ABI=ON if the link fails on cubload:: symbols")
    return ()
  endif ()
  execute_process (COMMAND ${NM_EXECUTABLE} -D --defined-only "${lib}"
                   OUTPUT_VARIABLE _syms ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  if (_syms MATCHES "_Z20loaddb_install_classRKN7cubload5batchERbRSs")
    set (${out_var} ON PARENT_SCOPE)
  elseif (_syms MATCHES "_Z20loaddb_install_classRKN7cubload5batchERbRNSt7__cxx11")
    set (${out_var} OFF PARENT_SCOPE)
  else ()
    message (WARNING
      "could not read the libstdc++ ABI from ${lib}"
      " -- if the link fails on cubload:: symbols, try -DFORCE_OLD_CXX_ABI=ON")
  endif ()
endfunction ()

# Probe whichever engine library this configuration will actually link.
find_library (CUBRID_CS_PROBE NAMES cubridcs
  HINTS "${CUBRID_BUILD_DIR}/cs" "${CUBRID_BUILD_DIR}/lib" "$ENV{CUBRID}/lib"
  NO_DEFAULT_PATH)
cubrid_detect_cxx_abi (CUBRID_OLD_CXX_ABI "${CUBRID_CS_PROBE}")
if (FORCE_OLD_CXX_ABI)
  set (CUBRID_OLD_CXX_ABI ON)
endif ()
if (CUBRID_OLD_CXX_ABI)
  message (STATUS "engine library uses the pre-C++11 std::string ABI -- matching it")
else ()
  message (STATUS "engine library uses the __cxx11 std::string ABI")
endif ()

# Applied per target rather than with add_compile_definitions(), which is
# directory-scoped and would silently reach targets that never link the engine's
# C++ interfaces.
# CXX_ABI_MATCH=OFF builds with the compiler's own default ABI regardless of what
# the library uses. It exists so the claim "this utility no longer touches the
# engine's C++ interfaces" stays testable: with OFF, a build that links a
# pre-C++11-ABI install must still succeed. If it ever fails again, something
# reintroduced a call across a std::string boundary.
option (CXX_ABI_MATCH "match the engine library's libstdc++ ABI" ON)

function (cubrid_apply_cxx_abi target)
  if (CUBRID_OLD_CXX_ABI AND CXX_ABI_MATCH)
    target_compile_definitions (${target} PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
  endif ()
endfunction ()

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

  find_library (CUBRID_CS_FROM_BUILD NAMES cubridcs
    HINTS "${CUBRID_BUILD_DIR}/cs" "${CUBRID_BUILD_DIR}/lib" "$ENV{CUBRID}/lib"
    NO_DEFAULT_PATH)
  if (NOT CUBRID_CS_FROM_BUILD)
    message (FATAL_ERROR "libcubridcs not found under ${CUBRID_BUILD_DIR} or \$CUBRID/lib")
  endif ()
  cubrid_apply_cxx_abi (cubrid-importdb)
  target_link_libraries (cubrid-importdb PRIVATE "${CUBRID_CS_FROM_BUILD}")
  message (STATUS "cubrid-importdb links ${CUBRID_CS_FROM_BUILD}")

  # libcubridcs pulls in libcascci, which CUBRID installs under $CUBRID/cci/lib --
  # a directory nothing puts on the loader path. DT_RUNPATH is not transitive, and
  # the published libcubridcs.so carries a baked-in RPATH of the vendor build host
  # (/home/CUBRID/lib), so its own dependency cannot resolve through ours. Naming
  # both directories is what lets the built binary run with only $CUBRID set,
  # instead of every caller having to know about LD_LIBRARY_PATH.
  get_filename_component (CUBRID_CS_LIBDIR "${CUBRID_CS_FROM_BUILD}" DIRECTORY)
  set (CUBRID_RUNTIME_RPATH "${CUBRID_CS_LIBDIR}")
  if (DEFINED ENV{CUBRID})
    list (APPEND CUBRID_RUNTIME_RPATH "$ENV{CUBRID}/lib" "$ENV{CUBRID}/cci/lib")
  endif ()
  list (REMOVE_DUPLICATES CUBRID_RUNTIME_RPATH)
  set_target_properties (cubrid-importdb PROPERTIES
    BUILD_RPATH "${CUBRID_RUNTIME_RPATH}"
    INSTALL_RPATH "${CUBRID_RUNTIME_RPATH}")
  # ... as DT_RPATH, not DT_RUNPATH. The distinction is the whole point here:
  # DT_RUNPATH applies only to the object that declares it, so ours would be
  # consulted for libcubridcs and then ignored while the loader resolved
  # libcubridcs's OWN need for libcascci -- falling back to that library's
  # baked-in /home/CUBRID/lib and failing. DT_RPATH is inherited down the
  # dependency chain, which is what makes the binary runnable with $CUBRID alone.
  # (It is the same reason CUBRID's own cub_admin ships a DT_RPATH.)
  target_link_options (cubrid-importdb PRIVATE "-Wl,--disable-new-dtags")
  install (TARGETS cubrid-importdb RUNTIME DESTINATION bin)
endif ()

# -------------------------------------------------------- the contract check
if (BUILD_CONTRACT)
  find_package (CUBRID REQUIRED)
  add_executable (contract_check contract/contract_check.cpp)
  # contract_check only calls the extern "C" db_* API, so the ABI does not reach
  # across the boundary -- but it is built against the same library, so match it
  # anyway rather than depending on that staying true.
  cubrid_apply_cxx_abi (contract_check)
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
  # The functional suite: round-trip fidelity, every column-type family,
  # dependency ordering, FK cycles, FK violations, --dry-run, --degree, resume
  # after SIGKILL, the refusals, a damaged dump, the pre-11.5 compat rewrite, the
  # cross-version import, and the performance comparison.
  # Same contract as smoke -- 0 pass, 1 fail, 77 could not set up -- and it
  # creates every database in a scratch directory outside the source tree.
  #
  # The crossversion case SKIPS unless IT_SRC_CUBRID names a second, older
  # install; pass it through the environment to include it here.
  #
  # An hour, not the half it used to be: the perf case alone builds a 393,000-row
  # fixture and runs four paired imports, and the cases added for the
  # cross-version work cost a few minutes more on top. Run
  # `tests/run_tests.sh <case>...` directly when you do not want to pay for perf.
  add_test (NAME functional
            COMMAND ${CMAKE_SOURCE_DIR}/tests/run_tests.sh --bin $<TARGET_FILE:cubrid-importdb>)
  set_tests_properties (functional PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600)
endif ()
