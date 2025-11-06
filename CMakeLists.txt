
#[[

  CMakeLists.txt

  This file is part of fizmo.

  ---

  Copyright (c) 2010-2025 Christoph Ender.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
  3. Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  ---

  This is fizmo-sdl2's main buildfile. There is another one location
  in "src/locales" which is invoked by this one. The one in "src/locales"
  will pre-process the locales into a ".c" source file, so that they can
  be "hard-compiled" into fizmo-ncursesw, which solves a lot of problems
  regarding handling of external files and makes debugging in IDEs much
  simpler.

  Compile-time options are described individually below.

]]


cmake_minimum_required(VERSION 3.10)


include(ExternalProject)
include(FindPkgConfig)


# Enable external commands so it's possible to build the tools which will
# in turn create the ".h" and ".c" files required for the main build first.
project(fizmo-sdl2
  LANGUAGES C
  HOMEPAGE_URL https://fizmo.spellbreaker.org
  DESCRIPTION "fizmo sdl2 frontend"
  VERSION 0.9.0)


ExternalProject_Add(locale_data_preparation
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/locales
  BINARY_DIR ${CMAKE_BINARY_DIR}/loacles_build
  CMAKE_ARGS
  ${GLOBAL_DEFAULT_ARGS}
  ${GLOBAL_THIRDPARTY_LIB_ARGS}
  BUILD_COMMAND make
  INSTALL_COMMAND cmake -E echo "Skipping install step."
  )


option(ENABLE_TRACING "Enable tracing" OFF)
if (ENABLE_TRACING)
  add_definitions(-DENABLE_TRACING)
endif()


find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFIZMO REQUIRED libfizmo>=0.9.0)
pkg_check_modules(LIBDRILBO REQUIRED libdrilbo)
pkg_check_modules(LIBPIXELIF REQUIRED libpixelif)
pkg_check_modules(SDL2 REQUIRED sdl2)


set (c_sources
  src/fizmo-sdl2/fizmo-sdl2.c
  src/locales/fizmo_sdl2_locales.c
  src/locales/locale_data.c
  src/locales/locale_data.h)

add_executable(fizmo-sdl2 ${c_sources})

target_include_directories(${PROJECT_NAME} PRIVATE
  ${LIBFIZMO_INCLUDE_DIRS}
  ${LIBDRILBO_INCLUDE_DIRS}
  ${LIBPIXELIF_INCLUDE_DIRS}
  ${SDL2_INCLUDE_DIRS})

target_link_directories(${PROJECT_NAME} PUBLIC
  ${LIBFIZMO_LIBRARY_DIRS}
  ${LIBDRILBO_LIBRARY_DIRS}
  ${LIBPIXELIF_LIBRARY_DIRS}
  ${SDL2_LIBRARY_DIRS})

set(merged_libs
  ${LIBFIZMO_LIBRARIES}
  ${LIBDRILBO_LIBRARIES}
  ${LIBPIXELIF_LIBRARIES}
  ${SDL2_LIBRARIES})

list(REMOVE_DUPLICATES merged_libs)

target_link_libraries(${PROJECT_NAME}
  ${merged_libs})


# TODO: Install manpage.
install(TARGETS ${PROJECT_NAME})

