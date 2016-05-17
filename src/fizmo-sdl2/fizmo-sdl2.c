
/* fizmo-sdl2.c
 *
 * This file is part of fizmo.
 *
 * Copyright (c) 2011-2016 Christoph Ender.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 *
 * RE-WORK THIS PARAGRAPH *
 * This implementation doesn't evaluate the SDL events as they come from
 * SDL_PollEvent or SDL_WaitEvent. Instead, an extra thread is running which
 * continously gets the events from SDL_PollEvent and pushes them into
 * a separate "sdl_event_queue" which is managed by fizmo-sdl2 only. This
 * serves three purposes: First, it apparently allows to capture more events.
 * In case expose events are handeled as they come in, at least the linux
 * SDL2-implementation gives less and much rarer resized-events, resulting in
 * a very delayed display (up to multiple seconds) during resizing. Second,
 * the Mac OS X SDL2-implementation only gives a single resized-event in
 * case the user resizes the window, and that is when the mouse button is
 * finally released. This results in a completely black window during resize.
 * With a separate event queue and a different SDL2 event filter this problem
 * can be worked around. In addition, it's easier to collect expose events,
 * which can be coming in extremely fast, into a single expose event every
 * 10ms, which is still more than fast enough for screen updates.
 *
 * GENREAL RULES:
 * The main thread handles actual painting and events.
 * The interpreter is running in a thread.
 * Surf_Backup and sdlTexture may only be accessed after locking
 *  "sdl_backup_surface_mutex".
 * The interpreter thread can freely access Surf_Displays to plot new
 *  pixels.
 * On some draw event which is initiated by the interpreter thread this
 *  one blocks/waits until the main thread has done the actual work. During
 *  this time the main thread can freely access Surf_Display and Surf_Backup.
 */



#include <locale.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <signal.h>

#include <SDL2/SDL.h>

#include <tools/i18n.h>
#include <tools/tracelog.h>
#include <tools/z_ucs.h>
#include <tools/unused.h>
#include <tools/filesys.h>
#include <interpreter/fizmo.h>
#include <interpreter/streams.h>
#include <interpreter/config.h>
#include <interpreter/filelist.h>
#include <interpreter/wordwrap.h>
#include <interpreter/blorb.h>
#include <interpreter/savegame.h>
#include <interpreter/output.h>
#include <screen_interface/screen_pixel_interface.h>
#include <pixel_interface/pixel_interface.h>

#include <drilbo/drilbo.h>

#ifdef SOUND_INTERFACE_INCLUDE_FILE
#include SOUND_INTERFACE_INCLUDE_FILE
#endif /* SOUND_INTERFACE_INCLUDE_FILE */

#include "../locales/fizmo_sdl2_locales.h"

#define FIZMO_SDL_VERSION "0.7.0"

#define SDL_OUTPUT_CHAR_BUF_SIZE 80
#define MINIMUM_X_WINDOW_SIZE 200
#define MINIMUM_Y_WINDOW_SIZE 100

static char* interface_name = "sdl2";
static SDL_Window *sdl_window = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Surface* Surf_Display = NULL;
static SDL_Surface* Surf_Backup = NULL;
static SDL_Texture *sdlTexture = NULL;
static z_colour screen_default_foreground_color = Z_COLOUR_BLACK;
static z_colour screen_default_background_color = Z_COLOUR_WHITE;
static int sdl2_interface_screen_height_in_pixels = 800;
static int sdl2_interface_screen_width_in_pixels = 600;
static double sdl2_device_to_pixel_ratio = 1;
static SDL_TimerID timeout_timer;
//static SDL_TimerID collection_timer;
static bool timeout_timer_exists;
//static bool collection_timer_exists;
static SDL_sem *timeout_semaphore;
//static SDL_sem *collection_semaphore;
static char output_char_buf[SDL_OUTPUT_CHAR_BUF_SIZE];


struct sdl_queued_event_struct {
  int event_type;
  z_ucs z_ucs_input;
};
typedef struct sdl_queued_event_struct sdl_queued_event;

static sdl_queued_event *sdl_event_queue = NULL;
static size_t sdl_event_queue_size = 0;
static size_t sdl_event_queue_index = 0; // index of next stored event
static size_t sdl_event_queue_size_increment = 1024;
static SDL_mutex *sdl_event_queue_mutex;

static SDL_Thread *sdl_interpreter_thread = NULL;
static bool sdl_event_evluation_should_stop = false;

static SDL_mutex *sdl_main_thread_working_mutex;
static SDL_mutex *sdl_backup_surface_mutex;
static SDL_cond *sdl_main_thread_working_cond;
static bool main_thread_work_complete = true;
static bool main_thread_should_update_screen = false;
static bool main_thread_should_expose_screen = false;

static z_file *story_stream = NULL;
static z_file *blorb_stream = NULL;
static z_file *savegame_to_restore= NULL;

// 1: Detection of resize event, notification for interpreter thread. Wait for
//    interpreter to finished processing.
static SDL_mutex *resize_event_pending_mutex = NULL;
static bool resize_event_pending = false;
static bool wait_for_interpreter_when_resizing = false;

// 2: Interpreter thread received resize notifiction and is processing it:
static bool interpreter_is_processing_winch = false;

// 3: Interpreter has finished processing
static SDL_mutex *interpreter_finished_processing_winch_mutex = NULL;
// Once this cond is set, the main thread will refresh the screen.
static SDL_cond *interpreter_finished_processing_winch_cond = NULL;
static bool interpreter_finished_processing_winch = false;


static int winchCollectionEventType;

int resize_event_new_x_size;
int resize_event_new_y_size;

bool resize_via_event_filter = false;

bool do_expose = false;



// handle SQL_Quit?














static void draw_rgb_pixel(int y, int x, uint8_t r, uint8_t g, uint8_t b) {
  Uint32 *bufp;

  bufp = (Uint32 *)Surf_Display->pixels
    + y*Surf_Display->pitch/4 + x;
  *bufp = SDL_MapRGB(Surf_Display->format, r, g, b);
}


static bool is_input_timeout_available() {
  return true;
}


static char* get_interface_name() {
  return interface_name;
}


static bool is_colour_available() {
  return true;
}


static void print_startup_syntax() {
  int i;
  char **available_locales = get_available_locale_names();

  streams_latin1_output("\n");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_USAGE_DESCRIPTION);
  streams_latin1_output("\n\n");

  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_FIZMO_SDL_VERSION_P0S, FIZMO_SDL_VERSION);
  streams_latin1_output("\n");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_LIBPIXELINTERFACE_VERSION_P0S,
      get_screen_pixel_interface_version());
  streams_latin1_output("\n");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_LIBFIZMO_VERSION_P0S,
      FIZMO_VERSION);
  streams_latin1_output("\n");
  if (active_sound_interface != NULL) {
    streams_latin1_output(active_sound_interface->get_interface_name());
    streams_latin1_output(" ");
    streams_latin1_output("version ");
    streams_latin1_output(active_sound_interface->get_interface_version());
    streams_latin1_output(".\n");
  }
  streams_latin1_output("\n");

  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_LOCALES_AVAILIABLE);
  streams_latin1_output(" ");

  i = 0;
  while (available_locales[i] != NULL) {
    if (i != 0)
      streams_latin1_output(", ");

    streams_latin1_output(available_locales[i]);
    free(available_locales[i]);
    i++;
  }
  free(available_locales);
  streams_latin1_output(".\n");

  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_LOCALE_SEARCH_PATH);
  streams_latin1_output(": ");
  streams_latin1_output(
      get_i18n_default_search_path());
  streams_latin1_output(".\n");

  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_COLORS_AVAILABLE);
  streams_latin1_output(": ");

  for (i=Z_COLOUR_BLACK; i<=Z_COLOUR_WHITE; i++) {
    if (i != Z_COLOUR_BLACK)
      streams_latin1_output(", ");
    streams_latin1_output(z_colour_names[i]);
  }
  streams_latin1_output(".\n\n");

  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_VALID_OPTIONS_ARE);
  streams_latin1_output("\n");

  streams_latin1_output( " -l,  --set-locale: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_LOCALE_NAME_FOR_INTERPRETER_MESSAGES);
  streams_latin1_output("\n");

  streams_latin1_output( " -pr, --predictable: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_START_WITH_RANDOM_GENERATOR_IN_PREDICTABLE_MODE);
  streams_latin1_output("\n");

  streams_latin1_output( " -ra, --random: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_START_WITH_RANDOM_GENERATOR_IN_RANDOM_MODE);
  streams_latin1_output("\n");

  streams_latin1_output( " -st, --start-transcript: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_START_GAME_WITH_TRANSCRIPT_ENABLED);
  streams_latin1_output("\n");

  streams_latin1_output( " -tf, --transcript-filename: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_TRANSCRIPT_FILENAME);
  streams_latin1_output("\n");

  streams_latin1_output( " -rc, --record-commands: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_START_GAME_WITH_RECORDING_COMMANDS);
  streams_latin1_output("\n");

  streams_latin1_output( " -fi, --start-file-input: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_START_GAME_WITH_INPUT_FROM_FILE);
  streams_latin1_output("\n");

  streams_latin1_output( " -if, --input-filename: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_FILENAME_TO_READ_COMMANDS_FROM);
  streams_latin1_output("\n");

  streams_latin1_output( " -rf, --record-filename: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_FILENAME_TO_RECORD_INPUT_TO);
  streams_latin1_output("\n");

  streams_latin1_output( " -f,  --foreground-color: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_FOREGROUND_COLOR);
  streams_latin1_output("\n");

  streams_latin1_output( " -b,  --background-color: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_BACKGROUND_COLOR);
  streams_latin1_output("\n");

  streams_latin1_output( " -cc, --cursor-color: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_CURSOR_COLOR);
  streams_latin1_output("\n");

  streams_latin1_output( " -fs, --font-size: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_FONT_SIZE);
  streams_latin1_output("\n");

  streams_latin1_output( " -lm, --left-margin: " );
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_LEFT_MARGIN_SIZE);
  streams_latin1_output("\n");

  streams_latin1_output( " -rm, --right-margin: " );
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_RIGHT_MARGIN_SIZE);
  streams_latin1_output("\n");

  streams_latin1_output( " -um, --umem: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_USE_UMEM_FOR_SAVEGAMES);
  streams_latin1_output("\n");

  streams_latin1_output( " -dh, --disable-hyphenation: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_DISABLE_HYPHENATION);
  streams_latin1_output("\n");

  streams_latin1_output( " -ds, --disable-sound: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_DISABLE_SOUND);
  streams_latin1_output("\n");

  streams_latin1_output( " -t,  --set-tandy-flag: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_TANDY_FLAG);
  streams_latin1_output("\n");

  streams_latin1_output( " -sy, --sync-transcript: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SYNC_TRANSCRIPT);
  streams_latin1_output("\n");

  streams_latin1_output( " -ww, --window-width: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_WINDOW_WIDTH);
  streams_latin1_output("\n");

  streams_latin1_output( " -wh, --window-height: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SET_WINDOW_HEIGHT);
  streams_latin1_output("\n");

  streams_latin1_output( " -h,  --help: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SHOW_HELP_MESSAGE_AND_EXIT);
  streams_latin1_output("\n");

  streams_latin1_output("\n");
}


static int parse_config_parameter(char *UNUSED(key), char *UNUSED(value)) {
  return -2;
}


static char *get_config_value(char *UNUSED(key)) {
  return NULL;
}


static char **get_config_option_names() {
  return NULL;
}


static void link_interface_to_story(struct z_story *story) {
  z_image *frontispiece;
  int frontispiece_resource_number;
  z_image *window_icon_zimage;
  uint32_t *icon_pixels;
  int x, y, pixel_left_shift;
  uint8_t red, green, blue;
  uint8_t *image_data;
  SDL_Surface *icon_surface;

  SDL_SetWindowTitle(sdl_window, story->title);

  frontispiece_resource_number
    = active_blorb_interface->get_frontispiece_resource_number(
        active_z_story->blorb_map);

  if (frontispiece_resource_number >= 0) {
    TRACE_LOG("frontispiece resnum: %d.\n", frontispiece_resource_number);
    if ((frontispiece = get_blorb_image(frontispiece_resource_number))!=NULL) {
      if ( (frontispiece->image_type == DRILBO_IMAGE_TYPE_RGB)
          || (frontispiece->image_type != DRILBO_IMAGE_TYPE_GRAYSCALE) ) {

        pixel_left_shift = 8 - frontispiece->bits_per_sample;

        window_icon_zimage = scale_zimage(
            frontispiece,
            128,
            128);

        icon_pixels = fizmo_malloc(sizeof(uint32_t) * 128 * 128);

        image_data = window_icon_zimage->data;

        for (y=0; y<128; y++) {
          for (x=0; x<128; x++) {

            red = *(image_data++);

            if (window_icon_zimage->image_type == DRILBO_IMAGE_TYPE_RGB) {
              green = *(image_data++);
              blue = *(image_data++);

              if (pixel_left_shift > 0) {
                red <<= pixel_left_shift;
                green <<= pixel_left_shift;
                blue <<= pixel_left_shift;
              }
              else if (pixel_left_shift < 0) {
                red >>= pixel_left_shift;
                green >>= pixel_left_shift;
                blue >>= pixel_left_shift;
              }

              icon_pixels[y*128+x]
                = (red << 24) | (green << 16) | (blue << 8);
            }
            else if (window_icon_zimage->image_type
                == DRILBO_IMAGE_TYPE_GRAYSCALE) {

              if (pixel_left_shift > 0) {
                red <<= pixel_left_shift;
              }
              else if (pixel_left_shift < 0) {
                red >>= pixel_left_shift;
              }

              icon_pixels[y*128+x]
                = (red << 24) | (red << 16) | (red << 8);
            }
          }
        }

        icon_surface = SDL_CreateRGBSurfaceFrom(
            icon_pixels,
            128,
            128,
            32,
            sizeof(uint32_t) * 128,
            0xff000000,
            0x00ff0000,
            0x0000ff00,
            0x00000000);

        SDL_SetWindowIcon(sdl_window, icon_surface);

        SDL_FreeSurface(icon_surface);
        free_zimage(window_icon_zimage);
        window_icon_zimage= NULL;
      }

      free_zimage(frontispiece);
      frontispiece = NULL;
    }
  }
}


static void reset_interface() {
}


static int sdl2_close_interface(z_ucs *UNUSED(error_message)) {
  return 0;
}


static void output_interface_info() {
  (void)i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_FIZMO_SDL_VERSION_P0S,
      FIZMO_SDL_VERSION);
  (void)streams_latin1_output("\n");
  (void)i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_LIBDRILBO_VERSION_P0S,
      get_drilbo_version());
  (void)streams_latin1_output("\n");
}


static int get_screen_width_in_pixels() {
  return sdl2_interface_screen_width_in_pixels;
}


static int get_screen_height_in_pixels() {
  return sdl2_interface_screen_height_in_pixels;
}


static double get_device_to_pixel_ratio() {
  return sdl2_device_to_pixel_ratio;
}


void update_screen() {
  TRACE_LOG("Doing update_screen().\n");

  if (interpreter_is_processing_winch == true) {
    interpreter_is_processing_winch = false;
    SDL_LockMutex(interpreter_finished_processing_winch_mutex);
    interpreter_finished_processing_winch = true;
    SDL_CondSignal(interpreter_finished_processing_winch_cond);
    SDL_UnlockMutex(interpreter_finished_processing_winch_mutex);
  }
  else {
    // This has to be done by the main thread.
    TRACE_LOG("Waiting for sdl_main_thread_working_mutex.\n");
    SDL_LockMutex(sdl_main_thread_working_mutex);
    TRACE_LOG("Locked sdl_main_thread_working_mutex.\n");
    main_thread_should_update_screen = true;
    main_thread_work_complete = false;
    while (main_thread_work_complete == false) {
      TRACE_LOG("Waiting for sdl_main_thread_working_cond ...\n");
      SDL_CondWait(sdl_main_thread_working_cond, sdl_main_thread_working_mutex);
    }
    TRACE_LOG("Found sdl_main_thread_working_cond.\n");
    SDL_UnlockMutex(sdl_main_thread_working_mutex);
  }

  TRACE_LOG("Finished update_screen().\n");
}


static void process_resize() {

  sdl2_interface_screen_width_in_pixels = resize_event_new_x_size;
  sdl2_interface_screen_height_in_pixels = resize_event_new_y_size;

  SDL_SetWindowSize(sdl_window,
      sdl2_interface_screen_width_in_pixels,
      sdl2_interface_screen_height_in_pixels);

  sdl2_interface_screen_width_in_pixels *= sdl2_device_to_pixel_ratio;
  sdl2_interface_screen_height_in_pixels *= sdl2_device_to_pixel_ratio;

  TRACE_LOG("%d x %d\n", sdl2_interface_screen_width_in_pixels,
      sdl2_interface_screen_height_in_pixels);

  SDL_FreeSurface(Surf_Display);
  if ((Surf_Display = SDL_CreateRGBSurface(
          0,
          sdl2_interface_screen_width_in_pixels,
          sdl2_interface_screen_height_in_pixels,
          32,
          0x00FF0000,
          0x0000FF00,
          0x000000FF,
          0xFF000000)) == NULL) {
    i18n_translate_and_exit(
        fizmo_sdl2_module_name,
        i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
        -1,
        "SDL_GetWindowSurface");
  }

  SDL_LockMutex(sdl_backup_surface_mutex);

  SDL_FreeSurface(Surf_Backup);
  if ((Surf_Backup = SDL_CreateRGBSurface(
          0,
          sdl2_interface_screen_width_in_pixels,
          sdl2_interface_screen_height_in_pixels,
          32,
          0x00FF0000,
          0x0000FF00,
          0x000000FF,
          0xFF000000)) == NULL) {
    i18n_translate_and_exit(
        fizmo_sdl2_module_name,
        i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
        -1,
        "SDL_GetWindowSurface");
  }

  SDL_DestroyTexture(sdlTexture);
  if ((sdlTexture = SDL_CreateTexture(sdl_renderer,
          SDL_PIXELFORMAT_ARGB8888,
          SDL_TEXTUREACCESS_STREAMING,
          sdl2_interface_screen_width_in_pixels,
          sdl2_interface_screen_height_in_pixels)) == NULL) {
    i18n_translate_and_exit(
        fizmo_sdl2_module_name,
        i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
        -1,
        "SDL_CreateTexture");
  }

  SDL_UnlockMutex(sdl_backup_surface_mutex);
}


// This function is executed in the context of the interpreter thread.
static int pull_sdl_event_from_queue(int *event_type, z_ucs *z_ucs_input) {
  int result;
  bool resize_event_has_to_be_processed = false;
  bool wait_for_terp = false;

  // Before we actually lookingg at the event queue, we have to process
  // window-resizes seperately (since resizing blocks the event queue in SDL's
  // Mac OS X implementation.

  SDL_LockMutex(resize_event_pending_mutex);
  if (resize_event_pending == true) {
    TRACE_LOG("Gotta resize.\n");
    wait_for_terp = wait_for_interpreter_when_resizing;
    resize_event_has_to_be_processed = true;
    resize_event_pending = false;
  }
  SDL_UnlockMutex(resize_event_pending_mutex);

  if (do_expose == true) {
    TRACE_LOG("Waiting for sdl_main_thread_working_mutex.\n");
    SDL_LockMutex(sdl_main_thread_working_mutex);
    TRACE_LOG("Locked sdl_main_thread_working_mutex.\n");
    main_thread_should_expose_screen = true;
    main_thread_work_complete = false;
    while (main_thread_work_complete == false) {
      TRACE_LOG("Waiting for sdl_main_thread_working_cond ...\n");
      SDL_CondWait(sdl_main_thread_working_cond, sdl_main_thread_working_mutex);
    }
    TRACE_LOG("Found sdl_main_thread_working_cond.\n");
    do_expose = false;
    SDL_UnlockMutex(sdl_main_thread_working_mutex);
  }

  if (resize_event_has_to_be_processed == true) {
    process_resize();
    *event_type = EVENT_WAS_WINCH;
    if (wait_for_terp == true) {
      TRACE_LOG("WAITING.\n");
      interpreter_is_processing_winch = true;
    }
    *z_ucs_input = 0;
    result = 0;
  }
  else {
    // In case we don't have to process resizing events we check the event queue.

    SDL_LockMutex(sdl_event_queue_mutex);

    if (sdl_event_queue_index > 0) {
      *event_type = sdl_event_queue[0].event_type;
      *z_ucs_input = sdl_event_queue[0].z_ucs_input;
      if (--sdl_event_queue_index > 0) {
        memmove(
            sdl_event_queue,
            sdl_event_queue + 1,
            sizeof(sdl_queued_event)*sdl_event_queue_index);
      }
      result = 0;
    }
    else {
      result = -1;
    }

    SDL_UnlockMutex(sdl_event_queue_mutex);
  }

  return result;
}


static void push_sdl_event_to_queue(int event_type, z_ucs z_ucs_input) {
  TRACE_LOG("push\n");
  SDL_LockMutex(sdl_event_queue_mutex);
  if (sdl_event_queue_index == sdl_event_queue_size) {
    sdl_event_queue_size += sdl_event_queue_size_increment;
    sdl_event_queue = fizmo_realloc(
        sdl_event_queue, sizeof(sdl_queued_event)*sdl_event_queue_size);
  }
  sdl_event_queue[sdl_event_queue_index].event_type = event_type;
  sdl_event_queue[sdl_event_queue_index].z_ucs_input = z_ucs_input;
  sdl_event_queue_index++;
  SDL_UnlockMutex(sdl_event_queue_mutex);
}


static Uint32 timeout_callback(Uint32 interval, void *UNUSED(param)) {
  SDL_SemWait(timeout_semaphore);

  if (timeout_timer_exists == true) {
    SDL_RemoveTimer(timeout_timer);
    timeout_timer_exists = false;
    push_sdl_event_to_queue(EVENT_WAS_TIMEOUT, 0);
  }

  SDL_SemPost(timeout_semaphore);

  return interval;
}


static int get_next_event(z_ucs *z_ucs_input, int timeout_millis,
    bool poll_only) {
  int wait_result, result = -1;

  TRACE_LOG("Invoked get_next_event.\n");

  if (timeout_millis > 0) {
    TRACE_LOG("input timeout: %d ms.\n", timeout_millis);
    SDL_SemWait(timeout_semaphore);
    timeout_timer = SDL_AddTimer(timeout_millis, &timeout_callback, NULL);
    timeout_timer_exists = true;
    SDL_SemPost(timeout_semaphore);
  }

  while (true) {
    TRACE_LOG("Pulling next event from queue ...\n");
    wait_result = pull_sdl_event_from_queue(&result, z_ucs_input);
    if ( (wait_result != -1) || (poll_only == true) ) {
      if (wait_result == -1) {
        result = EVENT_WAS_NOTHING;
      }
      TRACE_LOG("poll's wait_result: %d.\n", wait_result);
      break;
    }
    SDL_Delay(10);
  }

  if (timeout_millis > 0) {
    SDL_SemWait(timeout_semaphore);
    if (timeout_timer_exists == true) {
      SDL_RemoveTimer(timeout_timer);
      timeout_timer_exists = false;
    }
    SDL_SemPost(timeout_semaphore);
  }

  TRACE_LOG("Returning from get_next_event.\n");

  return result;
}


void redraw_screen_from_scratch() {
}


void copy_area(int dsty, int dstx, int srcy, int srcx, int height, int width) {
  int y;

  TRACE_LOG("copy-area: %d, %d to %d, %d: %d x %d.\n",
      srcx, srcy, dstx, dsty, width, height);

  if (srcy > dsty) {
    Uint32 *srcp = (Uint32 *)Surf_Display->pixels
      + srcy*Surf_Display->pitch/4 + srcx;
    Uint32 *dstp = (Uint32 *)Surf_Display->pixels
      + dsty*Surf_Display->pitch/4 + dstx;

    for (y=0; y<height; y++) {
      memcpy(dstp, srcp, width*4);
      srcp += Surf_Display->pitch/4;
      dstp += Surf_Display->pitch/4;
    }
  }
  else {
    Uint32 *srcp = (Uint32 *)Surf_Display->pixels
      + (srcy+(height-1))*Surf_Display->pitch/4 + srcx;
    Uint32 *dstp = (Uint32 *)Surf_Display->pixels
      + (dsty+(height-1))*Surf_Display->pitch/4 + dstx;

    for (y=0; y<height; y++) {
      memcpy(dstp, srcp, width*4);
      srcp -= Surf_Display->pitch/4;
      dstp -= Surf_Display->pitch/4;
    }
  }
}


void fill_area(int startx, int starty, int xsize, int ysize,
    uint8_t r, uint8_t g, uint8_t b) {
  int y, x;
  Uint32 sdl_colour;
  Uint32 *srcp;

  TRACE_LOG("Filling area %d,%d / %d,%d with %d,%d,%d\n",
      startx, starty, xsize, ysize, r, g, b);

  sdl_colour= SDL_MapRGB(Surf_Display->format, r, g, b);

  for (y=0; y<ysize; y++) {
    srcp = (Uint32 *)Surf_Display->pixels
      + (starty+y)*Surf_Display->pitch/4 + startx;
    for (x=0; x<xsize; x++) {
      *srcp = sdl_colour;
      srcp++;
    }
  }
}


static void set_cursor_visibility(bool UNUSED(visible)) {
}


static z_colour get_default_foreground_colour() {
  return Z_COLOUR_WHITE;
}


static z_colour get_default_background_colour() {
  return Z_COLOUR_BLACK;
}


static int console_output(z_ucs *output) {
  while (*output != 0) {
    zucs_string_to_utf8_string(
        output_char_buf,
        &output,
        SDL_OUTPUT_CHAR_BUF_SIZE);

    TRACE_LOG("Console output: %s\n", output_char_buf);
    fputs(output_char_buf, stdout);
  }
  fflush(stdout);

  return 0;
}


static int interpreter_thread_function(void *UNUSED(ptr)) {

  fizmo_start(
      story_stream,
      blorb_stream,
      savegame_to_restore);

  sdl_event_evluation_should_stop = true;

  return 0;
}


void do_update_screen() {
  TRACE_LOG("locking sdl_backup_surface_mutex...\n");
  SDL_LockMutex(sdl_backup_surface_mutex);
  TRACE_LOG("sdl_backup_surface_mutex locked\n");

  TRACE_LOG("Main thread updating screen.\n");
  SDL_BlitSurface(Surf_Display, NULL, Surf_Backup, NULL);
  SDL_UpdateTexture(
      sdlTexture,
      NULL,
      Surf_Display->pixels,
      Surf_Display->pitch);
  SDL_RenderClear(sdl_renderer);
  SDL_RenderCopy(sdl_renderer, sdlTexture, NULL, NULL);
  SDL_RenderPresent(sdl_renderer);

  SDL_UnlockMutex(sdl_backup_surface_mutex);
}


void preprocess_resize(int new_x_size, int new_y_size,
    bool wait_for_interpreter_processing) {

  SDL_LockMutex(resize_event_pending_mutex);

  resize_event_new_x_size
    = new_x_size < MINIMUM_X_WINDOW_SIZE
    ? MINIMUM_X_WINDOW_SIZE
    : new_x_size;

  resize_event_new_y_size
    = new_y_size < MINIMUM_Y_WINDOW_SIZE
    ? MINIMUM_Y_WINDOW_SIZE
    : new_y_size;

  resize_event_pending = true;
  wait_for_interpreter_when_resizing = wait_for_interpreter_processing;

  if (wait_for_interpreter_processing) {
    SDL_LockMutex(interpreter_finished_processing_winch_mutex);
  }

  SDL_UnlockMutex(resize_event_pending_mutex);

  if (wait_for_interpreter_processing) {
    // Wait for the main thread to process the resize event. We have to do
    // this since polling the event queue in SDL's Mac OS X implementation
    // blocks the SDL input queue we would never have the chance to repaint
    // the screen in a regular way.

    TRACE_LOG("Waiting for interpreter_finished_processing_winch_cond ...\n");
    interpreter_finished_processing_winch = false;
    while (interpreter_finished_processing_winch == false) {
      SDL_CondWait(
          interpreter_finished_processing_winch_cond,
          interpreter_finished_processing_winch_mutex);
    }

    TRACE_LOG("Found interpreter_finished_processing_winch_cond.\n");
    do_update_screen();

    SDL_UnlockMutex(interpreter_finished_processing_winch_mutex);
  }
}


int sdl_event_filter(void * userdata, SDL_Event *event) {

  if ( (event->type == SDL_WINDOWEVENT)
      && (event->window.event == SDL_WINDOWEVENT_RESIZED) ) {
    TRACE_LOG("resize found in filter function.\n");
    preprocess_resize(event->window.data1, event->window.data2, true);
    return 0;
  }
  else {
    return 1;
  }
}


static struct z_screen_pixel_interface sdl2_interface = {
  &draw_rgb_pixel,
  &is_input_timeout_available,
  &get_next_event,
  &get_interface_name,
  &is_colour_available,
  &parse_config_parameter,
  &get_config_value,
  &get_config_option_names,
  &link_interface_to_story,
  &reset_interface,
  &sdl2_close_interface,
  &output_interface_info,
  &get_screen_width_in_pixels,
  &get_screen_height_in_pixels,
  &get_device_to_pixel_ratio,
  &update_screen,
  &redraw_screen_from_scratch,
  &copy_area,
  &fill_area,
  &set_cursor_visibility,
  &get_default_foreground_colour,
  &get_default_background_colour,
  &console_output
};


int main(int argc, char *argv[]) {
  int argi = 1;
  int story_filename_parameter_number = -1;
  int blorb_filename_parameter_number = -1;
  char *input_file;
  z_colour new_color;
  int int_value, width, height;
  double hidpi_x_scale, hidpi_y_scale;
  int wait_result;
  SDL_Event Event;
  char *ptr;
  z_ucs z_ucs_input;
  const Uint8 *state;

#ifdef ENABLE_TRACING
  turn_on_trace();
#endif // ENABLE_TRACING

#ifdef _WIN32
  #ifdef _WIN64
  #endif
#elif __APPLE__
  #ifdef TARGET_OS_MAC
  resize_via_event_filter = true;
  #endif
#elif __linux__
#elif __unix__ // all unices not caught above
#elif defined(_POSIX_VERSION)
#else
#endif

  fizmo_register_screen_pixel_interface(&sdl2_interface);

  // Parsing must occur after "fizmo_register_screen_pixel_interface" so
  // that fizmo knows where to forward "parse_config_parameter" parameters
  // to.
#ifndef DISABLE_CONFIGFILES
  parse_fizmo_config_files();
#endif // DISABLE_CONFIGFILES


  while (argi < argc) {
    if ((strcmp(argv[argi], "-l") == 0)
        || (strcmp(argv[argi], "--set-locale") == 0)) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      if (set_current_locale_name(argv[argi]) != 0) {
        streams_latin1_output("\n");

        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_INVALID_CONFIGURATION_VALUE_P0S_FOR_P1S,
            argv[argi],
            "locale");

        streams_latin1_output("\n");

        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      set_configuration_value("dont-set-locale-from-config", "true");
      argi++;
    }
    else if ((strcmp(argv[argi], "-pr") == 0)
        || (strcmp(argv[argi], "--predictable") == 0)) {
      set_configuration_value("random-mode", "predictable");
      argi += 1;
    }
    else if ((strcmp(argv[argi], "-ra") == 0)
        || (strcmp(argv[argi], "--random") == 0)) {
      set_configuration_value("random-mode", "random");
      argi += 1;
    }
    else if ((strcmp(argv[argi], "-st") == 0)
        || (strcmp(argv[argi], "--start-transcript") == 0)) {
      set_configuration_value("start-script-when-story-starts", "true");
      argi += 1;
    }
    else if ((strcmp(argv[argi], "-rc") == 0)
        || (strcmp(argv[argi], "--start-recording-commands") == 0)) {
      set_configuration_value(
          "start-command-recording-when-story-starts", "true");
      argi += 1;
    }
    else if ((strcmp(argv[argi], "-fi") == 0)
        || (strcmp(argv[argi], "--start-file-input") == 0)) {
      set_configuration_value(
          "start-file-input-when-story-starts", "true");
      argi += 1;
    }
    else if ((strcmp(argv[argi], "-if") == 0)
        || (strcmp(argv[argi], "--input-filename") == 0)) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }
      set_configuration_value(
          "input-command-filename", argv[argi]);
      argi += 1;
    }
    else if ((strcmp(argv[argi], "-rf") == 0)
        || (strcmp(argv[argi], "--record-filename") == 0)) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }
      set_configuration_value(
          "record-command-filename", argv[argi]);
      argi += 1;
    }
    else if ((strcmp(argv[argi], "-tf") == 0)
        || (strcmp(argv[argi], "--transcript-filename") == 0)) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }
      set_configuration_value(
          "transcript-filename", argv[argi]);
      argi += 1;
    }
    else if (
        (strcmp(argv[argi], "-b") == 0)
        || (strcmp(argv[argi], "--background-color") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      if ((new_color = color_name_to_z_colour(argv[argi])) == -1) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      screen_default_background_color = new_color;
      set_configuration_value("background-color", argv[argi]);
      argi++;
    }
    else if (
        (strcmp(argv[argi], "-f") == 0)
        || (strcmp(argv[argi], "--foreground-color") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      if ((new_color = color_name_to_z_colour(argv[argi])) == -1) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      screen_default_foreground_color = new_color;
      set_configuration_value("foreground-color", argv[argi]);
      argi++;
    }
    else if (
        (strcmp(argv[argi], "-cc") == 0)
        || (strcmp(argv[argi], "--cursor-color") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      if ((new_color = color_name_to_z_colour(argv[argi])) == -1) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      set_configuration_value("cursor-color", argv[argi]);
      argi++;
    }
    else if ( (strcmp(argv[argi], "-fs") == 0)
        || (strcmp(argv[argi], "--font-size") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      int_value = atoi(argv[argi]);

      if (int_value < 4) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_INVALID_CONFIGURATION_VALUE_P0S_FOR_P1S,
            argv[argi],
            argv[argi - 1]);

        streams_latin1_output("\n");

        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      set_configuration_value("font-size", argv[argi]);
      argi += 1;
    }
    else if ( (strcmp(argv[argi], "-ww") == 0)
        || (strcmp(argv[argi], "--window-width") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      int_value = atoi(argv[argi]);

      if (int_value > MINIMUM_X_WINDOW_SIZE) {
        sdl2_interface_screen_width_in_pixels = int_value;
      }
      argi += 1;
    }
    else if ( (strcmp(argv[argi], "-wh") == 0)
        || (strcmp(argv[argi], "--window-height") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      int_value = atoi(argv[argi]);

      if (int_value > MINIMUM_Y_WINDOW_SIZE) {
        sdl2_interface_screen_height_in_pixels = int_value;
      }
      argi += 1;
    }
    else if (
        (strcmp(argv[argi], "-um") == 0)
        || (strcmp(argv[argi], "--umem") == 0) ) {
      set_configuration_value("quetzal-umem", "true");
      argi ++;
    }
    else if (
        (strcmp(argv[argi], "-dh") == 0)
        || (strcmp(argv[argi], "--disable-hyphenation") == 0) ) {
      set_configuration_value("disable-hyphenation", "true");
      argi ++;
    }
    else if ( (strcmp(argv[argi], "-ds") == 0)
        || (strcmp(argv[argi], "--disable-sound") == 0) ) {
      set_configuration_value("disable-sound", "true");
      argi += 1;
    }
    else if (
        (strcmp(argv[argi], "-t") == 0)
        || (strcmp(argv[argi], "--set-tandy-flag") == 0) ) {
      set_configuration_value("set-tandy-flag", "true");
      argi += 1;
    }
    else if ( (strcmp(argv[argi], "-lm") == 0)
        || (strcmp(argv[argi], "-rm") == 0)
        || (strcmp(argv[argi], "--left-margin") == 0)
        || (strcmp(argv[argi], "--right-margin") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      int_value = atoi(argv[argi]);

      if ( ( (int_value == 0) && (strcmp(argv[argi], "0") != 0) )
          || (int_value < 0) ) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_INVALID_CONFIGURATION_VALUE_P0S_FOR_P1S,
            argv[argi],
            argv[argi - 1]);

        streams_latin1_output("\n");

        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      if ( (strcmp(argv[argi - 1], "-lm") == 0)
          || (strcmp(argv[argi - 1], "--left-margin") == 0) ) {
        set_custom_left_pixel_margin(int_value);
      }
      else {
        set_custom_right_pixel_margin(int_value);
      }

      argi += 1;
    }
    else if ( (strcmp(argv[argi], "-h") == 0)
        || (strcmp(argv[argi], "--help") == 0) ) {
      print_startup_syntax();
      exit(0);
    }
    else if ( (strcmp(argv[argi], "-sy") == 0)
        || (strcmp(argv[argi], "--sync-transcript") == 0) ) {
      set_configuration_value("sync-transcript", "true");
      argi += 1;
    }
    else if (story_filename_parameter_number == -1) {
      story_filename_parameter_number = argi;
      argi++;
    }
    else if (blorb_filename_parameter_number == -1) {
      blorb_filename_parameter_number = argi;
      argi++;
    }
    else {
      // Unknown parameter:
      print_startup_syntax();
      exit(EXIT_FAILURE);
    }
  }

  if (story_filename_parameter_number == -1) {
    // User provided no story file name.
    print_startup_syntax();
  }
  else {
    // The user has given some filename or description name on the command line.
    input_file = argv[story_filename_parameter_number];

    // Check if parameter is a valid filename.
    story_stream = fsi->openfile(
        input_file, FILETYPE_DATA, FILEACCESS_READ);

    if (story_stream == NULL) {
      i18n_translate_and_exit(
          fizmo_sdl2_module_name,
          i18n_sdl2_COULD_NOT_OPEN_OR_FIND_P0S,
          -0x2016,
          input_file);
      exit(EXIT_FAILURE);
    }
    else {
      if (SDL_Init(SDL_INIT_EVERYTHING) < 0)
        i18n_translate_and_exit(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            -1,
            "SDL_Init");

      winchCollectionEventType = SDL_RegisterEvents(1);

      if (resize_via_event_filter == true) {
        SDL_SetEventFilter(sdl_event_filter, NULL);
      }

      SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

      //SDL_EnableKeyRepeat(200, 20);

      atexit(SDL_Quit);

      sdl_event_queue_mutex = SDL_CreateMutex();
      sdl_main_thread_working_mutex = SDL_CreateMutex();
      sdl_backup_surface_mutex = SDL_CreateMutex();
      resize_event_pending_mutex = SDL_CreateMutex();
      interpreter_finished_processing_winch_mutex = SDL_CreateMutex();

      sdl_main_thread_working_cond = SDL_CreateCond();
      interpreter_finished_processing_winch_cond = SDL_CreateCond();

      if ((sdl_window = SDL_CreateWindow("fizmo-sdl2",
          SDL_WINDOWPOS_UNDEFINED,
          SDL_WINDOWPOS_UNDEFINED,
          sdl2_interface_screen_width_in_pixels,
          sdl2_interface_screen_height_in_pixels,
          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)) == NULL) {
        i18n_translate_and_exit(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            -1,
            "SDL_SetVideoMode");
      }

      SDL_GL_GetDrawableSize(sdl_window, &width, &height);
      hidpi_x_scale = width / sdl2_interface_screen_width_in_pixels;
      hidpi_y_scale = height / sdl2_interface_screen_height_in_pixels;

      if (hidpi_x_scale == hidpi_y_scale) {
        sdl2_device_to_pixel_ratio = hidpi_x_scale;
        sdl2_interface_screen_width_in_pixels *= sdl2_device_to_pixel_ratio;
        sdl2_interface_screen_height_in_pixels *= sdl2_device_to_pixel_ratio;
      }

      if ((sdl_renderer = SDL_CreateRenderer(sdl_window, -1, 0)) == NULL) {
        i18n_translate_and_exit(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            -1,
            "SDL_CreateRenderer");
      }

      if ((Surf_Display = SDL_CreateRGBSurface(
              0,
              sdl2_interface_screen_width_in_pixels,
              sdl2_interface_screen_height_in_pixels,
              32,
              0x00FF0000,
              0x0000FF00,
              0x000000FF,
              0xFF000000)) == NULL) {
        i18n_translate_and_exit(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            -1,
            "SDL_GetWindowSurface");
      }

      if ((Surf_Backup = SDL_CreateRGBSurface(
              0,
              sdl2_interface_screen_width_in_pixels,
              sdl2_interface_screen_height_in_pixels,
              32,
              0x00FF0000,
              0x0000FF00,
              0x000000FF,
              0xFF000000)) == NULL) {
        i18n_translate_and_exit(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            -1,
            "SDL_GetWindowSurface");
      }

      if ((sdlTexture = SDL_CreateTexture(sdl_renderer,
          SDL_PIXELFORMAT_ARGB8888,
          SDL_TEXTUREACCESS_STREAMING,
          sdl2_interface_screen_width_in_pixels,
          sdl2_interface_screen_height_in_pixels)) == NULL) {
        i18n_translate_and_exit(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            -1,
            "SDL_CreateTexture");
      }

      timeout_semaphore = SDL_CreateSemaphore(1);

#ifdef SOUND_INTERFACE_STRUCT_NAME
      fizmo_register_sound_interface(&SOUND_INTERFACE_STRUCT_NAME);
#endif // SOUND_INTERFACE_STRUCT_NAME

      sdl_interpreter_thread = SDL_CreateThread(
          interpreter_thread_function, "InterpreterThread", NULL);

      // --- begin event evaluation
      do {

        if (main_thread_work_complete == false) {
          TRACE_LOG("Found some work to do.\n");
          SDL_LockMutex(sdl_main_thread_working_mutex);

          if (main_thread_should_update_screen == true) {
            do_update_screen();
            main_thread_should_update_screen = false;
          }

          if (main_thread_should_expose_screen == true) {
            SDL_LockMutex(sdl_backup_surface_mutex);

            SDL_UpdateTexture(
                sdlTexture,
                NULL,
                Surf_Backup->pixels,
                Surf_Backup->pitch);

            SDL_RenderClear(sdl_renderer);
            SDL_RenderCopy(sdl_renderer, sdlTexture, NULL, NULL);
            SDL_RenderPresent(sdl_renderer);

            SDL_UnlockMutex(sdl_backup_surface_mutex);
          }

          main_thread_work_complete = true;
          TRACE_LOG("Main thread work complete.\n");
          SDL_CondSignal(sdl_main_thread_working_cond);
          SDL_UnlockMutex(sdl_main_thread_working_mutex);
          TRACE_LOG("Continuing event loop.\n");
        }

        TRACE_LOG("Starting poll...\n");
        wait_result = SDL_PollEvent(&Event);
        TRACE_LOG("poll's wait_result: %d.\n", wait_result);
        if (wait_result == 0) {
          SDL_Delay(10);
        }
        else {
          /*
             if (Event.type == SDL_QUIT) {
             TRACE_LOG("quit\n");
             running = false;
             }
             */

          if (Event.type == SDL_TEXTINPUT) {
            ptr = Event.text.text;
            z_ucs_input = utf8_char_to_zucs_char(&ptr);
            TRACE_LOG("z_ucs_input: %d.\n", z_ucs_input);
            push_sdl_event_to_queue(EVENT_WAS_INPUT, z_ucs_input);
          }
          else if (Event.type == SDL_KEYDOWN) {
            TRACE_LOG("Event was keydown.\n");
            // https://wiki.libsdl.org/SDL_Scancode

            state = SDL_GetKeyboardState(NULL);
            if ( (state[SDL_SCANCODE_LCTRL])|| (state[SDL_SCANCODE_RCTRL]) ) {
              TRACE_LOG("ctrl\n");
              if (state[SDL_SCANCODE_L]) {
                TRACE_LOG("ctrl-l.\n");
                push_sdl_event_to_queue(EVENT_WAS_CODE_CTRL_L, 0);
              }
              else if (state[SDL_SCANCODE_R]) {
                TRACE_LOG("ctrl-r.\n");
                push_sdl_event_to_queue(EVENT_WAS_CODE_CTRL_R, 0);
              }
              else if (state[SDL_SCANCODE_A]) {
                push_sdl_event_to_queue(EVENT_WAS_CODE_CTRL_A, 0);
              }
              else if (state[SDL_SCANCODE_E]) {
                push_sdl_event_to_queue(EVENT_WAS_CODE_CTRL_E, 0);
              }
            }
            else if (Event.key.keysym.sym == SDLK_LEFT) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_CURSOR_LEFT, 0);
            }
            else if (Event.key.keysym.sym == SDLK_RIGHT) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_CURSOR_RIGHT, 0);
            }
            else if (Event.key.keysym.sym == SDLK_DOWN) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_CURSOR_DOWN, 0);
            }
            else if (Event.key.keysym.sym == SDLK_UP) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_CURSOR_UP, 0);
            }
            else if (Event.key.keysym.sym == SDLK_BACKSPACE) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_BACKSPACE, 0);
            }
            else if (Event.key.keysym.sym == SDLK_DELETE) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_DELETE, 0);
            }
            else if (Event.key.keysym.sym == SDLK_RETURN) {
              push_sdl_event_to_queue(EVENT_WAS_INPUT, Z_UCS_NEWLINE);
            }
            else if (Event.key.keysym.sym == SDLK_PAGEDOWN) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_PAGE_DOWN, 0);
            }
            else if (Event.key.keysym.sym == SDLK_PAGEUP) {
              push_sdl_event_to_queue(EVENT_WAS_CODE_PAGE_UP, 0);
            }
          }
          else if (Event.type == SDL_WINDOWEVENT) {
            TRACE_LOG("Found SDL_WINDOWEVENT: %d.\n", Event.window.event);

            if (Event.window.event == SDL_WINDOWEVENT_EXPOSED) {
              TRACE_LOG("Found SDL_WINDOWEVENT_EXPOSED.\n");
              do_expose = true;
            }
            else if ( (resize_via_event_filter == false)
                && (Event.window.event == SDL_WINDOWEVENT_RESIZED) ) {
              TRACE_LOG("Found SDL_WINDOWEVENT_RESIZED.\n");

              preprocess_resize(
                  Event.window.data1,
                  Event.window.data2,
                  false);
            }
          }
        }
      }
      while (sdl_event_evluation_should_stop == false);
      // --- end event evaluation

      SDL_DestroySemaphore(timeout_semaphore);

      SDL_DestroyTexture(sdlTexture);
      SDL_FreeSurface(Surf_Backup);
      SDL_FreeSurface(Surf_Display);

      SDL_Quit();
    }
  }

#ifdef ENABLE_TRACING
  TRACE_LOG("Turning off trace.\n\n");
  turn_off_trace();
#endif // ENABLE_TRACING

  return 0;
}

