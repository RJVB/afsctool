cmake_minimum_required(VERSION 2.8 FATAL_ERROR)
project(afsctool)
include(FeatureSummary)

find_package(ZLIB 1.2.8 REQUIRED)
include_directories(${ZLIB_INCLUDE_DIRS})

add_executable(afsctool afsctool.c)
target_link_libraries(afsctool ${ZLIB_LIBRARIES} "-framework CoreServices")

install(TARGETS afsctool DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
FEATURE_SUMMARY(WHAT ALL)
