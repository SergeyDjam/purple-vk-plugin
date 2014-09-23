cmake_minimum_required(VERSION 2.8)

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
  find_package(LibXml2 REQUIRED)
endif()
include_directories(${LIBXML2_INCLUDE_DIR})
add_definitions(${LIBXML2_DEFINITIONS})
# FindLibXml2 does not set LIBXML2_LIBRARY_DIRS, but it must be specified when compiling on Windows
link_directories(${LIBXML2_LIBRARY_DIRS})
list(APPEND EXTRA_LIBRARIES ${LIBXML2_LIBRARIES})

# A bunch of Windows-specific settings.
if(WIN32)
  # Pidgin on Windows is built with 32-bit time_t
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_USE_32BIT_TIME_T")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_USE_32BIT_TIME_T")

  # Link with libgcc and libstdc++ statically on Windows to ease the deployment.
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libgcc -static-libstdc++")

  list(APPEND EXTRA_LIBRARIES "intl")
  list(APPEND EXTRA_LIBRARIES "ws2_32")
endif()

include_directories(src)
include_directories(src/contrib/cpputils/include)

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
  src/vk-chat.cpp
  src/vk-chat.h
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
  src/vk-smileys.cpp
  src/vk-smileys.h
  src/vk-status.cpp
  src/vk-status.h
  src/vk-upload.cpp
  src/vk-upload.h
  src/vk-utils.cpp
  src/vk-utils.h

  src/contrib/picojson/picojson.h

  src/contrib/purple/http.c
  src/contrib/purple/http.h

  src/contrib/cpputils/include/cpputils/algorithm.h
  src/contrib/cpputils/include/cpputils/string.h
  src/contrib/cpputils/include/cpputils/trie.h
  src/contrib/cpputils/src/string/string.cpp
  src/contrib/cpputils/src/string/trio.c
  src/contrib/cpputils/src/string/trio.h
)

# Primary plugin library.

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if(APPLE)
  set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
endif()

target_link_libraries(${PROJECT_NAME} ${EXTRA_LIBRARIES})

# Install target for Linux (not tested on BSD)

if(UNIX AND NOT APPLE)
  # Default install prefix is /usr/local, change it to /usr
  if(NOT DEFINED CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr")
  endif()

  install(TARGETS ${PROJECT_NAME} DESTINATION ${PURPLE_PLUGIN_DIR})
  install(DIRECTORY "data/protocols" DESTINATION "share/pixmaps/pidgin")
  install(DIRECTORY "data/smileys/vk" DESTINATION "share/pixmaps/pidgin/emotes")
endif()

# Translations.

find_package(Gettext REQUIRED)
find_program(XGETTEXT_EXECUTABLE NAMES xgettext)

set(LANGUAGES ru)
set(PO_DIR data/i18n)

# Add custom target "extract-pot" for running xgettext and msgmerge for all languages.
set(POT_FILE ${PO_DIR}/${PROJECT_NAME}.pot)
set(XGETTEXT_OPTIONS --language=C++ --add-comments=i18n --keyword=i18n --no-location --no-wrap --omit-header --sort-output)
add_custom_command(OUTPUT ${POT_FILE}
  COMMAND ${XGETTEXT_EXECUTABLE} ${XGETTEXT_OPTIONS} -o ${POT_FILE} ${SOURCES}
  DEPENDS ${SOURCES}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Extract translatable messages to ${POT_FILE}"
)

# We do not want to call these commands each time on every build due to the dependency
# from po -> gmo compilation process, so we use phony targets instead of the proper
# OUTPUT in add_custom_command
set(PO_FILE_TARGETS)
set(MSGMERGE_OPTIONS --quiet --update --backup=none --no-wrap)
foreach(lang ${LANGUAGES})
  set(PO_FILE ${PO_DIR}/${lang}.po)
  add_custom_target(phony-${lang}-po
    COMMAND ${GETTEXT_MSGMERGE_EXECUTABLE} ${MSGMERGE_OPTIONS} -s ${PO_FILE} ${POT_FILE}
    DEPENDS ${POT_FILE}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Merging translatable messages to ${PO_FILE}"
  )
  list(APPEND PO_FILE_TARGETS phony-${lang}-po)
endforeach()

add_custom_target(extract-pot DEPENDS ${PO_FILE_TARGETS})

# Po -> gmo compiles are performed during each build.
set(GMO_FILES)
foreach(lang ${LANGUAGES})
  set(PO_FILE ${PO_DIR}/${lang}.po)
  set(GMO_FILE ${CMAKE_CURRENT_BINARY_DIR}/${lang}.gmo)
  add_custom_command(OUTPUT ${GMO_FILE}
    COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -o ${GMO_FILE} ${PO_FILE}
    DEPENDS ${PO_FILE}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Compiling translatable messages to ${GMO_FILE}"
  )
  list(APPEND GMO_FILES ${GMO_FILE})

  if(UNIX)
    install(FILES ${GMO_FILE} DESTINATION "share/locale/${lang}/LC_MESSAGES/" RENAME ${PROJECT_NAME}.mo)
  endif()
endforeach()

add_custom_target(gmofiles ALL DEPENDS ${GMO_FILES})

# Separate target for running cppcheck. Running cppcheck from CMake projects is easier
# because all the defines/includes are readily available. This also makes integration
# in QtCreator super-easy (just add another build target, the output format is the same
# as gcc messages).

find_program(CPPCHECK_EXECUTABLE NAMES cppcheck)

# platform=unix64 is used because it should exhibit most of the integer-related problems
# (different size for int vs long and pointers)
set(CPPCHECK_OPTIONS --enable=all --inconclusive --library=std --max-configs=1 --platform=unix64 --template=gcc)

get_property(ALL_INCLUDES DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
set(CPPCHECK_INCLUDE_OPTIONS)
foreach(include ${ALL_INCLUDES})
  # cppcheck run times are absolutely atrocious once we start adding /usr/include and similar include
  # directories, so just skip them.
  if(${include} MATCHES "^${CMAKE_SOURCE_DIR}")
    list(APPEND CPPCHECK_INCLUDE_OPTIONS "-I${include}")
  endif()
endforeach()

get_property(ALL_DEFINES DIRECTORY PROPERTY COMPILE_DEFINITIONS)
set(CPPCHECK_DEFINE_OPTIONS)
foreach(define ${ALL_DEFINES})
  list(APPEND CPPCHECK_DEFINE_OPTIONS "-D${define}")
endforeach()

set(CPPCHECK_SOURCES)
foreach(source ${SOURCES})
  if(${source} MATCHES ".*cpp$")
    list(APPEND CPPCHECK_SOURCES "${source}")
  endif()
endforeach()

add_custom_target(cppcheck
                  COMMAND ${CPPCHECK_EXECUTABLE} ${CPPCHECK_OPTIONS}
                  ${CPPCHECK_INCLUDE_OPTIONS} ${CPPCHECK_DEFINE_OPTIONS} ${CPPCHECK_SOURCES}
                  COMMENT "Running cppcheck"
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                 )
