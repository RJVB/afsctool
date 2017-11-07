cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(afsctool VERSION 1.6.8.5)
set(afsctool_FULL_VERSION "${afsctool_VERSION_MAJOR}.${afsctool_VERSION_MINOR}.${afsctool_VERSION_PATCH}.${afsctool_VERSION_TWEAK}")

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)

include(FindPkgConfig)
include(FeatureSummary)

include(GetGitRevisionDescription)

find_package(ZLIB 1.2.8 REQUIRED)
pkg_check_modules(SPARSEHASH libsparsehash)
include_directories(${ZLIB_INCLUDE_DIRS} ${SPARSEHASH_INCLUDEDIR} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR})
link_directories(${SPARSEHASH_LIBRARY_DIRS})
add_definitions(-DSUPPORT_PARALLEL)

git_describe(GIT_FULL_VERSION "--tags")
if(GIT_FULL_VERSION)
    # development build, i.e. from a git working copy.
    # Get the current commit version using `git describe` and
    # strip the leading v plus the part that matches the version
    # declared in the project definition above.
    string(REPLACE "v${afsctool_FULL_VERSION}" "" COMMIT_VERSION "${GIT_FULL_VERSION}")
    if(NOT ${COMMIT_VERSION} MATCHES ${GIT_FULL_VERSION})
        # `git describe` agrees with afsctool_VERSION_STRING;
        # make an atomic version string by appending the additional info
        string(APPEND afsctool_FULL_VERSION ${COMMIT_VERSION})
    else()
        # `git describe` provides a seemingly unrelated version string;
        # append it in parentheses.
        string(APPEND afsctool_FULL_VERSION " (${GIT_FULL_VERSION})")
    endif()
    message(STATUS "Setting afsctool version to: ${afsctool_FULL_VERSION}")
endif()
configure_file(afsctool_fullversion.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/afsctool_fullversion.h)

add_executable(afsctool
    afsctool.c
    main.cpp
    ParallelProcess.cpp
    Thread/Thread.cpp
    CritSectEx/CritSectEx.cpp
    CritSectEx/msemul.cpp
    CritSectEx/timing.c
)
target_include_directories(afsctool PRIVATE ${CMAKE_SOURCE_DIR})
# set_target_properties(afsctool PROPERTIES COMPILE_FLAGS -DSUPPORT_PARALLEL)
target_link_libraries(afsctool ${ZLIB_LIBRARIES} ${SPARSEHASH_LIBRARIES} "-framework CoreServices")

install(TARGETS afsctool DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
FEATURE_SUMMARY(WHAT ALL)
