// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef MAC_WINDOW_MANAGER_H
#define MAC_WINDOW_MANAGER_H

////////////////////////////////
//~ rjf: Includes
//
// All Cocoa work happens in the separately compiled src/mac/mac_platform.m;
// this layer only talks to its plain C API.

#include "mac/mac_platform.h"

////////////////////////////////
//~ rjf: Window State

typedef struct MAC_WM_WindowClientArea MAC_WM_WindowClientArea;
struct MAC_WM_WindowClientArea
{
  MAC_WM_WindowClientArea *next;
  Rng2F32 rect;
};

typedef struct MAC_WM_Window MAC_WM_Window;
struct MAC_WM_Window
{
  MAC_WM_Window *next;
  MAC_WM_Window *prev;
  MAC_PLAT_Window *plat;
  B32 custom_border;
  F32 custom_title_bar_thickness;
  F32 custom_edge_thickness;
  MAC_WM_WindowClientArea *first_client_area;
  MAC_WM_WindowClientArea *last_client_area;
};

////////////////////////////////
//~ rjf: State Bundle

typedef struct MAC_WM_State MAC_WM_State;
struct MAC_WM_State
{
  Arena *arena;
  Arena *clipboard_arena;
  MAC_WM_Window *first_window;
  MAC_WM_Window *last_window;
  MAC_WM_Window *free_window;
  MAC_WM_WindowClientArea *free_client_area;
  WM_SystemInfo gfx_info;
  WM_Cursor last_set_cursor;
  U64 key_down_state[(WM_Key_COUNT+63)/64];
  WM_Key key_from_keycode_table[256];
  B8 keycode_is_right_sided_table[256];
  B32 in_frame_hook;
};

////////////////////////////////
//~ rjf: Globals

global MAC_WM_State *mac_wm_state = 0;

////////////////////////////////
//~ rjf: Helpers

internal WM_Window       mac_wm_handle_from_window(MAC_WM_Window *window);
internal MAC_WM_Window * mac_wm_window_from_handle(WM_Window handle);
internal MAC_WM_Window * mac_wm_window_from_plat(MAC_PLAT_Window *plat);
internal WM_Key          mac_wm_key_from_keycode(U16 keycode, B32 *out_is_right_sided);
internal WM_Modifiers    mac_wm_modifiers_from_plat(MAC_PLAT_Modifiers modifiers);
internal MAC_PLAT_Cursor mac_wm_plat_cursor_from_cursor(WM_Cursor cursor);
internal void *          mac_wm_arena_alloc(void *user, size_t size);
internal void            mac_wm_frame_hook(void *user);

#endif // MAC_WINDOW_MANAGER_H
