cmake_minimum_required(VERSION 2.6)

project(purple-vk-plugin)

# Set default build type

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
      "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()

# Set global compiler-specific flags

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wshadow -fvisibility=hidden")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wshadow -fvisibility=hidden")

  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    if ("${CMAKE_CXX_COMPILER_VERSION}" VERSION_GREATER 4.7 OR "${CMAKE_CXX_COMPILER_VERSION}" VERSION_EQUAL 4.7)
      set(CXX11_FLAGS "-std=c++11")
    else()
      set(CXX11_FLAGS "-std=c++0x")
    endif()
  else()
    set(CXX11_FLAGS "-std=c++11")
  endif()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX11_FLAGS}")

  if(APPLE AND "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
  endif()

  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -ggdb")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -ggdb")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -ggdb")
endif()

# Set library-specific flags

if(UNIX)
  include(FindPkgConfig)
endif()

# Libpurple
add_definitions(-DPURPLE_PLUGINS)

# When compiling on Windows, specify PURPLE_INCLUDE_DIRS, PURPLE_LIBRARY_DIRS and PURPLE_LIBRARIES when calling CMake.
if(UNIX)
  pkg_check_modules(PURPLE REQUIRED purple)
endif()
include_directories(${PURPLE_INCLUDE_DIRS})
add_definitions(${PURPLE_CFLAGS_OTHER})
link_directories(${PURPLE_LIBRARY_DIRS})
list(APPEND EXTRA_LIBRARIES ${PURPLE_LIBRARIES})

# Store path to libpurple plugins directory for later install() command.
if (UNIX AND NOT APPLE)
  execute_process(COMMAND pkg-config purple --variable=plugindir
                  OUTPUT_VARIABLE PURPLE_PLUGIN_DIR
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

# Configure-time options

# Gio
if(UNIX)
  pkg_check_modules(GIO REQUIRED gio-2.0)
endif()
include_directories(${GIO_INCLUDE_DIRS})
add_definitions(${GIO_CFLAGS_OTHER})
link_directories(${GIO_LIBRARY_DIRS})
list(APPEND EXTRA_LIBRARIES ${GIO_LIBRARIES})

# zlib
# 
# When compiling on Windows, specify ZLIB_INCLUDE_DIRS, ZLIB_LIBRARY_DIRS and ZLIB_LIBRARIES when calling CMake.
if(UNIX)
  find_package(ZLIB REQUIRED)
endif()
include_directories(${ZLIB_INCLUDE_DIRS})
add_definitions(${ZLIB_CFLAGS_OTHER})
link_directories(${ZLIB_LIBRARY_DIRS})
list(APPEND EXTRA_LIBRARIES ${ZLIB_LIBRARIES})

# Libxml2
# 
# When compiling on Windows, specify LIBXML2_INCLUDE_DIR, LIBXML2_LIBRARY_DIRS and LIBXML2_LIBRARIES when calling CMake.
if(UNIX)
  include(FindLibXml2)
endif()
include_directories(${LIBXML2_INCLUDE_DIR})
add_definitions(${LIBXML2_DEFINITIONS})
# FindLibXml2 does not set LIBXML2_LIBRARY_DIRS, but it must be specified when compiling on Windows
link_directories(${LIBXML2_LIBRARY_DIRS})
list(APPEND EXTRA_LIBRARIES ${LIBXML2_LIBRARIES})

# Link with libgcc and libstdc++ statically on Windows to ease the deployment.
if(WIN32)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

include_directories(src)

set(SOURCES
  src/common.h
  src/httputils.cpp
  src/httputils.h
  src/miscutils.cpp
  src/miscutils.h
  src/vk-api.cpp
  src/vk-api.h
  src/vk-auth.cpp
  src/vk-auth.h
  src/vk-buddy.cpp
  src/vk-buddy.h
  src/vk-captcha.cpp
  src/vk-captcha.h
  src/vk-common.cpp
  src/vk-common.h
  src/vk-filexfer.cpp
  src/vk-filexfer.h
  src/vk-longpoll.cpp
  src/vk-longpoll.h
  src/vk-message-recv.cpp
  src/vk-message-recv.h
  src/vk-message-send.cpp
  src/vk-message-send.h
  src/vk-plugin.cpp
  src/vk-status.cpp
  src/vk-status.h
  src/vk-upload.cpp
  src/vk-upload.h
  src/vk-utils.cpp
  src/vk-utils.h

  src/contrib/purple/http.c
  src/contrib/purple/http.h
)

# Primary plugin library.

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if(APPLE)
  set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
endif()

target_link_libraries(${PROJECT_NAME} ${EXTRA_LIBRARIES})

# Install target for Linux (not tested on BSD)

if (UNIX AND NOT APPLE)
  install(TARGETS ${PROJECT_NAME} DESTINATION ${PURPLE_PLUGIN_DIR})
  install(DIRECTORY "data/protocols" DESTINATION "/usr/share/pixmaps/pidgin")
endif()
