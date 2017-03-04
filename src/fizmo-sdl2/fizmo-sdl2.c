
/* fizmo-sdl2.c
 *
 * This file is part of fizmo.
 *
 * Copyright (c) 2011-2017 Christoph Ender.
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
 *
 *
 *
 * OPERATION MODES: EVENT QUEUE AND EVENT FILTER
 *
 * Due to different implementations of SDL, this frontend has two modes of
 * operation.
 *
 * When the user drags the mouse in the Mac OS X implementation, the
 * SDL_PollEvent call blocks until the user releases the mouse button. That
 * means that as longs as the user is resizing the window it stays completely
 * black. This does not only mean the operation doesn't look very nice, it
 * also means the user can only guess how the final window layout will look
 * like once he's releasing the mouse button. The result is a lot of trying
 * and guessing.
 *
 * To work around this, the resize event is intercepted in the event filter.
 * Using this method however has the disadvantage that in the linux
 * implementation there are so many resize events that the resizing and
 * redraw is lagging quite much behind the actual mouse pointer. Furthermore,
 * if the mouse button is released before the window has reached the pointer
 * position, all the resize-events are coming in again -- the window is
 * constantly resized from the old size and position from the new one in an
 * endless loop. It also turned out that evaluating the events with a little
 * bit of delay, even one millisecond, thus "collecting" multiple events, make
 * the resulting window flicker all the time while resizing. For linux, using
 * the regulart event queue instead of the event filter eliminates almost all
 * of these problems, just the handling of the expose events, which never
 * appear to occur in Mac OS X, makes the window contents "shake" a bit.
 *
 * All this leads to the implementation of both modes. By default the standard
 * event queue is used. The OS is detected via #ifdefs in the main() method
 * and the "resize_via_event_filter" flag is set accordingly.
 *
 * At the time of writing, the "fizmo-sdl2"-interface has not been tested
 * with any windows implemenation.
 *
 *
 *
 * THE EVENT LOOP
 *
 * It appears that only the main thread is safe to use for any video-related
 * or event-processing activity. To implement this behavior, the interpreter
 * is working in a seperate thread while the main thread is processing events.
 * Once an event has been received from SDL, it's stored in the fizmo-internal
 * "sdl_event_queue". Once the interpreter invokes the "get_next_event"
 * function, the next event is pulled from this queue and returned the the
 * interpreter thread.
 *
 * Video output is initially written to the "Surf_Display" surface. When the
 * current frame is supposed to be displayed on-screen, the main thread is
 * notified via the "main_thread_work_complete" flag and a another,
 * action-specific flag is set. The interpreter thread waits for the main
 * thread to complete the activity and then resumes working.
 *
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

#define FIZMO_SDL_VERSION "0.8.4"

#define SDL_OUTPUT_CHAR_BUF_SIZE 80
#define MINIMUM_X_WINDOW_SIZE 200
#define MINIMUM_Y_WINDOW_SIZE 100

static char* interface_name = "sdl2";

static char *config_option_names[] = {
  "process-sdl2-events", NULL };
static char* sdl2_event_processing_queue_option_name = "queue";
static char* sdl2_event_processing_filter_option_name = "filter";

static SDL_Window *sdl_window = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Surface* Surf_Display = NULL;
static SDL_Surface* Surf_Backup = NULL;
static SDL_Texture *sdlTexture = NULL;
static z_colour screen_default_foreground_color = Z_COLOUR_BLACK;
static z_colour screen_default_background_color = Z_COLOUR_WHITE;
static int unscaled_sdl2_interface_screen_height_in_pixels = 800;
static int unscaled_sdl2_interface_screen_width_in_pixels = 600;
static int scaled_sdl2_interface_screen_height_in_pixels = 800;
static int scaled_sdl2_interface_screen_width_in_pixels = 600;
static double sdl2_device_to_pixel_ratio = 1;
static SDL_TimerID timeout_timer;
//static SDL_TimerID collection_timer;
static bool timeout_timer_exists;
//static bool collection_timer_exists;
static SDL_sem *timeout_semaphore;
//static SDL_sem *collection_semaphore;
static char output_char_buf[SDL_OUTPUT_CHAR_BUF_SIZE];

static bool resize_via_event_filter = false;

struct sdl_queued_event_struct {
  int event_type;
  z_ucs z_ucs_input;
};
typedef struct sdl_queued_event_struct sdl_queued_event;

static sdl_queued_event *sdl_event_queue = NULL;
static size_t sdl_event_queue_size = 0;
static size_t sdl_event_queue_index = 0; // index of next stored event
static size_t sdl_event_queue_size_increment = 1024;
static SDL_mutex *sdl_event_queue_mutex = NULL;

static SDL_Thread *sdl_interpreter_thread = NULL;
static bool sdl_event_evluation_should_stop = false;

static SDL_mutex *sdl_main_thread_working_mutex;
static SDL_mutex *sdl_backup_surface_mutex;
static SDL_cond *update_screen_wait_cond;
static SDL_cond *sdl_main_thread_working_cond;
static bool main_thread_work_complete = true;
static bool main_thread_should_update_screen = false;
//static bool main_thread_should_expose_screen = false;
static bool main_thread_should_set_title = false;

static z_file *story_stream = NULL;
static z_file *blorb_stream = NULL;
static z_file *savegame_to_restore= NULL;

// This mutex is used to manage access to the "resize_event_pending"
// variable:
static SDL_mutex *resize_event_pending_mutex = NULL;
static bool resize_event_pending = false;


// 2: Interpreter thread received resize notifiction and is processing it:
static bool interpreter_is_processing_winch = false;

// 3: Interpreter has finished processing
//static SDL_mutex *interpreter_finished_processing_winch_mutex = NULL;
// Once this cond is set, the main thread will refresh the screen.
static SDL_cond *interpreter_finished_processing_winch_cond = NULL;
static bool interpreter_finished_processing_winch = false;

static int resize_event_new_x_size;
static int resize_event_new_y_size;

//static bool do_expose = false;
static int frontispiece_resource_number;
static char* story_title;

//static SDL_mutex *filter_mutex;
static bool filter_is_waiting_for_interpreter_screen_update = false;

static bool interpreter_history_was_remeasured = false;

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

  if (available_locales == NULL) {
    streams_latin1_output("Could not find any installed locales.\n");
    exit(EXIT_FAILURE);
  }

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

  streams_latin1_output( " -ps, --process-sdl2-events: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_PROCESS_SDL2_EVENTS);
  streams_latin1_output("\n");

  streams_latin1_output( " -h,  --help: ");
  i18n_translate(
      fizmo_sdl2_module_name,
      i18n_sdl2_SHOW_HELP_MESSAGE_AND_EXIT);
  streams_latin1_output("\n");

  streams_latin1_output("\n");
}


static int parse_config_parameter(char *key, char *value) {
  long long_value;
  char *endptr;

  if (strcasecmp(key, "process-sdl2-events") == 0) {
    if (strcasecmp(value, sdl2_event_processing_queue_option_name) == 0) {
      resize_via_event_filter = false;
      return 0;
    }
    else if (strcasecmp(value, sdl2_event_processing_filter_option_name) == 0) {
      resize_via_event_filter = true;
      return 0;
    }
    else {
      return -1;
    }
  }
  else if ( (strcasecmp(key, "window-width") == 0)
      || (strcasecmp(key, "window-height") == 0) ) {
    if ( (value == NULL) || (strlen(value) == 0) )
      return -1;
    long_value = strtol(value, &endptr, 10);
    free(value);
    if (*endptr != 0)
      return -1;
    if (strcasecmp(key, "window-width") == 0)
      unscaled_sdl2_interface_screen_width_in_pixels = long_value;
    else
      unscaled_sdl2_interface_screen_height_in_pixels = long_value;
    return 0;
  }
  else {
    return -2;
  }
}


static char *get_config_value(char *key) {
  if (strcasecmp(key, "process-sdl2-events") == 0) {
    return resize_via_event_filter == true
      ?  sdl2_event_processing_filter_option_name
      :  sdl2_event_processing_queue_option_name;
  }
  else {
    return NULL;
  }
}


static char **get_config_option_names() {
  return config_option_names;
}


static void set_title_and_icon() {
  z_image *frontispiece;
  z_image *window_icon_zimage;
  uint32_t *icon_pixels;
  int x, y, pixel_left_shift;
  uint8_t red, green, blue;
  uint8_t *image_data;
  SDL_Surface *icon_surface;

  SDL_SetWindowTitle(sdl_window, story_title);

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


static void link_interface_to_story(struct z_story *story) {
  int resource_number;

  story_title = story->title;

  resource_number
    = active_blorb_interface->get_frontispiece_resource_number(
        active_z_story->blorb_map);

  frontispiece_resource_number
    = resource_number >= 0 ? resource_number : -1;

  TRACE_LOG("Waiting for sdl_main_thread_working_mutex.\n");
  SDL_LockMutex(sdl_main_thread_working_mutex);
  TRACE_LOG("Locked sdl_main_thread_working_mutex.\n");
  main_thread_should_set_title = true;
  main_thread_work_complete = false;
  while (main_thread_work_complete == false) {
    TRACE_LOG("Waiting for sdl_main_thread_working_cond ...\n");
    SDL_CondWait(sdl_main_thread_working_cond, sdl_main_thread_working_mutex);
  }
  TRACE_LOG("Found sdl_main_thread_working_cond.\n");
  SDL_UnlockMutex(sdl_main_thread_working_mutex);
}


static void reset_interface() {
}


static int sdl2_close_interface(z_ucs *error_message) {
  char *output;

  if (error_message != NULL) {
    output = dup_zucs_string_to_utf8_string(error_message);
    puts(output);
    free(output);
  }
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
  return scaled_sdl2_interface_screen_width_in_pixels;
}


static int get_screen_height_in_pixels() {
  return scaled_sdl2_interface_screen_height_in_pixels;
}


static double get_device_to_pixel_ratio() {
  return sdl2_device_to_pixel_ratio;
}


static void process_resize2() {
  SDL_LockMutex(sdl_backup_surface_mutex);

  TRACE_LOG("process_resize2: %d / %d\n",
      unscaled_sdl2_interface_screen_width_in_pixels,
      unscaled_sdl2_interface_screen_height_in_pixels);

  SDL_SetWindowSize(sdl_window,
      unscaled_sdl2_interface_screen_width_in_pixels,
      unscaled_sdl2_interface_screen_height_in_pixels);

  SDL_FreeSurface(Surf_Backup);
  if ((Surf_Backup = SDL_CreateRGBSurface(
          0,
          scaled_sdl2_interface_screen_width_in_pixels,
          scaled_sdl2_interface_screen_height_in_pixels,
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
          scaled_sdl2_interface_screen_width_in_pixels,
          scaled_sdl2_interface_screen_height_in_pixels)) == NULL) {
    i18n_translate_and_exit(
        fizmo_sdl2_module_name,
        i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
        -1,
        "SDL_CreateTexture");
  }

  SDL_UnlockMutex(sdl_backup_surface_mutex);
}


void update_screen() {
  TRACE_LOG("Doing update_screen().\n");

  // This thread is executed in the interpreter's context. This means
  // we have to notify the main sdl thread that we want the screen
  // updated.
  //
  // There are two ways this can be handeled: In the unfiltered processing
  // method this is always done by the main event loop. In case we're
  // using the filtered approach, it is also possible that the main
  // thread is hanging in the "sdl_event_filter" method, outside the
  // regular event loop. In this case, the screen update has to be done
  // by ths method, since the main event loop is blocked in this kind
  // of SDL implementation (which generally means Mac OS X).

  TRACE_LOG("Waiting for sdl_main_thread_working_mutex.\n");
  SDL_LockMutex(sdl_main_thread_working_mutex);
  TRACE_LOG("Locked sdl_main_thread_working_mutex.\n");

  TRACE_LOG("filter_is_waiting_for_interpreter_screen_update: %d\n",
      filter_is_waiting_for_interpreter_screen_update);

  if (filter_is_waiting_for_interpreter_screen_update == true) {
    // In case the resize thread is already waiting for an update, we
    // can notify it right away, wait till it is done updating the screen,
    // which we know is done then the resize thread releases the
    // "interpreter_finished_processing_winch_mutex" mutex, and are done.
    interpreter_finished_processing_winch = true;
    SDL_CondSignal(interpreter_finished_processing_winch_cond);
    //SDL_UnlockMutex(filter_mutex);
  }
  else {
    // In case the resize thread is not -- or not yet -- waiting for an
    // update, we'll set the flags in a way that tells the main thread
    // that we need an update.
    //SDL_UnlockMutex(interpreter_finished_processing_winch_mutex);

    main_thread_should_update_screen = true;
    main_thread_work_complete = false;
    while ( (main_thread_should_update_screen == false)
        && (filter_is_waiting_for_interpreter_screen_update == false) ) {
      TRACE_LOG("Waiting for update_screen_wait_cond ...\n");
      SDL_CondWait(update_screen_wait_cond, sdl_main_thread_working_mutex);
      TRACE_LOG("Found for update_screen_wait_cond.\n");
    }

    if (filter_is_waiting_for_interpreter_screen_update == true) {
      //main_thread_should_update_screen = false;
      SDL_CondSignal(interpreter_finished_processing_winch_cond);
      interpreter_is_processing_winch = false;
    }

    // No matter whether the main event loop or the filter thread
    // have sent a signal to "update_screen_wait_cond", we know that
    // the screen is up to date and we can return.

  }

  SDL_UnlockMutex(sdl_main_thread_working_mutex);

  TRACE_LOG("Finished update_screen().\n");





  /*
  SDL_LockMutex(interpreter_finished_processing_winch_mutex);
  TRACE_LOG("resize_thread_is_waiting_for_interpreter_screen_update: %d\n",
      resize_thread_is_waiting_for_interpreter_screen_update);

  if (resize_thread_is_waiting_for_interpreter_screen_update == true) {
    // At this point we're only telling the main thread that we
    // wish to update the screen. it is currently waiting for this
    // due to a resize and will redraw the screen by itself in the
    // fiter function.
    interpreter_finished_processing_winch = true;
    SDL_CondSignal(interpreter_finished_processing_winch_cond);
    SDL_UnlockMutex(interpreter_finished_processing_winch_mutex);
  }
  else {
    SDL_UnlockMutex(interpreter_finished_processing_winch_mutex);
    // This has to be done by the main thread.
    TRACE_LOG("Waiting for sdl_main_thread_working_mutex.\n");
    SDL_LockMutex(sdl_main_thread_working_mutex);
    TRACE_LOG("Locked sdl_main_thread_working_mutex.\n");
    main_thread_should_update_screen = true;
    main_thread_work_complete = false;
    while (main_thread_work_complete == false) {
      TRACE_LOG("Waiting for sdl_main_thread_working_cond ...\n");
      SDL_CondWait(update_screen_wait_cond, sdl_main_thread_working_mutex);
      TRACE_LOG("FOund update_screen_wait_cond\n");
      if (resize_thread_is_waiting_for_interpreter_screen_update == true) {
        SDL_LockMutex(interpreter_finished_processing_winch_mutex);
        interpreter_finished_processing_winch = true;
        SDL_CondSignal(interpreter_finished_processing_winch_cond);
        SDL_UnlockMutex(interpreter_finished_processing_winch_mutex);
        break;
      }
      //SDL_CondWait(sdl_main_thread_working_cond,
      //    sdl_main_thread_working_mutex);
    }
    TRACE_LOG("Found sdl_main_thread_working_cond.\n");
    SDL_UnlockMutex(sdl_main_thread_working_mutex);
  }
  */
}


static void process_resize1() {

  unscaled_sdl2_interface_screen_width_in_pixels = resize_event_new_x_size;
  unscaled_sdl2_interface_screen_height_in_pixels = resize_event_new_y_size;

  scaled_sdl2_interface_screen_width_in_pixels
    = unscaled_sdl2_interface_screen_width_in_pixels
    * sdl2_device_to_pixel_ratio;

  scaled_sdl2_interface_screen_height_in_pixels
    = unscaled_sdl2_interface_screen_height_in_pixels
    * sdl2_device_to_pixel_ratio;

  TRACE_LOG("resize1: unscaled size: %d x %d\n",
      unscaled_sdl2_interface_screen_width_in_pixels,
      unscaled_sdl2_interface_screen_height_in_pixels);

  SDL_FreeSurface(Surf_Display);
  if ((Surf_Display = SDL_CreateRGBSurface(
          0,
          scaled_sdl2_interface_screen_width_in_pixels,
          scaled_sdl2_interface_screen_height_in_pixels,
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
}


static bool does_resize_event_exist() {
  static SDL_Event events[25];
  int nof_events;
  int i;

  SDL_PumpEvents();
  nof_events = SDL_PeepEvents(
      events, 25, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);

  if (nof_events > 0) {
    for (i=0; i<nof_events; i++) {
      if ( (events[i].type == SDL_WINDOWEVENT)
          && (events[i].window.event == SDL_WINDOWEVENT_RESIZED) ) {
        return true;
      }
    }
  }
  return false;
}


// This function is executed in the context of the interpreter thread.
static int pull_sdl_event_from_queue(int *event_type, z_ucs *z_ucs_input) {
  int result;
  bool resize_event_has_to_be_processed = false;
  //bool wait_for_terp = false;

  // Before we actually lookingg at the event queue, we have to process
  // window-resizes seperately (since resizing blocks the event queue in SDL's
  // Mac OS X implementation.

  SDL_LockMutex(resize_event_pending_mutex);
  if (resize_event_pending == true) {
    TRACE_LOG("Gotta resize.\n");
    resize_event_has_to_be_processed = true;
    resize_event_pending = false;
  }
  SDL_UnlockMutex(resize_event_pending_mutex);

  if (resize_event_has_to_be_processed == true) {
    process_resize1();
    *event_type = EVENT_WAS_WINCH;
    interpreter_is_processing_winch = true;
    TRACE_LOG("interpreter_is_processing_winch = true\n");
    *z_ucs_input = 0;
    result = 0;
  }
  else {
    // In case we don't have to process resizing events we check the event
    // queue.

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


static int get_next_event(z_ucs *z_ucs_input, int timeout_millis,
    bool poll_only, bool history_finished_remeasuring) {
  int wait_result, result = -1;

  TRACE_LOG("Invoked get_next_event.\n");

  if (history_finished_remeasuring == true) {
    SDL_LockMutex(sdl_main_thread_working_mutex);
    main_thread_work_complete = false;
    interpreter_history_was_remeasured = true;
    SDL_UnlockMutex(sdl_main_thread_working_mutex);
  }

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


void preprocess_nonfiltered_resize(int new_x_size, int new_y_size) {
  TRACE_LOG("Starting nonfiltered preprocess_resize.\n");
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

  SDL_UnlockMutex(resize_event_pending_mutex);
  TRACE_LOG("Finished pnonfiltered reprocess_resize.\n");
}


int sdl_event_filter(void * userdata, SDL_Event *event) {

  if ( (event->type == SDL_WINDOWEVENT)
      && (event->window.event == SDL_WINDOWEVENT_RESIZED) ) {
    TRACE_LOG("resize found in filter function.\n");

    // Since this function appears to be running in it's own thread
    // and we're updating the screen, we need to get a lock on the
    // main loop's mutex to avoid collisions:
    SDL_LockMutex(sdl_main_thread_working_mutex);

    SDL_LockMutex(resize_event_pending_mutex);

    resize_event_new_x_size
      = event->window.data1 < MINIMUM_X_WINDOW_SIZE
      ? MINIMUM_X_WINDOW_SIZE
      : event->window.data1 ;

    resize_event_new_y_size
      = event->window.data2 < MINIMUM_Y_WINDOW_SIZE
      ? MINIMUM_Y_WINDOW_SIZE
      : event->window.data2;

    resize_event_pending = true;
    SDL_UnlockMutex(resize_event_pending_mutex);

    //SDL_LockMutex(filter_mutex);
    filter_is_waiting_for_interpreter_screen_update = true;
    interpreter_finished_processing_winch = false;

    // In case the interpreter thread is already waiting in the
    // "do_update" function for a screen update, we'll wake it
    // up.
    SDL_CondSignal(update_screen_wait_cond);

    TRACE_LOG("Waiting for interpreter_finished_processing_winch_cond ...\n");
    while (interpreter_finished_processing_winch == false) {
      SDL_CondWait(
          interpreter_finished_processing_winch_cond,
          sdl_main_thread_working_mutex);
    }

    process_resize2();
    do_update_screen();

    filter_is_waiting_for_interpreter_screen_update = false;

    //SDL_UnlockMutex(interpreter_finished_processing_winch_mutex);

    SDL_UnlockMutex(sdl_main_thread_working_mutex);

    TRACE_LOG("Finished processing filetered resize.\n");
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
  int thread_status;
  bool screen_was_updated;

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

      if (int_value >= MINIMUM_X_WINDOW_SIZE) {
        unscaled_sdl2_interface_screen_width_in_pixels = int_value;
      }
      else {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_WINDOW_WIDTH_TOO_NARROW_MINIMUM_IS_P0D,
            MINIMUM_X_WINDOW_SIZE);
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
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

      if (int_value >= MINIMUM_Y_WINDOW_SIZE) {
        unscaled_sdl2_interface_screen_height_in_pixels = int_value;
      }
      else {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_WINDOW_HEIGHT_TOO_SMALL_MINIMUM_IS_P0D,
            MINIMUM_Y_WINDOW_SIZE);
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
      }
      argi += 1;
    }
    else if ( (strcmp(argv[argi], "-ps") == 0)
        || (strcmp(argv[argi], "--process-sdl2-events") == 0) ) {
      if (++argi == argc) {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }

      if ( (strcasecmp(argv[argi],
              sdl2_event_processing_queue_option_name) == 0)
          || (strcasecmp(argv[argi],
              sdl2_event_processing_filter_option_name) == 0) ) {
        set_configuration_value(
            "process-sdl2-events", argv[argi]);
      }
      else {
        print_startup_syntax();
        exit(EXIT_FAILURE);
      }
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
      i18n_translate(
          fizmo_sdl2_module_name,
          i18n_sdl2_COULD_NOT_OPEN_OR_FIND_P0S,
          input_file);
      streams_latin1_output("\n");
      exit(EXIT_FAILURE);
    }
    else {
      if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            "SDL_Init");
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
      }

      if (resize_via_event_filter == true) {
        SDL_SetEventFilter(sdl_event_filter, NULL);
      }

      SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

      //SDL_EnableKeyRepeat(200, 20);

      atexit(SDL_Quit);

      sdl_event_queue_mutex = SDL_CreateMutex();
      //filter_mutex = SDL_CreateMutex();
      sdl_main_thread_working_mutex = SDL_CreateMutex();
      sdl_backup_surface_mutex = SDL_CreateMutex();
      resize_event_pending_mutex = SDL_CreateMutex();
      //interpreter_finished_processing_winch_mutex = SDL_CreateMutex();

      sdl_main_thread_working_cond = SDL_CreateCond();
      update_screen_wait_cond = SDL_CreateCond();
      interpreter_finished_processing_winch_cond = SDL_CreateCond();

      if ((sdl_window = SDL_CreateWindow("fizmo-sdl2",
          SDL_WINDOWPOS_UNDEFINED,
          SDL_WINDOWPOS_UNDEFINED,
          unscaled_sdl2_interface_screen_width_in_pixels,
          unscaled_sdl2_interface_screen_height_in_pixels,
          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)) == NULL) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            "SDL_SetVideoMode");
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
      }

      SDL_GL_GetDrawableSize(sdl_window, &width, &height);
      hidpi_x_scale = width / unscaled_sdl2_interface_screen_width_in_pixels;
      hidpi_y_scale = height / unscaled_sdl2_interface_screen_height_in_pixels;

      if (hidpi_x_scale == hidpi_y_scale) {
        sdl2_device_to_pixel_ratio = hidpi_x_scale;

        scaled_sdl2_interface_screen_width_in_pixels
          = unscaled_sdl2_interface_screen_width_in_pixels
          * sdl2_device_to_pixel_ratio;

        scaled_sdl2_interface_screen_height_in_pixels
          = unscaled_sdl2_interface_screen_height_in_pixels
          * sdl2_device_to_pixel_ratio;
      }

      if ((sdl_renderer = SDL_CreateRenderer(sdl_window, -1, 0)) == NULL) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            "SDL_CreateRenderer");
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
      }

      if ((Surf_Display = SDL_CreateRGBSurface(
              0,
              scaled_sdl2_interface_screen_width_in_pixels,
              scaled_sdl2_interface_screen_height_in_pixels,
              32,
              0x00FF0000,
              0x0000FF00,
              0x000000FF,
              0xFF000000)) == NULL) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            "SDL_GetWindowSurface");
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
      }

      if ((Surf_Backup = SDL_CreateRGBSurface(
              0,
              scaled_sdl2_interface_screen_width_in_pixels,
              scaled_sdl2_interface_screen_height_in_pixels,
              32,
              0x00FF0000,
              0x0000FF00,
              0x000000FF,
              0xFF000000)) == NULL) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            "SDL_GetWindowSurface");
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
      }

      if ((sdlTexture = SDL_CreateTexture(sdl_renderer,
          SDL_PIXELFORMAT_ARGB8888,
          SDL_TEXTUREACCESS_STREAMING,
          scaled_sdl2_interface_screen_width_in_pixels,
          scaled_sdl2_interface_screen_height_in_pixels)) == NULL) {
        i18n_translate(
            fizmo_sdl2_module_name,
            i18n_sdl2_FUNCTION_CALL_P0S_ABORTED_DUE_TO_ERROR,
            "SDL_CreateTexture");
        streams_latin1_output("\n");
        exit(EXIT_FAILURE);
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
          screen_was_updated = false;
          TRACE_LOG("Found some work to do.\n");
          SDL_LockMutex(sdl_main_thread_working_mutex);

          if (interpreter_history_was_remeasured == true) {
            interpreter_history_was_remeasured = false;
            do_update_screen();
          }

          if (main_thread_should_update_screen == true) {
            if (interpreter_is_processing_winch == true) {
              process_resize2();
              interpreter_is_processing_winch = false;
            }
            do_update_screen();
            main_thread_should_update_screen = false;
          }

          /*
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
          */

          if (main_thread_should_set_title == true) {
            set_title_and_icon();
            main_thread_should_set_title = false;
          }

          main_thread_work_complete = true;
          TRACE_LOG("Main thread work complete.\n");
          SDL_CondSignal(sdl_main_thread_working_cond);
          if (screen_was_updated == true) {
            SDL_CondSignal(update_screen_wait_cond);
          }
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
          if (Event.type == SDL_QUIT) {
            push_sdl_event_to_queue(EVENT_WAS_QUIT, 0);
          }
          else if (Event.type == SDL_TEXTINPUT) {
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
              if (resize_via_event_filter == false) {
                TRACE_LOG("Found SDL_WINDOWEVENT_EXPOSED.\n");
                if ( (resize_event_pending == false)
                    && (interpreter_is_processing_winch == false) ) {
                  if (does_resize_event_exist() == false) {

                    preprocess_nonfiltered_resize(
                        unscaled_sdl2_interface_screen_width_in_pixels,
                        unscaled_sdl2_interface_screen_height_in_pixels);
                  }
                }
              }
            }
            else if ( (resize_via_event_filter == false)
                && (Event.window.event == SDL_WINDOWEVENT_RESIZED) ) {
              TRACE_LOG("Found SDL_WINDOWEVENT_RESIZED.\n");

              preprocess_nonfiltered_resize(
                  Event.window.data1,
                  Event.window.data2);
            }
          }
        }
      }
      while (sdl_event_evluation_should_stop == false);
      // --- end event evaluation

      SDL_WaitThread(sdl_interpreter_thread, &thread_status);

      SDL_DestroySemaphore(timeout_semaphore);

      SDL_DestroyWindow(sdl_window);
      SDL_DestroyRenderer(sdl_renderer);
      SDL_FreeSurface(Surf_Display);
      SDL_FreeSurface(Surf_Backup);
      SDL_DestroyTexture(sdlTexture);

      SDL_DestroyCond(interpreter_finished_processing_winch_cond);
      SDL_DestroyCond(sdl_main_thread_working_cond);

      //SDL_DestroyMutex(interpreter_finished_processing_winch_mutex);
      SDL_DestroyMutex(resize_event_pending_mutex);
      SDL_DestroyMutex(sdl_backup_surface_mutex);
      SDL_DestroyMutex(sdl_main_thread_working_mutex);
      SDL_DestroyMutex(sdl_event_queue_mutex);

      SDL_Quit();
    }
  }

#ifdef ENABLE_TRACING
  TRACE_LOG("Turning off trace.\n\n");
  turn_off_trace();
#endif // ENABLE_TRACING

  return 0;
}

