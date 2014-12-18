
AUTOMAKE_OPTIONS = subdir-objects

AM_CFLAGS = $(sdl2_CFLAGS) $(libfizmo_CFLAGS) $(libpixelif_CFLAGS)
AM_CPPFLAGS =
LDADD = $(sdl2_LIBS) $(libfizmo_LIBS) $(libpixelif_LIBS)

bin_PROGRAMS = fizmo-sdl2
fizmo_sdl2_SOURCES = ../locales/fizmo_sdl2_locales.c fizmo-sdl2.c

if ENABLE_TRACING
AM_CFLAGS += -DENABLE_TRACING=
endif

AM_CFLAGS += $(libsndifsdl2_CFLAGS) \
 -DSOUND_INTERFACE_STRUCT_NAME="$(SOUND_INTERFACE_STRUCT_NAME)" \
 -DSOUND_INTERFACE_INCLUDE_FILE="<$(SOUND_INTERFACE_INCLUDE_FILE)>"
LDADD += $(libsndifsdl2_LIBS)

#if ENABLE_FILELIST
#else
#AM_CFLAGS += -DDISABLE_FILELIST=
#endif

#if ENABLE_CONFIG_FILES
#else
#AM_CFLAGS += -DDISABLE_CONFIGFILES=
#endif

