
cmake_minimum_required(VERSION 3.10)

include(ExternalProject)
include(FindPkgConfig)

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

include_directories(${LIBFIZMO_INCLUDE_DIRS})
include_directories(${LIBDRILBO_INCLUDE_DIRS})
include_directories(${LIBPIXELIF_INCLUDE_DIRS})
include_directories(${SDL2_INCLUDE_DIRS})

message("LIBFIZMO_INCLUDE_DIRS: ${LIBFIZMO_INCLUDE_DIRS}")


set (MyCSources
  src/fizmo-sdl2/fizmo-sdl2.c
  src/locales/fizmo_sdl2_locales.c
  src/locales/locale_data.c
  src/locales/locale_data.h)

add_executable(fizmo-sdl2 ${MyCSources})

target_link_directories(${PROJECT_NAME} PUBLIC ${LIBFIZMO_LIBDIR}
  ${LIBDRILBO_LIBDIR} ${LIBPIXELIF_LIBDIR} ${SDL2_LIBDIR})

target_link_libraries(${PROJECT_NAME} ${LIBFIZMO_LIBRARIES}
  ${LIBDRILBO_LIBRARIES} ${LIBPIXELIF_LIBRARIES} ${SDL2_LIBRARIES})


# TODO: Install manpage.
install(TARGETS ${PROJECT_NAME})

