cmake_minimum_required(VERSION 2.8 FATAL_ERROR)
project(afsctool)

include(FindPkgConfig)
include(FeatureSummary)

find_package(ZLIB 1.2.8 REQUIRED)
pkg_check_modules(SPARSEHASH libsparsehash)
include_directories(${ZLIB_INCLUDE_DIRS} ${SPARSEHASH_INCLUDEDIR})
link_directories(${SPARSEHASH_LIBRARY_DIRS})
add_definitions(-DSUPPORT_PARALLEL)

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
