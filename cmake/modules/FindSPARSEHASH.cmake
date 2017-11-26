set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)

include(FindPkgConfig)
include(FindPackageHandleStandardArgs)
include(FeatureSummary)

pkg_check_modules(PKG_SPARSEHASH libsparsehash REQUIRED)

set(SPARSEHASH_DEFINITIONS ${PKG_SPARSEHASH_CFLAGS_OTHER})
set(SPARSEHASH_VERSION ${PKG_SPARSEHASH_VERSION})
set(SPARSEHASH_INCLUDE_DIR ${PKG_SPARSEHASH_INCLUDEDIR})

find_package_handle_standard_args(SPARSEHASH
    FOUND_VAR
        SPARSEHASH_FOUND
    REQUIRED_VARS
        SPARSEHASH_INCLUDE_DIR
    VERSION_VAR
        SPARSEHASH_VERSION
)

mark_as_advanced(SPARSEHASH_INCLUDE_DIR)

set_package_properties(SPARSEHASH PROPERTIES
                      DESCRIPTION "An extremely memory-efficient hash_map implementation"
                      TYPE REQUIRED
                      URL "https://github.com/sparsehash/sparsehash")

