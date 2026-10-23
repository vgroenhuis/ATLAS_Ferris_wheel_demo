cmake_minimum_required(VERSION 3.16)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")   # a new VERSION re-runs CMake
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER LIMIT_COUNT 1)
# Changing a page file re-runs CMake, which repacks the embedded web files (main/CMakeLists.txt).
file(GLOB_RECURSE WEB_FILES "${CMAKE_CURRENT_LIST_DIR}/web/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_FILES} "${CMAKE_CURRENT_LIST_DIR}/tools/pack_web.py")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(atlas)
