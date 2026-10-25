// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Helpers

internal void
r_ogl_mac_stub_debug_message_callback(void (*callback)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam), void *user_data)
{
  // rjf: GL_KHR_debug does not exist in macOS's OpenGL 4.1 core profile. The
  // renderer unconditionally installs a debug callback in debug builds, so
  // resolve the missing procedure to this no-op instead of a null pointer.
}

////////////////////////////////
//~ rjf: OS-Specific Hooks

internal VoidProc *
r_ogl_os_load_procedure(char *name)
{
  VoidProc *result = 0;
  if(r_ogl_mac_framework_handle == 0)
  {
    r_ogl_mac_framework_handle = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY|RTLD_LOCAL);
  }
  if(r_ogl_mac_framework_handle != 0)
  {
    result = (VoidProc *)dlsym(r_ogl_mac_framework_handle, name);
  }
  if(result == 0)
  {
    result = (VoidProc *)dlsym(RTLD_DEFAULT, name);
  }
  if(result == 0 && str8_match(str8_cstring(name), str8_lit("glDebugMessageCallback"), 0))
  {
    result = (VoidProc *)r_ogl_mac_stub_debug_message_callback;
  }
  return result;
}

internal void
r_ogl_os_init(CmdLine *cmdln)
{
  // rjf: one shared NSOpenGLContext (4.1 core profile) is used for every
  // window; its drawable is switched per window in r_ogl_os_select_window.
  r_ogl_mac_ctx = mac_plat_gl_context_create();
  if(r_ogl_mac_ctx == 0)
  {
    sh_message(1, str8_lit("Fatal Error"), str8_lit("Could not create an OpenGL 4.1 core profile context."));
    abort_self(1);
  }
  mac_plat_gl_context_set_swap_interval(r_ogl_mac_ctx, 1);
  mac_plat_gl_context_make_current(r_ogl_mac_ctx);
}

internal R_Handle
r_ogl_os_window_equip(WM_Window window)
{
  R_Handle result = {0};
  return result;
}

internal void
r_ogl_os_window_unequip(WM_Window os, R_Handle r)
{
  // rjf: the platform layer detaches the drawable itself when the window closes.
}

internal void
r_ogl_os_select_window(WM_Window os, R_Handle r)
{
  MAC_WM_Window *w = mac_wm_window_from_handle(os);
  if(w != 0 && w->plat != 0)
  {
    // rjf: binds the window's content view as the drawable (main thread),
    // updates the context for the current view geometry, and makes it current.
    // the view uses a best-resolution surface, so the drawable size equals the
    // pixel size reported by wm_client_rect_from_window.
    mac_plat_gl_context_set_window(r_ogl_mac_ctx, w->plat);
  }
  else
  {
    mac_plat_gl_context_make_current(r_ogl_mac_ctx);
  }
}

internal void
r_ogl_os_window_swap(WM_Window os, R_Handle r)
{
  mac_plat_gl_context_swap(r_ogl_mac_ctx);
}
