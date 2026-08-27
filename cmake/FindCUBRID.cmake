# FindCUBRID -- locate an installed CUBRID to build against.
#
# The whole point of this repo is that it needs only what CUBRID *installs*:
#   $CUBRID/include   the client headers (dbi.h and friends)
#   $CUBRID/lib       libcubridcs
#   $CUBRID/bin       cub_admin, which the data phase execs
#
# No CUBRID source tree is referenced. Set CUBRID (env) or -DCUBRID_ROOT=.
#
# Defines: CUBRID_FOUND, CUBRID_INCLUDE_DIR, CUBRID_CS_LIBRARY, CUBRID_BIN_DIR
#          and the imported target CUBRID::cs

if (NOT CUBRID_ROOT)
  if (DEFINED ENV{CUBRID})
    set (CUBRID_ROOT "$ENV{CUBRID}")
  endif ()
endif ()

find_path (CUBRID_INCLUDE_DIR
  NAMES dbi.h
  HINTS "${CUBRID_ROOT}/include"
  NO_DEFAULT_PATH)

find_library (CUBRID_CS_LIBRARY
  NAMES cubridcs
  HINTS "${CUBRID_ROOT}/lib"
  NO_DEFAULT_PATH)

set (CUBRID_BIN_DIR "${CUBRID_ROOT}/bin")

include (FindPackageHandleStandardArgs)
find_package_handle_standard_args (CUBRID
  REQUIRED_VARS CUBRID_INCLUDE_DIR CUBRID_CS_LIBRARY
  FAIL_MESSAGE "No installed CUBRID found. Set CUBRID or -DCUBRID_ROOT=<install>.")

if (CUBRID_FOUND AND NOT TARGET CUBRID::cs)
  add_library (CUBRID::cs UNKNOWN IMPORTED)
  set_target_properties (CUBRID::cs PROPERTIES
    IMPORTED_LOCATION "${CUBRID_CS_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CUBRID_INCLUDE_DIR}")
endif ()

# The headers this repo is allowed to use. Listed so a missing one fails the
# configure step with a clear message rather than a compile error 200 lines in.
set (CUBRID_REQUIRED_HEADERS
  dbi.h dbtype_def.h dbtype_function.h db_set_function.h db_date.h db_elo.h
  error_code.h dbtran_def.h)
foreach (h IN LISTS CUBRID_REQUIRED_HEADERS)
  if (NOT EXISTS "${CUBRID_INCLUDE_DIR}/${h}")
    message (FATAL_ERROR
      "CUBRID install at ${CUBRID_ROOT} does not install ${h}. "
      "This repo builds only against installed headers -- see contract/README.md.")
  endif ()
endforeach ()
