cmake_minimum_required(VERSION 3.1 FATAL_ERROR)

project(afsctool VERSION 1.7.2)
set(afsctool_FULL_VERSION "${afsctool_VERSION_MAJOR}.${afsctool_VERSION_MINOR}.${afsctool_VERSION_PATCH}.${afsctool_VERSION_TWEAK}")

include(CheckTypeSize)

option(HFSCOMPRESS_TO_ZFS
    "Should afsctool compress files on ZFS dataset that claim to be HFS (testing only: the effort will be wasted)"
    OFF)
option(ZLIB_SINGLESHOT
    "Does the ZLIB compression into a sufficiently (= too) large output buffer instead of using a growing buffer.\
    May be somewhat faster at the expense of approx. 4x higher memory usage."
    OFF)
option(NEW_DRIVER_NAMES
    "If Off, use the old driver name (afsctool, and thus also zfsctool). When On, rename the drivers \
    to afscompress and zfscompress."
    OFF)
check_type_size(void* VOID_P_SIZE)
message(STATUS "sizeof(void*)=${VOID_P_SIZE}")
# check if we're not doing a universal binary build on Mac:
if(NOT ${VOID_P_SIZE} EQUAL 0)
    if(${VOID_P_SIZE} EQUAL 4)
        option(64BITBUILD
            "Build for 64bit architecture"
            OFF)
    else()
        option(64BITBUILD
            "Build for 64bit architecture"
            ON)
    endif()

    if(64BITBUILD)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m64")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m64")
#         set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m64")
    else()
    endif()
endif()
# Not currently possible on Linux because the fts functions are incompatible
# add_definitions("-D_FILE_OFFSET_BITS=64")

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/_cmake/modules ${CMAKE_MODULE_PATH} )

include(FeatureSummary)
include(CheckIncludeFile)

include(GetGitRevisionDescription)

set(CMAKE_CXX_STANDARD 11)

find_package(ZLIBP 1.2.8 REQUIRED)
find_package(SPARSEHASH)
include_directories(${ZLIBP_INCLUDE_DIR})

include_directories(${SPARSEHASH_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src)
link_directories(${SPARSEHASH_LIBRARY})
add_definitions(-DSUPPORT_PARALLEL)
if(ZLIB_SINGLESHOT)
    add_definitions(-DZLIB_SINGLESHOT_OUTBUF)
endif()
if(APPLE)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/private/lzfse/CMakeLists.txt")
        message(STATUS "Enabling LZVN and (possibly) LZFSE support")
        add_definitions(-DHAS_LZVN)
        add_definitions(-DHAS_LZFSE)
        add_subdirectory(src/private/lzfse)
        include_directories(src/private/lzfse)
        set(HAS_LZFSE 1)
    else()
        message(WARNING "Not enabling LZVN/LZFSE support - did you check out the lzfse submodule?!")
    endif()
endif()

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

add_library(PP OBJECT
    src/utils.cpp
    src/ParallelProcess.cpp
    src/Thread/Thread.cpp
    src/CritSectEx/CritSectEx.cpp
    src/CritSectEx/msemul.cpp
    src/CritSectEx/timing.c
)

if (NEW_DRIVER_NAMES)
    set(AFSCTOOL "afscompress")
    set(ZFSCTOOL "zfscompress")
else()
    set(AFSCTOOL "afsctool")
    set(ZFSCTOOL "zfsctool")
endif()
set_source_files_properties(src/afsctool.c PROPERTIES COMPILE_DEFINITIONS AFSCTOOL_PROG_NAME="${AFSCTOOL}")

add_executable(${AFSCTOOL}
    src/afsctool.c
    src/main.cpp
    $<TARGET_OBJECTS:PP>
)
if(HFSCOMPRESS_TO_ZFS)
    set_source_files_properties(src/afsctool.c PROPERTIES COMPILE_DEFINITIONS HFSCOMPRESS_TO_ZFS)
endif()

# target_include_directories(afsctool PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
set_target_properties(${AFSCTOOL} PROPERTIES
    LINK_FLAGS "${ZLIBP_LIBRARY_LDFLAGS}")
target_link_libraries(${AFSCTOOL} ${ZLIBP_LIBRARIES} ${PKG_SPARSEHASH_LIBRARIES})
if(APPLE)
    if(HAS_LZFSE)
        target_link_libraries(${AFSCTOOL} lzfse)
    endif()
    target_link_libraries(${AFSCTOOL} "-framework CoreServices")

    install(TARGETS ${AFSCTOOL} DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
else()
    target_link_libraries(${AFSCTOOL} "-lrt -ldl -lbsd -pthread")
endif()

add_executable(${ZFSCTOOL}
    src/zfsctool.cpp
    $<TARGET_OBJECTS:PP>
)
set_source_files_properties(src/zfsctool.cpp PROPERTIES COMPILE_DEFINITIONS ZFSCTOOL_PROG_NAME="${ZFSCTOOL}")

target_link_libraries(${ZFSCTOOL} ${PKG_SPARSEHASH_LIBRARIES})
if(APPLE)
    target_link_libraries(${ZFSCTOOL} "-framework CoreServices")
else()
    target_link_libraries(${ZFSCTOOL} "-lrt -ldl -lbsd -pthread")
endif()

FEATURE_SUMMARY(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
