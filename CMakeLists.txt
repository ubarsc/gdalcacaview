###############################################################################
# Set Project name and version
project (GDALCACAVIEW)

set (PROJECT_SOURCE_DIR src)
set(GDALCACAVIEW_BIN_NAME gdalcacaview)

set(GDAL_INCLUDE_DIR /usr/local/include CACHE PATH "Include PATH for GDAL")
set(GDAL_LIB_PATH /usr/local/lib CACHE PATH "Library PATH for GDAL")

set(LIBCACA_INCLUDE_DIR /usr/local/include CACHE PATH "Include PATH for libcaca")
set(LIBCACA_LIB_PATH /usr/local/lib CACHE PATH "Library PATH for libcaca")
###############################################################################

###############################################################################
# CMake settings
cmake_minimum_required(VERSION 2.6.0)

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

include_directories(${LIBCACA_INCLUDE_DIR})
set(LIBCACA_LIBRARIES -L${LIBCACA_LIB_PATH} -lcaca )

include_directories(${GDAL_INCLUDE_DIR})
set(GDAL_LIBRARIES -L${GDAL_LIB_PATH} -lgdal)

###############################################################################

###############################################################################
# Build executables
if(CMAKE_COMPILER_IS_GNUCC)
    set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "-Wl,-rpath ${GDAL_LIB_PATH}")
endif()

add_executable(${GDALCACAVIEW_BIN_NAME} ${PROJECT_SOURCE_DIR}/gdalcacaview.c)
target_link_libraries(${GDALCACAVIEW_BIN_NAME} ${GDAL_LIBRARIES} ${LIBCACA_LIBRARIES})
install(TARGETS ${GDALCACAVIEW_BIN_NAME} DESTINATION bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

###############################################################################


