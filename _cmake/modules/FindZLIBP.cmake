set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)

include(FindPkgConfig)
include(FindPackageHandleStandardArgs)
include(FeatureSummary)

pkg_check_modules(PKG_ZLIBP zlib)

if(NOT PKG_ZLIBP_FOUND)
	message(STATUS "zlib was not found with pkg-config, falling back to cmake's find routine")
     # find via cmake's standard FindZLIB
	find_package(ZLIB)
	set(PKG_ZLIBP_FOUND ${ZLIB_FOUND})
	set(ZLIBP_VERSION ${ZLIB_VERSION_STRING})
	set(ZLIBP_INCLUDE_DIR ${ZLIB_INCLUDE_DIR})
	set(ZLIBP_INCLUDE_DIRS ${ZLIB_INCLUDE_DIRS})
	set(ZLIBP_LIBRARIES ${ZLIB_LIBRARIES})
	set(ZLIBP_LIBRARY_LDFLAGS "")
else()
	set(ZLIBP_DEFINITIONS ${PKG_ZLIBP_CFLAGS_OTHER})
	set(ZLIBP_VERSION ${PKG_ZLIBP_VERSION})
	set(ZLIBP_INCLUDE_DIR ${PKG_ZLIBP_INCLUDEDIR})
	set(ZLIBP_LIBRARIES ${PKG_ZLIBP_LIBRARIES})
    if(PKG_ZLIBP_LIBRARY_DIRS)
        set(ZLIBP_LIBRARY_LDFLAGS "-L${PKG_ZLIBP_LIBRARY_DIRS}")
    else()
        set(ZLIBP_LIBRARY_LDFLAGS "")
    endif()
endif()

find_package_handle_standard_args(ZLIBP
    FOUND_VAR
        ZLIBP_FOUND
    REQUIRED_VARS
        ZLIBP_INCLUDE_DIR
    VERSION_VAR
        ZLIBP_VERSION
)

mark_as_advanced(ZLIBP_INCLUDE_DIR)

set_package_properties(ZLIBP PROPERTIES
                      DESCRIPTION "zlib compression library"
                      TYPE REQUIRED
                      URL "http://www.zlib.net/")

