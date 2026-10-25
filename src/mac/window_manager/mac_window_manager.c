// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Helpers

internal WM_Window
mac_wm_handle_from_window(MAC_WM_Window *window)
{
  WM_Window handle = {(U64)window};
  return handle;
}

internal MAC_WM_Window *
mac_wm_window_from_handle(WM_Window handle)
{
  MAC_WM_Window *window = (MAC_WM_Window *)handle.u64[0];
  return window;
}

internal MAC_WM_Window *
mac_wm_window_from_plat(MAC_PLAT_Window *plat)
{
  MAC_WM_Window *result = 0;
  if(plat != 0)
  {
    result = (MAC_WM_Window *)mac_plat_window_get_user(plat);
  }
  return result;
}

internal WM_Key
mac_wm_key_from_keycode(U16 keycode, B32 *out_is_right_sided)
{
  WM_Key key = WM_Key_Null;
  B32 is_right_sided = 0;
  if(keycode < ArrayCount(mac_wm_state->key_from_keycode_table))
  {
    key = mac_wm_state->key_from_keycode_table[keycode];
    is_right_sided = mac_wm_state->keycode_is_right_sided_table[keycode];
  }
  if(out_is_right_sided)
  {
    out_is_right_sided[0] = is_right_sided;
  }
  return key;
}

internal WM_Modifiers
mac_wm_modifiers_from_plat(MAC_PLAT_Modifiers modifiers)
{
  WM_Modifiers result = 0;
  if(modifiers & MAC_PLAT_Modifier_Shift)  { result |= WM_Modifier_Shift; }
  if(modifiers & MAC_PLAT_Modifier_Option) { result |= WM_Modifier_Alt; }
  // rjf: both Command and Control act as "Ctrl", so that Cmd+S and Ctrl+S both work
  if(modifiers & (MAC_PLAT_Modifier_Control|MAC_PLAT_Modifier_Command)) { result |= WM_Modifier_Ctrl; }
  return result;
}

internal MAC_PLAT_Cursor
mac_wm_plat_cursor_from_cursor(WM_Cursor cursor)
{
  MAC_PLAT_Cursor result = MAC_PLAT_Cursor_Arrow;
  switch(cursor)
  {
    default:
    case WM_Cursor_Pointer:         {result = MAC_PLAT_Cursor_Arrow;}break;
    case WM_Cursor_IBar:            {result = MAC_PLAT_Cursor_IBeam;}break;
    case WM_Cursor_LeftRight:       {result = MAC_PLAT_Cursor_ResizeLeftRight;}break;
    case WM_Cursor_UpDown:          {result = MAC_PLAT_Cursor_ResizeUpDown;}break;
    case WM_Cursor_DownRight:       {result = MAC_PLAT_Cursor_ResizeNWSE;}break;
    case WM_Cursor_UpRight:         {result = MAC_PLAT_Cursor_ResizeNESW;}break;
    case WM_Cursor_UpDownLeftRight: {result = MAC_PLAT_Cursor_Move;}break;
    case WM_Cursor_HandPoint:       {result = MAC_PLAT_Cursor_Hand;}break;
    case WM_Cursor_Disabled:        {result = MAC_PLAT_Cursor_NotAllowed;}break;
  }
  return result;
}

internal void *
mac_wm_arena_alloc(void *user, size_t size)
{
  Arena *arena = (Arena *)user;
  void *result = push_array_no_zero(arena, U8, size);
  return result;
}

internal void
mac_wm_frame_hook(void *user)
{
  // rjf: called by the platform layer while a visible window is being resized
  // (or changes backing scale), so the UI repaints during live resizes - the
  // equivalent of the X11 Expose / Win32 WM_SIZE paths.
  if(mac_wm_state != 0 && !mac_wm_state->in_frame_hook)
  {
    mac_wm_state->in_frame_hook = 1;
    update();
    mac_wm_state->in_frame_hook = 0;
  }
}

////////////////////////////////
//~ rjf: @per_os_impl Main Initialization API (Implemented Per-OS)

internal void
wm_init(void)
{
  //- rjf: initialize basics
  Arena *arena = arena_alloc();
  mac_wm_state = push_array(arena, MAC_WM_State, 1);
  mac_wm_state->arena = arena;
  mac_wm_state->clipboard_arena = arena_alloc();
  mac_wm_state->last_set_cursor = WM_Cursor_Pointer;

  //- rjf: initialize cocoa application (NSApplication, menu bar, delegate)
  mac_plat_app_init(0);
  mac_plat_set_frame_hook(mac_wm_frame_hook, 0);

  //- rjf: fill out gfx info
  {
    MAC_PLAT_AppInfo info = {0};
    mac_plat_app_get_info(&info);
    mac_wm_state->gfx_info.double_click_time = (info.double_click_time_s > 0 ? info.double_click_time_s : 0.5f);
    mac_wm_state->gfx_info.caret_blink_time = 0.5f;
    mac_wm_state->gfx_info.default_refresh_rate = (info.refresh_rate_hz > 0 ? info.refresh_rate_hz : 60.f);
  }

  //- rjf: build key table (Carbon kVK_* virtual key codes -> WM_Key)
  {
    local_persist read_only struct
    {
      U16 keycode;
      WM_Key key;
      B8 right_sided;
    }
    map[] =
    {
      {0x00, WM_Key_A},
      {0x01, WM_Key_S},
      {0x02, WM_Key_D},
      {0x03, WM_Key_F},
      {0x04, WM_Key_H},
      {0x05, WM_Key_G},
      {0x06, WM_Key_Z},
      {0x07, WM_Key_X},
      {0x08, WM_Key_C},
      {0x09, WM_Key_V},
      {0x0A, WM_Key_Tick},         // kVK_ISO_Section (key left of 1 on ISO keyboards)
      {0x0B, WM_Key_B},
      {0x0C, WM_Key_Q},
      {0x0D, WM_Key_W},
      {0x0E, WM_Key_E},
      {0x0F, WM_Key_R},
      {0x10, WM_Key_Y},
      {0x11, WM_Key_T},
      {0x12, WM_Key_1},
      {0x13, WM_Key_2},
      {0x14, WM_Key_3},
      {0x15, WM_Key_4},
      {0x16, WM_Key_6},
      {0x17, WM_Key_5},
      {0x18, WM_Key_Equal},
      {0x19, WM_Key_9},
      {0x1A, WM_Key_7},
      {0x1B, WM_Key_Minus},
      {0x1C, WM_Key_8},
      {0x1D, WM_Key_0},
      {0x1E, WM_Key_RightBracket},
      {0x1F, WM_Key_O},
      {0x20, WM_Key_U},
      {0x21, WM_Key_LeftBracket},
      {0x22, WM_Key_I},
      {0x23, WM_Key_P},
      {0x24, WM_Key_Return},
      {0x25, WM_Key_L},
      {0x26, WM_Key_J},
      {0x27, WM_Key_Quote},
      {0x28, WM_Key_K},
      {0x29, WM_Key_Semicolon},
      {0x2A, WM_Key_BackSlash},
      {0x2B, WM_Key_Comma},
      {0x2C, WM_Key_Slash},
      {0x2D, WM_Key_N},
      {0x2E, WM_Key_M},
      {0x2F, WM_Key_Period},
      {0x30, WM_Key_Tab},
      {0x31, WM_Key_Space},
      {0x32, WM_Key_Tick},         // kVK_ANSI_Grave
      {0x33, WM_Key_Backspace},    // kVK_Delete
      {0x35, WM_Key_Esc},
      {0x36, WM_Key_Ctrl, 1},      // kVK_RightCommand (Command acts as Ctrl)
      {0x37, WM_Key_Ctrl},         // kVK_Command
      {0x38, WM_Key_Shift},        // kVK_Shift
      {0x39, WM_Key_CapsLock},
      {0x3A, WM_Key_Alt},          // kVK_Option
      {0x3B, WM_Key_Ctrl},         // kVK_Control
      {0x3C, WM_Key_Shift, 1},     // kVK_RightShift
      {0x3D, WM_Key_Alt, 1},       // kVK_RightOption
      {0x3E, WM_Key_Ctrl, 1},      // kVK_RightControl
      {0x40, WM_Key_F17},
      {0x41, WM_Key_NumPeriod},
      {0x43, WM_Key_NumStar},
      {0x45, WM_Key_NumPlus},
      {0x47, WM_Key_NumLock},      // kVK_ANSI_KeypadClear
      {0x4B, WM_Key_NumSlash},
      {0x4C, WM_Key_Return},       // kVK_ANSI_KeypadEnter
      {0x4E, WM_Key_NumMinus},
      {0x4F, WM_Key_F18},
      {0x50, WM_Key_F19},
      {0x51, WM_Key_Equal},        // kVK_ANSI_KeypadEquals
      {0x52, WM_Key_Num0},
      {0x53, WM_Key_Num1},
      {0x54, WM_Key_Num2},
      {0x55, WM_Key_Num3},
      {0x56, WM_Key_Num4},
      {0x57, WM_Key_Num5},
      {0x58, WM_Key_Num6},
      {0x59, WM_Key_Num7},
      {0x5A, WM_Key_F20},
      {0x5B, WM_Key_Num8},
      {0x5C, WM_Key_Num9},
      {0x60, WM_Key_F5},
      {0x61, WM_Key_F6},
      {0x62, WM_Key_F7},
      {0x63, WM_Key_F3},
      {0x64, WM_Key_F8},
      {0x65, WM_Key_F9},
      {0x67, WM_Key_F11},
      {0x69, WM_Key_F13},
      {0x6A, WM_Key_F16},
      {0x6B, WM_Key_F14},
      {0x6D, WM_Key_F10},
      {0x6E, WM_Key_Menu},         // kVK_ContextualMenu
      {0x6F, WM_Key_F12},
      {0x71, WM_Key_F15},
      {0x72, WM_Key_Insert},       // kVK_Help (Insert on PC keyboards)
      {0x73, WM_Key_Home},
      {0x74, WM_Key_PageUp},
      {0x75, WM_Key_Delete},       // kVK_ForwardDelete
      {0x76, WM_Key_F4},
      {0x77, WM_Key_End},
      {0x78, WM_Key_F2},
      {0x79, WM_Key_PageDown},
      {0x7A, WM_Key_F1},
      {0x7B, WM_Key_Left},
      {0x7C, WM_Key_Right},
      {0x7D, WM_Key_Down},
      {0x7E, WM_Key_Up},
    };
    for EachElement(idx, map)
    {
      if(map[idx].keycode < ArrayCount(mac_wm_state->key_from_keycode_table))
      {
        mac_wm_state->key_from_keycode_table[map[idx].keycode] = map[idx].key;
        mac_wm_state->keycode_is_right_sided_table[map[idx].keycode] = map[idx].right_sided;
      }
    }
  }
}

////////////////////////////////
//~ rjf: @per_os_impl Graphics System Info (Implemented Per-OS)

internal WM_SystemInfo *
wm_get_system_info(void)
{
  return &mac_wm_state->gfx_info;
}

////////////////////////////////
//~ rjf: @per_os_impl Clipboards (Implemented Per-OS)

internal void
wm_set_clipboard_text(String8 string)
{
  mac_plat_set_clipboard_text((char *)string.str, (size_t)string.size);
}

internal String8
wm_get_clipboard_text(Arena *arena)
{
  String8 result = {0};
  size_t size = 0;
  char *text = mac_plat_get_clipboard_text(mac_wm_arena_alloc, arena, &size);
  if(text != 0)
  {
    result = str8((U8 *)text, (U64)size);
  }
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl Windows (Implemented Per-OS)

internal WM_Window
wm_window_open(Rng2F32 rect, WM_WindowFlags flags, String8 title)
{
  Temp scratch = scratch_begin(0, 0);

  //- rjf: allocate window
  MAC_WM_Window *w = mac_wm_state->free_window;
  if(w)
  {
    SLLStackPop(mac_wm_state->free_window);
  }
  else
  {
    w = push_array_no_zero(mac_wm_state->arena, MAC_WM_Window, 1);
  }
  MemoryZeroStruct(w);
  DLLPushBack(mac_wm_state->first_window, mac_wm_state->last_window, w);

  //- rjf: create platform window
  {
    String8 title_copy = push_str8_copy(scratch.arena, title);
    MAC_PLAT_WindowFlags plat_flags = 0;
    if(flags & WM_WindowFlag_CustomBorder)       { plat_flags |= MAC_PLAT_WindowFlag_CustomBorder; }
    if(flags & WM_WindowFlag_UseDefaultPosition) { plat_flags |= MAC_PLAT_WindowFlag_DefaultPosition; }
    w->custom_border = !!(flags & WM_WindowFlag_CustomBorder);
    w->plat = mac_plat_window_open(rect.x0, rect.y0, rect.x1, rect.y1, plat_flags, (char *)title_copy.str);
    if(w->plat != 0)
    {
      mac_plat_window_set_user(w->plat, w);
    }
  }

  //- rjf: convert to handle & return
  scratch_end(scratch);
  WM_Window handle = mac_wm_handle_from_window(w);
  return handle;
}

internal void
wm_window_close(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0) {return;}
  wm_window_clear_custom_border_data(handle);
  if(w->plat != 0)
  {
    mac_plat_window_close(w->plat);
    w->plat = 0;
  }
  DLLRemove(mac_wm_state->first_window, mac_wm_state->last_window, w);
  SLLStackPush(mac_wm_state->free_window, w);
}

internal void
wm_window_set_title(WM_Window handle, String8 title)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  Temp scratch = scratch_begin(0, 0);
  String8 title_copy = push_str8_copy(scratch.arena, title);
  mac_plat_window_set_title(w->plat, (char *)title_copy.str);
  scratch_end(scratch);
}

internal void
wm_window_first_paint(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  mac_plat_window_show(w->plat);
}

internal void
wm_window_focus(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  mac_plat_window_focus(w->plat);
}

internal B32
wm_window_is_focused(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return 0;}
  B32 result = mac_plat_window_is_focused(w->plat);
  return result;
}

internal B32
wm_window_is_fullscreen(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return 0;}
  B32 result = mac_plat_window_is_fullscreen(w->plat);
  return result;
}

internal void
wm_window_set_fullscreen(WM_Window handle, B32 fullscreen)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  mac_plat_window_set_fullscreen(w->plat, !!fullscreen);
}

internal B32
wm_window_is_maximized(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return 0;}
  B32 result = mac_plat_window_is_maximized(w->plat);
  return result;
}

internal void
wm_window_set_maximized(WM_Window handle, B32 maximized)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  mac_plat_window_set_maximized(w->plat, !!maximized);
}

internal B32
wm_window_is_minimized(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return 0;}
  B32 result = mac_plat_window_is_minimized(w->plat);
  return result;
}

internal void
wm_window_set_minimized(WM_Window handle, B32 minimized)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  mac_plat_window_set_minimized(w->plat, !!minimized);
}

internal void
wm_window_bring_to_front(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  mac_plat_window_bring_to_front(w->plat);
}

internal void
wm_window_set_monitor(WM_Window handle, WM_Monitor monitor)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0 || w->plat == 0) {return;}
  MAC_PLAT_Screen *screen = (MAC_PLAT_Screen *)monitor.u64[0];
  if(screen != 0)
  {
    mac_plat_window_move_to_screen(w->plat, screen);
  }
}

internal void
wm_window_clear_custom_border_data(WM_Window handle)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0) {return;}
  for(MAC_WM_WindowClientArea *n = w->first_client_area, *next = 0; n != 0; n = next)
  {
    next = n->next;
    SLLStackPush(mac_wm_state->free_client_area, n);
  }
  w->first_client_area = w->last_client_area = 0;
  w->custom_title_bar_thickness = 0;
  w->custom_edge_thickness = 0;
  if(w->plat != 0)
  {
    mac_plat_window_clear_custom_border_data(w->plat);
  }
}

internal void
wm_window_push_custom_title_bar(WM_Window handle, F32 thickness)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0) {return;}
  w->custom_title_bar_thickness = thickness;
  if(w->plat != 0 && w->custom_border)
  {
    mac_plat_window_set_custom_title_bar(w->plat, thickness);
  }
}

internal void
wm_window_push_custom_edges(WM_Window handle, F32 thickness)
{
  // rjf: edge resizing is native on macOS; the thickness is only recorded.
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0) {return;}
  w->custom_edge_thickness = thickness;
}

internal void
wm_window_push_custom_title_bar_client_area(WM_Window handle, Rng2F32 rect)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w == 0) {return;}
  MAC_WM_WindowClientArea *n = mac_wm_state->free_client_area;
  if(n != 0)
  {
    SLLStackPop(mac_wm_state->free_client_area);
  }
  else
  {
    n = push_array(mac_wm_state->arena, MAC_WM_WindowClientArea, 1);
  }
  n->next = 0;
  n->rect = rect;
  SLLQueuePush(w->first_client_area, w->last_client_area, n);
  if(w->plat != 0 && w->custom_border)
  {
    mac_plat_window_push_custom_title_bar_client_rect(w->plat, rect.x0, rect.y0, rect.x1, rect.y1);
  }
}

internal Rng2F32
wm_rect_from_window(WM_Window handle)
{
  Rng2F32 result = {0};
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w != 0 && w->plat != 0)
  {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    mac_plat_window_get_frame_rect(w->plat, &x0, &y0, &x1, &y1);
    result = r2f32p(x0, y0, x1, y1);
  }
  return result;
}

internal Rng2F32
wm_client_rect_from_window(WM_Window handle)
{
  Rng2F32 result = {0};
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w != 0 && w->plat != 0)
  {
    float width = 0, height = 0;
    mac_plat_window_get_client_size(w->plat, &width, &height);
    result = r2f32p(0, 0, width, height);
  }
  return result;
}

internal F32
wm_dpi_from_window(WM_Window handle)
{
  F32 result = 96.f;
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w != 0 && w->plat != 0)
  {
    result = 96.f * mac_plat_window_get_scale(w->plat);
  }
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl External Windows (Implemented Per-OS)

internal WM_ExtWindow
wm_focused_external_window(void)
{
  // rjf: macOS has no cross-process window handles; track the frontmost application's pid instead.
  WM_ExtWindow result = {0};
  S32 pid = mac_plat_frontmost_app_pid();
  if(pid > 0)
  {
    result.u64[0] = (U64)pid;
  }
  return result;
}

internal void
wm_focus_external_window(WM_ExtWindow handle)
{
  S32 pid = (S32)handle.u64[0];
  if(pid > 0)
  {
    mac_plat_activate_app_pid(pid);
  }
}

////////////////////////////////
//~ rjf: @per_os_impl Monitors (Implemented Per-OS)

internal WM_MonitorArray
wm_push_monitors_array(Arena *arena)
{
  WM_MonitorArray result = {0};
  U32 count = mac_plat_screen_count();
  result.v = push_array(arena, WM_Monitor, count);
  for(U32 idx = 0; idx < count; idx += 1)
  {
    MAC_PLAT_Screen *screen = mac_plat_screen_at(idx);
    if(screen != 0)
    {
      result.v[result.count].u64[0] = (U64)screen;
      result.count += 1;
    }
  }
  return result;
}

internal WM_Monitor
wm_primary_monitor(void)
{
  WM_Monitor result = {0};
  result.u64[0] = (U64)mac_plat_screen_primary();
  return result;
}

internal WM_Monitor
wm_monitor_from_window(WM_Window handle)
{
  WM_Monitor result = {0};
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w != 0 && w->plat != 0)
  {
    result.u64[0] = (U64)mac_plat_screen_from_window(w->plat);
  }
  return result;
}

internal String8
wm_name_from_monitor(Arena *arena, WM_Monitor monitor)
{
  String8 result = {0};
  MAC_PLAT_Screen *screen = (MAC_PLAT_Screen *)monitor.u64[0];
  if(screen != 0)
  {
    char buffer[512];
    size_t length = mac_plat_screen_name(screen, buffer, sizeof(buffer));
    if(length > sizeof(buffer)-1)
    {
      length = sizeof(buffer)-1;
    }
    result = str8_copy(arena, str8((U8 *)buffer, (U64)length));
  }
  return result;
}

internal Vec2F32
wm_dim_from_monitor(WM_Monitor monitor)
{
  Vec2F32 result = {0};
  MAC_PLAT_Screen *screen = (MAC_PLAT_Screen *)monitor.u64[0];
  if(screen != 0)
  {
    float width = 0, height = 0;
    mac_plat_screen_get_pixel_size(screen, &width, &height);
    result = v2f32(width, height);
  }
  return result;
}

internal F32
wm_dpi_from_monitor(WM_Monitor monitor)
{
  F32 result = 96.f;
  MAC_PLAT_Screen *screen = (MAC_PLAT_Screen *)monitor.u64[0];
  if(screen != 0)
  {
    result = 96.f * mac_plat_screen_get_scale(screen);
  }
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl Events (Implemented Per-OS)

internal void
wm_send_wakeup_event(void)
{
  mac_plat_send_wakeup_event();
}

internal WM_EventList
wm_get_events(Arena *arena, B32 wait)
{
  WM_EventList evts = {0};
  for(;;)
  {
    //- rjf: pump cocoa (blocks only if we have nothing yet and were asked to wait)
    mac_plat_poll_events(wait && evts.count == 0);

    //- rjf: translate platform events -> WM events
    U64 now_us = now_time_us();
    U32 count = mac_plat_event_count();
    for(U32 idx = 0; idx < count; idx += 1)
    {
      const MAC_PLAT_Event *pe = mac_plat_event_at(idx);
      if(pe == 0) { break; }
      MAC_WM_Window *window = mac_wm_window_from_plat(pe->window);
      WM_Window window_handle = mac_wm_handle_from_window(window);
      WM_Modifiers modifiers = mac_wm_modifiers_from_plat(pe->modifiers);
      U64 timestamp_us = now_us - Min(now_us, pe->age_us);
      WM_Event *e = 0;
      switch(pe->kind)
      {
        default:{}break;

        //- rjf: key presses/releases
        case MAC_PLAT_EventKind_KeyDown:
        case MAC_PLAT_EventKind_KeyUp:
        {
          B32 is_press = (pe->kind == MAC_PLAT_EventKind_KeyDown);
          B32 is_right_sided = 0;
          WM_Key key = mac_wm_key_from_keycode(pe->key_code, &is_right_sided);

          // rjf: modifier keys do not carry their own modifier (matches win32)
          if(key == WM_Key_Ctrl)  { modifiers &= ~WM_Modifier_Ctrl; }
          if(key == WM_Key_Shift) { modifiers &= ~WM_Modifier_Shift; }
          if(key == WM_Key_Alt)   { modifiers &= ~WM_Modifier_Alt; }

          // rjf: update key down state
          if(is_press)
          {
            mac_wm_state->key_down_state[key/64] |= (1ull<<(key%64));
          }
          else
          {
            mac_wm_state->key_down_state[key/64] &= ~(1ull<<(key%64));
          }

          // rjf: push event
          e = wm_event_list_push_new(arena, &evts, is_press ? WM_EventKind_Press : WM_EventKind_Release);
          e->key = key;
          e->is_repeat = pe->is_repeat;
          e->right_sided = is_right_sided;
        }break;

        //- rjf: text
        case MAC_PLAT_EventKind_Text:
        {
          e = wm_event_list_push_new(arena, &evts, WM_EventKind_Text);
          e->character = pe->codepoint;
        }break;

        //- rjf: mouse button presses/releases
        case MAC_PLAT_EventKind_MouseDown:
        case MAC_PLAT_EventKind_MouseUp:
        {
          B32 is_press = (pe->kind == MAC_PLAT_EventKind_MouseDown);
          WM_Key key = WM_Key_Null;
          switch(pe->button)
          {
            default:{}break;
            case MAC_PLAT_MouseButton_Left:  {key = WM_Key_LeftMouseButton;}break;
            case MAC_PLAT_MouseButton_Right: {key = WM_Key_RightMouseButton;}break;
            case MAC_PLAT_MouseButton_Middle:{key = WM_Key_MiddleMouseButton;}break;
          }
          if(key != WM_Key_Null)
          {
            if(is_press)
            {
              mac_wm_state->key_down_state[key/64] |= (1ull<<(key%64));
            }
            else
            {
              mac_wm_state->key_down_state[key/64] &= ~(1ull<<(key%64));
            }
            e = wm_event_list_push_new(arena, &evts, is_press ? WM_EventKind_Press : WM_EventKind_Release);
            e->key = key;
            e->pos = v2f32(pe->x, pe->y);
          }
        }break;

        //- rjf: mouse motion
        case MAC_PLAT_EventKind_MouseMove:
        {
          e = wm_event_list_push_new(arena, &evts, WM_EventKind_MouseMove);
          e->pos = v2f32(pe->x, pe->y);
        }break;

        //- rjf: scrolling (one wheel notch = 120, like win32 / x11; trackpad points scaled by 120/40)
        case MAC_PLAT_EventKind_Scroll:
        {
          F32 scale = pe->precise_scroll ? (120.f/40.f) : 120.f;
          e = wm_event_list_push_new(arena, &evts, WM_EventKind_Scroll);
          e->pos = v2f32(pe->x, pe->y);
          e->delta = v2f32(-pe->dx*scale, -pe->dy*scale);
        }break;

        //- rjf: window close requests
        case MAC_PLAT_EventKind_WindowClose:
        {
          e = wm_event_list_push_new(arena, &evts, WM_EventKind_WindowClose);
        }break;

        //- rjf: window focus loss
        case MAC_PLAT_EventKind_WindowLoseFocus:
        {
          e = wm_event_list_push_new(arena, &evts, WM_EventKind_WindowLoseFocus);
        }break;

        //- rjf: file drops
        case MAC_PLAT_EventKind_FileDrop:
        {
          e = wm_event_list_push_new(arena, &evts, WM_EventKind_FileDrop);
          e->pos = v2f32(pe->x, pe->y);
          const char *ptr = pe->paths;
          for(U32 path_idx = 0; ptr != 0 && path_idx < pe->path_count; path_idx += 1)
          {
            String8 path = str8_cstring(ptr);
            String8 path_normalized = path_normalized_from_string(arena, path);
            str8_list_push(arena, &e->strings, path_normalized);
            ptr += path.size + 1;
          }
        }break;

        //- rjf: wakeups
        case MAC_PLAT_EventKind_Wakeup:
        {
          e = wm_event_list_push_new(arena, &evts, WM_EventKind_Wakeup);
        }break;
      }

      //- rjf: fill common event info
      if(e != 0)
      {
        e->timestamp_us = timestamp_us;
        e->window = window_handle;
        e->modifiers = modifiers;
      }
    }
    mac_plat_clear_events();

    //- rjf: keep waiting until we have something to report
    if(evts.count > 0 || !wait)
    {
      break;
    }
  }
  return evts;
}

internal WM_Modifiers
wm_get_modifiers(void)
{
  WM_Modifiers result = mac_wm_modifiers_from_plat(mac_plat_get_modifiers());
  return result;
}

internal B32
wm_key_is_down(WM_Key key)
{
  B32 result = 0;
  switch(key)
  {
    default:
    {
      result = !!(mac_wm_state->key_down_state[key/64] & (1ull<<(key%64)));
    }break;
    // rjf: modifier keys: both Command and Control map to Ctrl, so derive these from the
    // live modifier state rather than the press/release stream
    case WM_Key_Ctrl:  {result = !!(mac_plat_get_modifiers() & (MAC_PLAT_Modifier_Command|MAC_PLAT_Modifier_Control));}break;
    case WM_Key_Shift: {result = !!(mac_plat_get_modifiers() & MAC_PLAT_Modifier_Shift);}break;
    case WM_Key_Alt:   {result = !!(mac_plat_get_modifiers() & MAC_PLAT_Modifier_Option);}break;
    case WM_Key_LeftMouseButton:  {result = !!(mac_plat_get_pressed_mouse_buttons() & (1<<MAC_PLAT_MouseButton_Left));}break;
    case WM_Key_RightMouseButton: {result = !!(mac_plat_get_pressed_mouse_buttons() & (1<<MAC_PLAT_MouseButton_Right));}break;
    case WM_Key_MiddleMouseButton:{result = !!(mac_plat_get_pressed_mouse_buttons() & (1<<MAC_PLAT_MouseButton_Middle));}break;
  }
  return result;
}

internal Vec2F32
wm_mouse_from_window(WM_Window handle)
{
  Vec2F32 result = {0};
  MAC_WM_Window *w = mac_wm_window_from_handle(handle);
  if(w != 0 && w->plat != 0)
  {
    float x = 0, y = 0;
    mac_plat_window_get_mouse(w->plat, &x, &y);
    result = v2f32(x, y);
  }
  return result;
}

////////////////////////////////
//~ rjf: @per_os_impl Cursors (Implemented Per-OS)

internal void
wm_set_cursor(WM_Cursor cursor)
{
  mac_wm_state->last_set_cursor = cursor;
  mac_plat_set_cursor(mac_wm_plat_cursor_from_cursor(cursor));
}
