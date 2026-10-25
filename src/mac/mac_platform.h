// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MAC_PLATFORM_H
#define MAC_PLATFORM_H

////////////////////////////////
//~ rjf: macOS Platform C API
//
// This header is the ONLY interface between the unity build (plain C, no
// Apple framework headers) and the separately-compiled Objective-C file
// src/mac/mac_platform.m (Cocoa / OpenGL / CoreText / CoreGraphics). It
// depends only on the C standard integer/size/bool headers and must not
// include any raddbg or framework header.
//
// Coordinate conventions used by every function below:
//  - "client pixels": backing (physical) pixels, origin at the top-left of a
//    window's content area, y growing downward.
//  - "screen pixels": the Cocoa global point space flipped so that the origin
//    is the top-left corner of the primary screen (the one carrying the menu
//    bar) with y growing downward, multiplied by the primary screen's backing
//    scale factor. Window frame rects are expressed in this space.
//

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

////////////////////////////////
//~ rjf: Opaque Handle Types

typedef struct MAC_PLAT_Window MAC_PLAT_Window;       // wraps an NSWindow + content view + delegate
typedef struct MAC_PLAT_Screen MAC_PLAT_Screen;       // wraps an NSScreen
typedef struct MAC_PLAT_GLContext MAC_PLAT_GLContext; // wraps an NSOpenGLContext
typedef struct MAC_PLAT_Font MAC_PLAT_Font;           // wraps a CGFontRef (+ CTFont cache)

////////////////////////////////
//~ rjf: Allocator Callback
//
// Used wherever variable-size data has to be handed back to the caller
// (clipboard text, picked paths, glyph atlases). The memory returned by the
// callback is owned by the caller; the platform layer never frees it.

typedef void *MAC_PLAT_AllocFunc(void *user, size_t size);

////////////////////////////////
//~ rjf: Application

typedef struct MAC_PLAT_AppInfo MAC_PLAT_AppInfo;
struct MAC_PLAT_AppInfo
{
  float double_click_time_s;
  float refresh_rate_hz;
};

// The frame hook is invoked (on the main thread) whenever a visible window is
// resized or changes its backing scale, so that the application can repaint
// during live resizes. The platform layer guards against re-entrancy.
typedef void MAC_PLAT_FrameHook(void *user);

bool mac_plat_app_init(const char *app_name); // creates NSApplication, menu bar, delegate; activates the app
void mac_plat_app_get_info(MAC_PLAT_AppInfo *out);
void mac_plat_set_frame_hook(MAC_PLAT_FrameHook *hook, void *user);
bool mac_plat_is_main_thread(void);

////////////////////////////////
//~ rjf: Windows

typedef uint32_t MAC_PLAT_WindowFlags;
enum
{
  MAC_PLAT_WindowFlag_CustomBorder    = (1<<0), // full-size content view, transparent/hidden title bar, hidden standard buttons
  MAC_PLAT_WindowFlag_DefaultPosition = (1<<1), // ignore the rect's position; center the window
};

MAC_PLAT_Window *mac_plat_window_open(float x0, float y0, float x1, float y1, MAC_PLAT_WindowFlags flags, const char *title); // rect in screen pixels
void             mac_plat_window_close(MAC_PLAT_Window *window);
void             mac_plat_window_set_user(MAC_PLAT_Window *window, void *user);
void *           mac_plat_window_get_user(MAC_PLAT_Window *window);
void             mac_plat_window_set_title(MAC_PLAT_Window *window, const char *title);
void             mac_plat_window_show(MAC_PLAT_Window *window); // first paint: order front + make key, apply pending maximize/fullscreen
void             mac_plat_window_focus(MAC_PLAT_Window *window);
bool             mac_plat_window_is_focused(MAC_PLAT_Window *window);
bool             mac_plat_window_is_fullscreen(MAC_PLAT_Window *window);
void             mac_plat_window_set_fullscreen(MAC_PLAT_Window *window, bool fullscreen);
bool             mac_plat_window_is_maximized(MAC_PLAT_Window *window);
void             mac_plat_window_set_maximized(MAC_PLAT_Window *window, bool maximized);
bool             mac_plat_window_is_minimized(MAC_PLAT_Window *window);
void             mac_plat_window_set_minimized(MAC_PLAT_Window *window, bool minimized);
void             mac_plat_window_bring_to_front(MAC_PLAT_Window *window);
void             mac_plat_window_move_to_screen(MAC_PLAT_Window *window, MAC_PLAT_Screen *screen); // centers the window on that screen
void             mac_plat_window_get_frame_rect(MAC_PLAT_Window *window, float *x0, float *y0, float *x1, float *y1); // screen pixels
void             mac_plat_window_get_client_size(MAC_PLAT_Window *window, float *width, float *height); // client pixels
float            mac_plat_window_get_scale(MAC_PLAT_Window *window); // backing scale factor (1.0 or 2.0 typically)
void             mac_plat_window_get_mouse(MAC_PLAT_Window *window, float *x, float *y); // client pixels

// custom border data (client pixels). a left-button press inside the top
// `thickness` pixels that is not inside any pushed client rect starts a
// native window drag (double click toggles zoom) instead of being delivered.
void             mac_plat_window_clear_custom_border_data(MAC_PLAT_Window *window);
void             mac_plat_window_set_custom_title_bar(MAC_PLAT_Window *window, float thickness);
void             mac_plat_window_push_custom_title_bar_client_rect(MAC_PLAT_Window *window, float x0, float y0, float x1, float y1);

////////////////////////////////
//~ rjf: External Windows (Other Applications)

int32_t mac_plat_frontmost_app_pid(void);
void    mac_plat_activate_app_pid(int32_t pid);

////////////////////////////////
//~ rjf: Screens

uint32_t         mac_plat_screen_count(void);
MAC_PLAT_Screen *mac_plat_screen_at(uint32_t idx);
MAC_PLAT_Screen *mac_plat_screen_primary(void);                         // screen carrying the menu bar
MAC_PLAT_Screen *mac_plat_screen_from_window(MAC_PLAT_Window *window);  // screen the window is (mostly) on
size_t           mac_plat_screen_name(MAC_PLAT_Screen *screen, char *buffer, size_t capacity); // UTF-8, NUL-terminated if capacity > 0; returns full length
void             mac_plat_screen_get_pixel_size(MAC_PLAT_Screen *screen, float *width, float *height);
float            mac_plat_screen_get_scale(MAC_PLAT_Screen *screen);

////////////////////////////////
//~ rjf: Events

typedef enum MAC_PLAT_EventKind
{
  MAC_PLAT_EventKind_Null,
  MAC_PLAT_EventKind_KeyDown,         // key_code, is_repeat
  MAC_PLAT_EventKind_KeyUp,           // key_code
  MAC_PLAT_EventKind_Text,            // codepoint
  MAC_PLAT_EventKind_MouseDown,       // button, x, y
  MAC_PLAT_EventKind_MouseUp,         // button, x, y
  MAC_PLAT_EventKind_MouseMove,       // x, y
  MAC_PLAT_EventKind_Scroll,          // x, y, dx, dy, precise_scroll
  MAC_PLAT_EventKind_WindowClose,     // the user asked to close the window (nothing is closed by the platform layer)
  MAC_PLAT_EventKind_WindowLoseFocus,
  MAC_PLAT_EventKind_WindowGainFocus,
  MAC_PLAT_EventKind_FileDrop,        // x, y, path_count, paths
  MAC_PLAT_EventKind_Wakeup,          // produced by mac_plat_send_wakeup_event
  MAC_PLAT_EventKind_COUNT
}
MAC_PLAT_EventKind;

typedef uint32_t MAC_PLAT_Modifiers;
enum
{
  MAC_PLAT_Modifier_Shift    = (1<<0),
  MAC_PLAT_Modifier_Control  = (1<<1),
  MAC_PLAT_Modifier_Option   = (1<<2),
  MAC_PLAT_Modifier_Command  = (1<<3),
  MAC_PLAT_Modifier_CapsLock = (1<<4),
};

enum
{
  MAC_PLAT_MouseButton_Left   = 0,
  MAC_PLAT_MouseButton_Right  = 1,
  MAC_PLAT_MouseButton_Middle = 2,
};

typedef struct MAC_PLAT_Event MAC_PLAT_Event;
struct MAC_PLAT_Event
{
  MAC_PLAT_EventKind kind;
  MAC_PLAT_Window *window;      // may be NULL (application-level events, or a window closed since)
  MAC_PLAT_Modifiers modifiers; // modifier state attached to the event
  uint16_t key_code;            // Carbon virtual key code (kVK_*) for KeyDown/KeyUp
  uint16_t button;              // MAC_PLAT_MouseButton_* for MouseDown/MouseUp
  bool is_repeat;               // KeyDown auto-repeat
  bool precise_scroll;          // Scroll: deltas are in points (trackpad) rather than wheel lines
  uint32_t codepoint;           // Text
  float x;                      // client pixels
  float y;                      // client pixels
  float dx;                     // Scroll: raw NSEvent scrollingDeltaX (positive = scroll left)
  float dy;                     // Scroll: raw NSEvent scrollingDeltaY (positive = scroll up)
  uint64_t age_us;              // how long before the end of the poll the event happened (microseconds)
  uint32_t path_count;          // FileDrop
  const char *paths;            // FileDrop: path_count NUL-terminated UTF-8 paths, back to back; valid until mac_plat_clear_events
};

// Polls Cocoa for events. If `wait` is true and no event is already queued,
// blocks until at least one NSEvent arrives (a wakeup counts). Every pending
// NSEvent is dispatched; the resulting MAC_PLAT_Events are appended to an
// internal queue that the caller drains with the two accessors and then
// releases with mac_plat_clear_events. Must be called from the main thread.
void                  mac_plat_poll_events(bool wait);
uint32_t              mac_plat_event_count(void);
const MAC_PLAT_Event *mac_plat_event_at(uint32_t idx);
void                  mac_plat_clear_events(void);
void                  mac_plat_send_wakeup_event(void); // thread-safe; ends a blocking poll
MAC_PLAT_Modifiers    mac_plat_get_modifiers(void);     // live modifier state
uint32_t              mac_plat_get_pressed_mouse_buttons(void); // bit (1<<MAC_PLAT_MouseButton_*)

////////////////////////////////
//~ rjf: Cursors

typedef enum MAC_PLAT_Cursor
{
  MAC_PLAT_Cursor_Arrow,
  MAC_PLAT_Cursor_IBeam,
  MAC_PLAT_Cursor_ResizeLeftRight,
  MAC_PLAT_Cursor_ResizeUpDown,
  MAC_PLAT_Cursor_ResizeNWSE,
  MAC_PLAT_Cursor_ResizeNESW,
  MAC_PLAT_Cursor_Move,
  MAC_PLAT_Cursor_Hand,
  MAC_PLAT_Cursor_NotAllowed,
  MAC_PLAT_Cursor_COUNT
}
MAC_PLAT_Cursor;

void mac_plat_set_cursor(MAC_PLAT_Cursor cursor); // applied immediately (when the mouse is over one of our windows) and remembered

////////////////////////////////
//~ rjf: Clipboard

void  mac_plat_set_clipboard_text(const char *utf8, size_t size);
char *mac_plat_get_clipboard_text(MAC_PLAT_AllocFunc *alloc, void *user, size_t *out_size); // NUL-terminated; NULL if empty

////////////////////////////////
//~ rjf: OpenGL

MAC_PLAT_GLContext *mac_plat_gl_context_create(void); // 4.1 core profile, double buffered, 24/8/24/8, swap interval 1
void                mac_plat_gl_context_make_current(MAC_PLAT_GLContext *ctx);
void                mac_plat_gl_context_set_window(MAC_PLAT_GLContext *ctx, MAC_PLAT_Window *window); // main thread only; binds the window's view (NULL detaches), updates, makes current
void                mac_plat_gl_context_update(MAC_PLAT_GLContext *ctx);
void                mac_plat_gl_context_swap(MAC_PLAT_GLContext *ctx);
void                mac_plat_gl_context_set_swap_interval(MAC_PLAT_GLContext *ctx, int32_t interval);

////////////////////////////////
//~ rjf: Fonts (CoreText)

typedef struct MAC_PLAT_FontMetrics MAC_PLAT_FontMetrics;
struct MAC_PLAT_FontMetrics
{
  float units_per_em;
  float ascent;     // design units, positive
  float descent;    // design units, positive
  float line_gap;   // design units
  float cap_height; // design units
};

typedef uint32_t MAC_PLAT_RasterFlags;
enum
{
  MAC_PLAT_RasterFlag_Smooth = (1<<0), // enable font smoothing (otherwise plain grayscale antialiasing)
  MAC_PLAT_RasterFlag_Hinted = (1<<1), // snap glyph origins to whole pixels (CoreText does not hint outlines)
};

typedef struct MAC_PLAT_RasterResult MAC_PLAT_RasterResult;
struct MAC_PLAT_RasterResult
{
  uint32_t width;   // atlas width in pixels
  uint32_t height;  // atlas height in pixels
  void *pixels;     // width*height RGBA8 pixels (RGB = 255, A = coverage), allocated with the alloc callback
  float advance;    // total horizontal advance of the string, in pixels
};

MAC_PLAT_Font *mac_plat_font_open_from_path(const char *path);
MAC_PLAT_Font *mac_plat_font_open_from_memory(const void *data, size_t size); // data must outlive the font (no copy is made)
void           mac_plat_font_close(MAC_PLAT_Font *font);
bool           mac_plat_font_get_metrics(MAC_PLAT_Font *font, MAC_PLAT_FontMetrics *out);
bool           mac_plat_font_raster(MAC_PLAT_Font *font, float size_px, MAC_PLAT_RasterFlags flags,
                                    const uint32_t *codepoints, size_t codepoint_count,
                                    MAC_PLAT_AllocFunc *alloc, void *user, MAC_PLAT_RasterResult *out);

////////////////////////////////
//~ rjf: Shell

bool  mac_plat_message_box(bool error, const char *title, const char *message); // modal; returns false if it could not be shown (not on main thread)
char *mac_plat_pick_file(const char *title, const char *initial_path, MAC_PLAT_AllocFunc *alloc, void *user, size_t *out_size); // NUL-terminated path or NULL
void  mac_plat_show_in_file_browser(const char *path);
bool  mac_plat_open_url(const char *url);

#ifdef __cplusplus
}
#endif

#endif // MAC_PLATFORM_H
