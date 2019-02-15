###############################################################################
# Set Project name and version
project (GDALCACAVIEW)

set(PROJECT_SOURCE_DIR src)
set(GDALCACAVIEW_BIN_NAME gdalcacaview)

###############################################################################

###############################################################################
# CMake settings
cmake_minimum_required(VERSION 2.6.0)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

IF(NOT CMAKE_BUILD_TYPE)
  #SET(CMAKE_BUILD_TYPE "DEBUG")
  SET(CMAKE_BUILD_TYPE "RELEASE")
  #SET(CMAKE_BUILD_TYPE "RELWITHDEBINFO")
  #SET(CMAKE_BUILD_TYPE "MINSIZEREL")
ENDIF()

set(CMAKE_COLOR_MAKEFILE ON)

# Allow advanced users to generate Makefiles printing detailed commands
mark_as_advanced(CMAKE_VERBOSE_MAKEFILE)

###############################################################################
# Platform and compiler specific settings

###############################################################################
# Check the required libraries are present

find_package(GDAL REQUIRED)
find_package(libcaca REQUIRED)

include_directories(${LIBCACA_INCLUDE_DIR})
include_directories(${GDAL_INCLUDE_DIR})

if(NOT MSVC)
    set(MATH_LIBRARY -lm)
endif()

if(MSVC)
    add_definitions("-D_CRT_SECURE_NO_WARNINGS")
endif()
###############################################################################

###############################################################################
# Build executables
get_filename_component(GDAL_PATH ${GDAL_LIBRARY} DIRECTORY)
if(APPLE)
    set(CMAKE_MACOSX_RPATH 1)
endif()
set(CMAKE_INSTALL_RPATH "${GDAL_PATH}")

add_executable(${GDALCACAVIEW_BIN_NAME} ${PROJECT_SOURCE_DIR}/gdalcacaview.c)
target_link_libraries(${GDALCACAVIEW_BIN_NAME} ${GDAL_LIBRARY} ${libcaca_LIBRARY} ${MATH_LIBRARY})
install(TARGETS ${GDALCACAVIEW_BIN_NAME} DESTINATION bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

###############################################################################


