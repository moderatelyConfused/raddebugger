// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: macOS Platform Layer (Objective-C)
//
// This file is compiled separately from the unity build:
//   clang -x objective-c -fno-objc-arc -c src/mac/mac_platform.m -o build/mac_platform.o
// and linked with:
//   -framework Cocoa -framework OpenGL -framework CoreText -framework CoreGraphics -framework QuartzCore
//
// It must not include any raddbg header. Everything it exposes is declared in
// src/mac/mac_platform.h. Manual retain/release is used throughout (no ARC).
//

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mac_platform.h"

////////////////////////////////
//~ rjf: Constants

// Carbon virtual key codes (HIToolbox/Events.h) for the modifier keys
#define MAC_PLAT_KVK_Command      0x37
#define MAC_PLAT_KVK_Shift        0x38
#define MAC_PLAT_KVK_CapsLock     0x39
#define MAC_PLAT_KVK_Option       0x3A
#define MAC_PLAT_KVK_Control      0x3B
#define MAC_PLAT_KVK_RightCommand 0x36
#define MAC_PLAT_KVK_RightShift   0x3C
#define MAC_PLAT_KVK_RightOption  0x3D
#define MAC_PLAT_KVK_RightControl 0x3E

// device-dependent modifier flag bits (IOKit/hidsystem/IOLLEvent.h, NX_DEVICE*KEYMASK)
#define MAC_PLAT_NX_DEVICELCTLKEYMASK   0x00000001
#define MAC_PLAT_NX_DEVICELSHIFTKEYMASK 0x00000002
#define MAC_PLAT_NX_DEVICERSHIFTKEYMASK 0x00000004
#define MAC_PLAT_NX_DEVICELCMDKEYMASK   0x00000008
#define MAC_PLAT_NX_DEVICERCMDKEYMASK   0x00000010
#define MAC_PLAT_NX_DEVICELALTKEYMASK   0x00000020
#define MAC_PLAT_NX_DEVICERALTKEYMASK   0x00000040
#define MAC_PLAT_NX_DEVICERCTLKEYMASK   0x00002000

// identification of our own application-defined (wakeup) events
#define MAC_PLAT_WAKEUP_SUBTYPE 1
#define MAC_PLAT_WAKEUP_DATA1   0x77616B65 // 'wake'

#define MAC_PLAT_FONT_SIZE_CACHE_COUNT 16
#define MAC_PLAT_MAX_ATLAS_DIM 16384

////////////////////////////////
//~ rjf: Cocoa Class Declarations (implementations follow the C helpers below)

@interface MAC_PLAT_WindowDelegate : NSObject <NSWindowDelegate>
{
@public
  MAC_PLAT_Window *plat_window;
}
@end

@interface MAC_PLAT_View : NSView <NSDraggingDestination>
{
@public
  MAC_PLAT_Window *plat_window;
  NSTrackingArea *tracking_area;
  bool mouse_inside;
}
@end

////////////////////////////////
//~ rjf: Window Type

struct MAC_PLAT_Window
{
  MAC_PLAT_Window *next;
  MAC_PLAT_Window *prev;
  NSWindow *window;
  MAC_PLAT_View *view;
  MAC_PLAT_WindowDelegate *delegate;
  void *user;
  bool custom_border;
  bool shown;
  bool pending_maximized;
  bool pending_fullscreen;
  float custom_title_bar_thickness; // client pixels
  float *client_rects;              // groups of 4 floats (x0, y0, x1, y1), client pixels
  uint32_t client_rect_count;
  uint32_t client_rect_cap;
};

////////////////////////////////
//~ rjf: GL Context Type

struct MAC_PLAT_GLContext
{
  NSOpenGLContext *ctx;
  NSOpenGLPixelFormat *pixel_format;
};

////////////////////////////////
//~ rjf: Font Type

typedef struct MAC_PLAT_SizedFont MAC_PLAT_SizedFont;
struct MAC_PLAT_SizedFont
{
  float size;
  CTFontRef ct_font;
};

struct MAC_PLAT_Font
{
  CGFontRef cg_font;
  MAC_PLAT_SizedFont sized[MAC_PLAT_FONT_SIZE_CACHE_COUNT];
  uint32_t sized_count;
  uint32_t sized_next;
};

////////////////////////////////
//~ rjf: Globals

static MAC_PLAT_Window *mac_plat_first_window = 0;
static MAC_PLAT_Window *mac_plat_last_window = 0;
static id mac_plat_app_delegate = nil;
static bool mac_plat_app_initialized = false;
static MAC_PLAT_FrameHook *mac_plat_frame_hook = 0;
static void *mac_plat_frame_hook_user = 0;
static bool mac_plat_in_frame_hook = false;
static MAC_PLAT_Event *mac_plat_events = 0;
static uint32_t mac_plat_events_count = 0;
static uint32_t mac_plat_events_cap = 0;
static bool mac_plat_key_down[256];
static NSCursor *mac_plat_current_cursor = nil;
static int mac_plat_mouse_inside_count = 0;
static bool mac_plat_power_off_pending = false; // set when macOS is logging out / shutting down
static NSOpenGLContext *mac_plat_gl_current = nil;
static NSMutableSet *mac_plat_retained_screens = nil;

////////////////////////////////
//~ rjf: Internal Helpers (Strings, Geometry, Events)

static NSString *
mac_plat_nsstring_from_cstring(const char *cstr)
{
  NSString *result = nil;
  if(cstr != 0)
  {
    result = [[[NSString alloc] initWithBytes:cstr length:strlen(cstr) encoding:NSUTF8StringEncoding] autorelease];
  }
  if(result == nil)
  {
    result = @"";
  }
  return result;
}

static NSScreen *
mac_plat_primary_screen(void)
{
  NSArray *screens = [NSScreen screens];
  NSScreen *result = nil;
  if([screens count] > 0)
  {
    result = [screens objectAtIndex:0];
  }
  return result;
}

static CGFloat
mac_plat_primary_scale(void)
{
  CGFloat result = 1.0;
  NSScreen *primary = mac_plat_primary_screen();
  if(primary != nil)
  {
    result = [primary backingScaleFactor];
  }
  if(result <= 0.0)
  {
    result = 1.0;
  }
  return result;
}

static CGFloat
mac_plat_primary_height(void)
{
  CGFloat result = 0.0;
  NSScreen *primary = mac_plat_primary_screen();
  if(primary != nil)
  {
    result = [primary frame].size.height;
  }
  return result;
}

// screen pixels -> cocoa global points
static NSRect
mac_plat_ns_rect_from_pixel_rect(float x0, float y0, float x1, float y1)
{
  CGFloat scale = mac_plat_primary_scale();
  CGFloat primary_h = mac_plat_primary_height();
  CGFloat w = (CGFloat)(x1 - x0) / scale;
  CGFloat h = (CGFloat)(y1 - y0) / scale;
  CGFloat x = (CGFloat)x0 / scale;
  CGFloat y = primary_h - (CGFloat)y1 / scale;
  return NSMakeRect(x, y, w, h);
}

// cocoa global points -> screen pixels
static void
mac_plat_pixel_rect_from_ns_rect(NSRect r, float *x0, float *y0, float *x1, float *y1)
{
  CGFloat scale = mac_plat_primary_scale();
  CGFloat primary_h = mac_plat_primary_height();
  *x0 = (float)(r.origin.x * scale);
  *x1 = (float)((r.origin.x + r.size.width) * scale);
  *y0 = (float)((primary_h - (r.origin.y + r.size.height)) * scale);
  *y1 = (float)((primary_h - r.origin.y) * scale);
}

// view-local points -> client pixels
static void
mac_plat_pixel_from_view_point(MAC_PLAT_Window *w, NSPoint p, float *x, float *y)
{
  NSRect bounds_backing = [w->view convertRectToBacking:[w->view bounds]];
  NSPoint p_backing = [w->view convertPointToBacking:p];
  *x = (float)p_backing.x;
  *y = (float)(bounds_backing.size.height - p_backing.y);
}

static void
mac_plat_pixel_from_event(MAC_PLAT_Window *w, NSEvent *event, float *x, float *y)
{
  NSPoint p = [w->view convertPoint:[event locationInWindow] fromView:nil];
  mac_plat_pixel_from_view_point(w, p, x, y);
}

static MAC_PLAT_Modifiers
mac_plat_modifiers_from_flags(NSEventModifierFlags flags)
{
  MAC_PLAT_Modifiers result = 0;
  if(flags & NSEventModifierFlagShift)    { result |= MAC_PLAT_Modifier_Shift; }
  if(flags & NSEventModifierFlagControl)  { result |= MAC_PLAT_Modifier_Control; }
  if(flags & NSEventModifierFlagOption)   { result |= MAC_PLAT_Modifier_Option; }
  if(flags & NSEventModifierFlagCommand)  { result |= MAC_PLAT_Modifier_Command; }
  if(flags & NSEventModifierFlagCapsLock) { result |= MAC_PLAT_Modifier_CapsLock; }
  return result;
}

static MAC_PLAT_Event *
mac_plat_push_event(MAC_PLAT_EventKind kind, MAC_PLAT_Window *window, NSEvent *ns_event)
{
  if(mac_plat_events_count >= mac_plat_events_cap)
  {
    uint32_t new_cap = mac_plat_events_cap ? mac_plat_events_cap*2 : 256;
    MAC_PLAT_Event *new_events = (MAC_PLAT_Event *)realloc(mac_plat_events, sizeof(MAC_PLAT_Event)*new_cap);
    if(new_events == 0)
    {
      // rjf: out of memory -> drop the oldest event to make room
      if(mac_plat_events_count > 0)
      {
        if(mac_plat_events[0].paths != 0)
        {
          free((void *)mac_plat_events[0].paths);
        }
        memmove(&mac_plat_events[0], &mac_plat_events[1], sizeof(MAC_PLAT_Event)*(mac_plat_events_count-1));
        mac_plat_events_count -= 1;
      }
      else
      {
        static MAC_PLAT_Event dummy;
        memset(&dummy, 0, sizeof(dummy));
        return &dummy;
      }
    }
    else
    {
      mac_plat_events = new_events;
      mac_plat_events_cap = new_cap;
    }
  }
  MAC_PLAT_Event *e = &mac_plat_events[mac_plat_events_count];
  mac_plat_events_count += 1;
  memset(e, 0, sizeof(*e));
  e->kind = kind;
  e->window = window;
  if(ns_event != nil)
  {
    e->modifiers = mac_plat_modifiers_from_flags([ns_event modifierFlags]);
    NSTimeInterval now = [[NSProcessInfo processInfo] systemUptime];
    NSTimeInterval age = now - [ns_event timestamp];
    if(age < 0) { age = 0; }
    e->age_us = (uint64_t)(age * 1000000.0);
  }
  else
  {
    e->modifiers = mac_plat_modifiers_from_flags([NSEvent modifierFlags]);
  }
  return e;
}

static bool
mac_plat_window_point_in_drag_region(MAC_PLAT_Window *w, float x, float y)
{
  bool result = false;
  if(w->custom_border && w->custom_title_bar_thickness > 0 && y >= 0 && y < w->custom_title_bar_thickness)
  {
    result = true;
    for(uint32_t idx = 0; idx < w->client_rect_count; idx += 1)
    {
      float *r = &w->client_rects[idx*4];
      if(r[0] <= x && x < r[2] && r[1] <= y && y < r[3])
      {
        result = false;
        break;
      }
    }
  }
  return result;
}

static void
mac_plat_window_did_resize(MAC_PLAT_Window *w)
{
  // rjf: keep the GL drawable in sync with the view geometry
  if(mac_plat_gl_current != nil && [mac_plat_gl_current view] == w->view)
  {
    [mac_plat_gl_current update];
  }

  // rjf: repaint through the application's frame hook (like the X11 expose path)
  if(mac_plat_frame_hook != 0 && !mac_plat_in_frame_hook && w->shown && [w->window isVisible])
  {
    mac_plat_in_frame_hook = true;
    mac_plat_frame_hook(mac_plat_frame_hook_user);
    mac_plat_in_frame_hook = false;
  }
}

static void
mac_plat_handle_mouse_button(MAC_PLAT_Window *w, NSEvent *event, uint16_t button, bool down)
{
  if(w == 0) { return; }
  float x = 0;
  float y = 0;
  mac_plat_pixel_from_event(w, event, &x, &y);

  // rjf: custom border -> presses in the title bar (outside client areas) drag/zoom the window
  if(down && button == MAC_PLAT_MouseButton_Left &&
     !([w->window styleMask] & NSWindowStyleMaskFullScreen) &&
     mac_plat_window_point_in_drag_region(w, x, y))
  {
    if([event clickCount] == 2)
    {
      [w->window zoom:nil];
    }
    else if([w->window respondsToSelector:@selector(performWindowDragWithEvent:)])
    {
      [w->window performWindowDragWithEvent:event];
    }
    return;
  }

  MAC_PLAT_Event *e = mac_plat_push_event(down ? MAC_PLAT_EventKind_MouseDown : MAC_PLAT_EventKind_MouseUp, w, event);
  e->button = button;
  e->x = x;
  e->y = y;
}

static void
mac_plat_handle_mouse_move(MAC_PLAT_Window *w, NSEvent *event)
{
  if(w == 0) { return; }
  float x = 0;
  float y = 0;
  mac_plat_pixel_from_event(w, event, &x, &y);
  MAC_PLAT_Event *e = mac_plat_push_event(MAC_PLAT_EventKind_MouseMove, w, event);
  e->x = x;
  e->y = y;
}

////////////////////////////////
//~ rjf: Application Subclass
//
// AppKit does not deliver keyUp events carrying the Command modifier to the
// key window's responder chain; forward them manually (as GLFW does).

@interface MAC_PLAT_Application : NSApplication
@end

@implementation MAC_PLAT_Application

- (void)sendEvent:(NSEvent *)event
{
  if([event type] == NSEventTypeKeyUp && ([event modifierFlags] & NSEventModifierFlagCommand))
  {
    [[self keyWindow] sendEvent:event];
  }
  else
  {
    [super sendEvent:event];
  }
}

@end

static void
mac_plat_ensure_app(void)
{
  if(NSApp == nil)
  {
    // rjf: the application object class is picked by the first sharedApplication call
    [MAC_PLAT_Application sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  }
}

////////////////////////////////
//~ rjf: Application Delegate

@interface MAC_PLAT_AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation MAC_PLAT_AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
  // rjf: we drive the event loop ourselves; stop the run loop started by
  // mac_plat_app_init as soon as the application has finished launching.
  [NSApp stop:nil];
  mac_plat_send_wakeup_event();
}

- (void)workspaceWillPowerOff:(NSNotification *)notification
{
  mac_plat_power_off_pending = true;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
  // rjf: logout / restart / shutdown -> do not block the system
  if(mac_plat_power_off_pending)
  {
    return NSTerminateNow;
  }
  // rjf: Cmd+Q / Dock quit -> ask the application to close every window; it decides.
  for(MAC_PLAT_Window *w = mac_plat_first_window; w != 0; w = w->next)
  {
    mac_plat_push_event(MAC_PLAT_EventKind_WindowClose, w, nil);
  }
  mac_plat_send_wakeup_event();
  return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
  return NO;
}

@end

////////////////////////////////
//~ rjf: Window Delegate

@implementation MAC_PLAT_WindowDelegate

- (BOOL)windowShouldClose:(NSWindow *)sender
{
  mac_plat_push_event(MAC_PLAT_EventKind_WindowClose, plat_window, nil);
  mac_plat_send_wakeup_event();
  return NO;
}

- (void)windowDidResignKey:(NSNotification *)notification
{
  mac_plat_push_event(MAC_PLAT_EventKind_WindowLoseFocus, plat_window, nil);
}

- (void)windowDidBecomeKey:(NSNotification *)notification
{
  mac_plat_push_event(MAC_PLAT_EventKind_WindowGainFocus, plat_window, nil);
}

- (void)windowDidResize:(NSNotification *)notification
{
  mac_plat_window_did_resize(plat_window);
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification
{
  mac_plat_window_did_resize(plat_window);
}

- (void)windowDidEnterFullScreen:(NSNotification *)notification
{
  mac_plat_window_did_resize(plat_window);
}

- (void)windowDidExitFullScreen:(NSNotification *)notification
{
  mac_plat_window_did_resize(plat_window);
}

@end

////////////////////////////////
//~ rjf: Content View

@implementation MAC_PLAT_View

- (instancetype)initWithFrame:(NSRect)frame
{
  self = [super initWithFrame:frame];
  if(self != nil)
  {
    plat_window = 0;
    tracking_area = nil;
    [self updateTrackingAreas];
  }
  return self;
}

- (void)dealloc
{
  if(tracking_area != nil)
  {
    [self removeTrackingArea:tracking_area];
    [tracking_area release];
    tracking_area = nil;
  }
  [super dealloc];
}

- (BOOL)acceptsFirstResponder
{
  return YES;
}

- (BOOL)canBecomeKeyView
{
  return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
  return YES;
}

- (BOOL)isOpaque
{
  return YES;
}

- (BOOL)mouseDownCanMoveWindow
{
  return NO;
}

- (BOOL)wantsUpdateLayer
{
  return NO;
}

- (void)updateTrackingAreas
{
  if(tracking_area != nil)
  {
    [self removeTrackingArea:tracking_area];
    [tracking_area release];
    tracking_area = nil;
  }
  NSTrackingAreaOptions options = (NSTrackingMouseEnteredAndExited|
                                   NSTrackingMouseMoved|
                                   NSTrackingActiveAlways|
                                   NSTrackingEnabledDuringMouseDrag|
                                   NSTrackingInVisibleRect);
  tracking_area = [[NSTrackingArea alloc] initWithRect:[self bounds] options:options owner:self userInfo:nil];
  [self addTrackingArea:tracking_area];
  [super updateTrackingAreas];
}

- (void)cursorUpdate:(NSEvent *)event
{
  if(mac_plat_current_cursor != nil)
  {
    [mac_plat_current_cursor set];
  }
}

- (void)mouseEntered:(NSEvent *)event
{
  if(!mouse_inside)
  {
    mouse_inside = true;
    mac_plat_mouse_inside_count += 1;
  }
  if(mac_plat_current_cursor != nil)
  {
    [mac_plat_current_cursor set];
  }
}

- (void)mouseExited:(NSEvent *)event
{
  if(mouse_inside)
  {
    mouse_inside = false;
    if(mac_plat_mouse_inside_count > 0)
    {
      mac_plat_mouse_inside_count -= 1;
    }
  }
}

//- rjf: mouse buttons

- (void)mouseDown:(NSEvent *)event
{
  mac_plat_handle_mouse_button(plat_window, event, MAC_PLAT_MouseButton_Left, true);
}

- (void)mouseUp:(NSEvent *)event
{
  mac_plat_handle_mouse_button(plat_window, event, MAC_PLAT_MouseButton_Left, false);
}

- (void)rightMouseDown:(NSEvent *)event
{
  mac_plat_handle_mouse_button(plat_window, event, MAC_PLAT_MouseButton_Right, true);
}

- (void)rightMouseUp:(NSEvent *)event
{
  mac_plat_handle_mouse_button(plat_window, event, MAC_PLAT_MouseButton_Right, false);
}

- (void)otherMouseDown:(NSEvent *)event
{
  if([event buttonNumber] == 2)
  {
    mac_plat_handle_mouse_button(plat_window, event, MAC_PLAT_MouseButton_Middle, true);
  }
}

- (void)otherMouseUp:(NSEvent *)event
{
  if([event buttonNumber] == 2)
  {
    mac_plat_handle_mouse_button(plat_window, event, MAC_PLAT_MouseButton_Middle, false);
  }
}

//- rjf: mouse motion

- (void)mouseMoved:(NSEvent *)event
{
  mac_plat_handle_mouse_move(plat_window, event);
}

- (void)mouseDragged:(NSEvent *)event
{
  mac_plat_handle_mouse_move(plat_window, event);
}

- (void)rightMouseDragged:(NSEvent *)event
{
  mac_plat_handle_mouse_move(plat_window, event);
}

- (void)otherMouseDragged:(NSEvent *)event
{
  mac_plat_handle_mouse_move(plat_window, event);
}

//- rjf: scrolling

- (void)scrollWheel:(NSEvent *)event
{
  if(plat_window == 0) { return; }
  float x = 0;
  float y = 0;
  mac_plat_pixel_from_event(plat_window, event, &x, &y);
  MAC_PLAT_Event *e = mac_plat_push_event(MAC_PLAT_EventKind_Scroll, plat_window, event);
  e->x = x;
  e->y = y;
  e->dx = (float)[event scrollingDeltaX];
  e->dy = (float)[event scrollingDeltaY];
  e->precise_scroll = [event hasPreciseScrollingDeltas] ? true : false;
}

//- rjf: keyboard

- (void)keyDown:(NSEvent *)event
{
  if(plat_window == 0) { return; }
  uint16_t code = [event keyCode];
  NSEventModifierFlags flags = [event modifierFlags];
  if(code < 256)
  {
    mac_plat_key_down[code] = true;
  }

  // rjf: key press
  {
    MAC_PLAT_Event *e = mac_plat_push_event(MAC_PLAT_EventKind_KeyDown, plat_window, event);
    e->key_code = code;
    e->is_repeat = [event isARepeat] ? true : false;
  }

  // rjf: text (not for shortcuts; Option is ignored like X11 does with Alt)
  if(!(flags & (NSEventModifierFlagCommand|NSEventModifierFlagControl)))
  {
    NSString *chars = (flags & NSEventModifierFlagOption) ? [event charactersIgnoringModifiers] : [event characters];
    NSUInteger len = (chars != nil) ? [chars length] : 0;
    for(NSUInteger idx = 0; idx < len; idx += 1)
    {
      uint32_t codepoint = [chars characterAtIndex:idx];
      if(codepoint >= 0xD800 && codepoint <= 0xDBFF && idx+1 < len)
      {
        uint32_t low = [chars characterAtIndex:idx+1];
        if(low >= 0xDC00 && low <= 0xDFFF)
        {
          codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
          idx += 1;
        }
      }
      if(codepoint < 32 || codepoint == 127 ||
         (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
         (codepoint >= 0xF700 && codepoint <= 0xF8FF)) // NSEvent function-key private use range
      {
        continue;
      }
      MAC_PLAT_Event *e = mac_plat_push_event(MAC_PLAT_EventKind_Text, plat_window, event);
      e->codepoint = codepoint;
    }
  }
}

- (void)keyUp:(NSEvent *)event
{
  if(plat_window == 0) { return; }
  uint16_t code = [event keyCode];
  if(code < 256)
  {
    mac_plat_key_down[code] = false;
  }
  MAC_PLAT_Event *e = mac_plat_push_event(MAC_PLAT_EventKind_KeyUp, plat_window, event);
  e->key_code = code;
}

- (void)flagsChanged:(NSEvent *)event
{
  if(plat_window == 0) { return; }
  uint16_t code = [event keyCode];
  NSEventModifierFlags flags = [event modifierFlags];
  NSEventModifierFlags generic_mask = 0;
  NSEventModifierFlags device_mask = 0;
  switch(code)
  {
    default:{}break;
    case MAC_PLAT_KVK_Shift:        {generic_mask = NSEventModifierFlagShift;    device_mask = MAC_PLAT_NX_DEVICELSHIFTKEYMASK;}break;
    case MAC_PLAT_KVK_RightShift:   {generic_mask = NSEventModifierFlagShift;    device_mask = MAC_PLAT_NX_DEVICERSHIFTKEYMASK;}break;
    case MAC_PLAT_KVK_Control:      {generic_mask = NSEventModifierFlagControl;  device_mask = MAC_PLAT_NX_DEVICELCTLKEYMASK;}break;
    case MAC_PLAT_KVK_RightControl: {generic_mask = NSEventModifierFlagControl;  device_mask = MAC_PLAT_NX_DEVICERCTLKEYMASK;}break;
    case MAC_PLAT_KVK_Option:       {generic_mask = NSEventModifierFlagOption;   device_mask = MAC_PLAT_NX_DEVICELALTKEYMASK;}break;
    case MAC_PLAT_KVK_RightOption:  {generic_mask = NSEventModifierFlagOption;   device_mask = MAC_PLAT_NX_DEVICERALTKEYMASK;}break;
    case MAC_PLAT_KVK_Command:      {generic_mask = NSEventModifierFlagCommand;  device_mask = MAC_PLAT_NX_DEVICELCMDKEYMASK;}break;
    case MAC_PLAT_KVK_RightCommand: {generic_mask = NSEventModifierFlagCommand;  device_mask = MAC_PLAT_NX_DEVICERCMDKEYMASK;}break;
    case MAC_PLAT_KVK_CapsLock:     {generic_mask = NSEventModifierFlagCapsLock; device_mask = 0;}break;
  }
  if(generic_mask != 0 && code < 256)
  {
    bool down = ((flags & generic_mask) != 0);
    if(down && device_mask != 0 && (flags & device_mask) == 0)
    {
      // rjf: the generic flag is still held by the other-sided key; this one was released
      down = false;
    }
    if(mac_plat_key_down[code] != down)
    {
      mac_plat_key_down[code] = down;
      MAC_PLAT_Event *e = mac_plat_push_event(down ? MAC_PLAT_EventKind_KeyDown : MAC_PLAT_EventKind_KeyUp, plat_window, event);
      e->key_code = code;
    }
  }
}

//- rjf: file drops

- (NSDragOperation)draggingEntered:(id <NSDraggingInfo>)sender
{
  return NSDragOperationGeneric;
}

- (BOOL)prepareForDragOperation:(id <NSDraggingInfo>)sender
{
  return YES;
}

- (BOOL)performDragOperation:(id <NSDraggingInfo>)sender
{
  if(plat_window == 0) { return NO; }
  NSPasteboard *pasteboard = [sender draggingPasteboard];
  NSDictionary *options = [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:YES] forKey:NSPasteboardURLReadingFileURLsOnlyKey];
  NSArray *urls = [pasteboard readObjectsForClasses:[NSArray arrayWithObject:[NSURL class]] options:options];
  if(urls == nil || [urls count] == 0)
  {
    return NO;
  }

  // rjf: gather paths into one NUL-separated buffer
  size_t total_size = 1;
  uint32_t path_count = 0;
  for(NSURL *url in urls)
  {
    const char *path = [[url path] fileSystemRepresentation];
    if(path != 0)
    {
      total_size += strlen(path) + 1;
      path_count += 1;
    }
  }
  char *buffer = (char *)malloc(total_size);
  if(buffer == 0)
  {
    return NO;
  }
  {
    char *ptr = buffer;
    for(NSURL *url in urls)
    {
      const char *path = [[url path] fileSystemRepresentation];
      if(path != 0)
      {
        size_t len = strlen(path);
        memcpy(ptr, path, len + 1);
        ptr += len + 1;
      }
    }
    *ptr = 0;
  }

  // rjf: push event
  float x = 0;
  float y = 0;
  {
    NSPoint p = [self convertPoint:[sender draggingLocation] fromView:nil];
    mac_plat_pixel_from_view_point(plat_window, p, &x, &y);
  }
  MAC_PLAT_Event *e = mac_plat_push_event(MAC_PLAT_EventKind_FileDrop, plat_window, nil);
  e->x = x;
  e->y = y;
  e->path_count = path_count;
  e->paths = buffer;
  mac_plat_send_wakeup_event();
  return YES;
}

@end

////////////////////////////////
//~ rjf: Application API

bool
mac_plat_app_init(const char *app_name)
{
  @autoreleasepool
  {
    if(mac_plat_app_initialized)
    {
      return true;
    }
    mac_plat_app_initialized = true;

    //- rjf: application object (our subclass), regular activation policy
    [MAC_PLAT_Application sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    //- rjf: delegate
    mac_plat_app_delegate = [[MAC_PLAT_AppDelegate alloc] init];
    [NSApp setDelegate:mac_plat_app_delegate];
    [[[NSWorkspace sharedWorkspace] notificationCenter] addObserver:mac_plat_app_delegate
                                                           selector:@selector(workspaceWillPowerOff:)
                                                               name:NSWorkspaceWillPowerOffNotification
                                                             object:nil];

    //- rjf: minimal main menu (so Cmd+Q works and the app menu exists)
    {
      NSString *name = (app_name != 0 && app_name[0] != 0) ? mac_plat_nsstring_from_cstring(app_name) : [[NSProcessInfo processInfo] processName];
      NSMenu *menu_bar = [[NSMenu alloc] initWithTitle:@""];
      NSMenuItem *app_item = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
      NSMenu *app_menu = [[NSMenu alloc] initWithTitle:name];
      NSMenuItem *quit_item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Quit %@", name]
                                                         action:@selector(terminate:)
                                                  keyEquivalent:@"q"];
      [app_menu addItem:quit_item];
      [app_item setSubmenu:app_menu];
      [menu_bar addItem:app_item];
      [NSApp setMainMenu:menu_bar];
      [quit_item release];
      [app_menu release];
      [app_item release];
      [menu_bar release];
    }

    //- rjf: screens retained for monitor handles
    mac_plat_retained_screens = [[NSMutableSet alloc] init];

    //- rjf: finish launching: run the main loop until applicationDidFinishLaunching stops it
    if(![[NSRunningApplication currentApplication] isFinishedLaunching])
    {
      [NSApp run];
    }

    //- rjf: bring the app to the front
    [NSApp activateIgnoringOtherApps:YES];

    //- rjf: default cursor
    mac_plat_current_cursor = [[NSCursor arrowCursor] retain];
  }
  return true;
}

void
mac_plat_app_get_info(MAC_PLAT_AppInfo *out)
{
  @autoreleasepool
  {
    memset(out, 0, sizeof(*out));
    out->double_click_time_s = (float)[NSEvent doubleClickInterval];
    if(out->double_click_time_s <= 0)
    {
      out->double_click_time_s = 0.5f;
    }
    out->refresh_rate_hz = 60.f;
    NSScreen *screen = [NSScreen mainScreen];
    if(screen == nil)
    {
      screen = mac_plat_primary_screen();
    }
    if(screen != nil && [screen respondsToSelector:@selector(maximumFramesPerSecond)])
    {
      NSInteger fps = [screen maximumFramesPerSecond];
      if(fps > 0)
      {
        out->refresh_rate_hz = (float)fps;
      }
    }
  }
}

void
mac_plat_set_frame_hook(MAC_PLAT_FrameHook *hook, void *user)
{
  mac_plat_frame_hook = hook;
  mac_plat_frame_hook_user = user;
}

bool
mac_plat_is_main_thread(void)
{
  return [NSThread isMainThread] ? true : false;
}

////////////////////////////////
//~ rjf: Window API

MAC_PLAT_Window *
mac_plat_window_open(float x0, float y0, float x1, float y1, MAC_PLAT_WindowFlags flags, const char *title)
{
  MAC_PLAT_Window *w = 0;
  @autoreleasepool
  {
    mac_plat_ensure_app();
    w = (MAC_PLAT_Window *)calloc(1, sizeof(MAC_PLAT_Window));
    if(w == 0)
    {
      return 0;
    }
    w->custom_border = !!(flags & MAC_PLAT_WindowFlag_CustomBorder);

    //- rjf: compute frame (cocoa points)
    NSRect frame = mac_plat_ns_rect_from_pixel_rect(x0, y0, x1, y1);
    if(frame.size.width < 100 || frame.size.height < 100 || isnan(frame.size.width) || isnan(frame.size.height))
    {
      frame.size.width = 1280;
      frame.size.height = 720;
    }
    if(isnan(frame.origin.x) || isnan(frame.origin.y))
    {
      frame.origin.x = 0;
      frame.origin.y = 0;
      flags |= MAC_PLAT_WindowFlag_DefaultPosition;
    }

    //- rjf: style
    NSWindowStyleMask style = (NSWindowStyleMaskTitled|
                               NSWindowStyleMaskClosable|
                               NSWindowStyleMaskMiniaturizable|
                               NSWindowStyleMaskResizable);
    if(w->custom_border)
    {
      style |= NSWindowStyleMaskFullSizeContentView;
    }

    //- rjf: create window
    NSRect content_rect = [NSWindow contentRectForFrameRect:frame styleMask:style];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:content_rect
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if(window == nil)
    {
      free(w);
      return 0;
    }
    [window setReleasedWhenClosed:NO];
    [window setTitle:mac_plat_nsstring_from_cstring(title)];
    [window setAcceptsMouseMovedEvents:YES];
    [window setMovableByWindowBackground:NO];
    [window setBackgroundColor:[NSColor blackColor]];
    [window setCollectionBehavior:([window collectionBehavior]|NSWindowCollectionBehaviorFullScreenPrimary)];
    [window setFrame:frame display:NO];

    //- rjf: custom border: the application draws its own title bar
    if(w->custom_border)
    {
      [window setTitlebarAppearsTransparent:YES];
      [window setTitleVisibility:NSWindowTitleHidden];
      [[window standardWindowButton:NSWindowCloseButton] setHidden:YES];
      [[window standardWindowButton:NSWindowMiniaturizeButton] setHidden:YES];
      [[window standardWindowButton:NSWindowZoomButton] setHidden:YES];
    }

    //- rjf: content view
    MAC_PLAT_View *view = [[MAC_PLAT_View alloc] initWithFrame:[[window contentView] bounds]];
    view->plat_window = w;
    [view setWantsBestResolutionOpenGLSurface:YES];
    [view registerForDraggedTypes:[NSArray arrayWithObject:NSPasteboardTypeFileURL]];
    [window setContentView:view];
    [window makeFirstResponder:view];

    //- rjf: delegate
    MAC_PLAT_WindowDelegate *delegate = [[MAC_PLAT_WindowDelegate alloc] init];
    delegate->plat_window = w;
    [window setDelegate:delegate];

    //- rjf: position
    if(flags & MAC_PLAT_WindowFlag_DefaultPosition)
    {
      [window center];
    }

    //- rjf: fill & link
    w->window = window;
    w->view = view;
    w->delegate = delegate;
    if(mac_plat_last_window != 0)
    {
      mac_plat_last_window->next = w;
      w->prev = mac_plat_last_window;
      mac_plat_last_window = w;
    }
    else
    {
      mac_plat_first_window = mac_plat_last_window = w;
    }
  }
  return w;
}

void
mac_plat_window_close(MAC_PLAT_Window *w)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    //- rjf: a window closed while the pointer is inside it never gets a mouseExited
    if(w->view != nil && w->view->mouse_inside)
    {
      w->view->mouse_inside = false;
      if(mac_plat_mouse_inside_count > 0)
      {
        mac_plat_mouse_inside_count -= 1;
      }
    }

    //- rjf: detach GL drawable if it targets this window
    if(mac_plat_gl_current != nil && [mac_plat_gl_current view] == w->view)
    {
      [mac_plat_gl_current clearDrawable];
      mac_plat_gl_current = nil;
    }

    //- rjf: scrub queued events referencing this window
    for(uint32_t idx = 0; idx < mac_plat_events_count; idx += 1)
    {
      if(mac_plat_events[idx].window == w)
      {
        mac_plat_events[idx].window = 0;
      }
    }

    //- rjf: unlink
    if(w->prev != 0) { w->prev->next = w->next; } else { mac_plat_first_window = w->next; }
    if(w->next != 0) { w->next->prev = w->prev; } else { mac_plat_last_window = w->prev; }

    //- rjf: tear down cocoa objects
    [w->window setDelegate:nil];
    w->view->plat_window = 0;
    [w->window orderOut:nil];
    [w->window close];
    [w->window release];
    [w->view release];
    [w->delegate release];

    //- rjf: free
    if(w->client_rects != 0)
    {
      free(w->client_rects);
    }
    free(w);
  }
}

void
mac_plat_window_set_user(MAC_PLAT_Window *w, void *user)
{
  if(w != 0)
  {
    w->user = user;
  }
}

void *
mac_plat_window_get_user(MAC_PLAT_Window *w)
{
  return w ? w->user : 0;
}

void
mac_plat_window_set_title(MAC_PLAT_Window *w, const char *title)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    [w->window setTitle:mac_plat_nsstring_from_cstring(title)];
  }
}

void
mac_plat_window_show(MAC_PLAT_Window *w)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    w->shown = true;
    [w->window makeKeyAndOrderFront:nil];
    if(w->pending_maximized)
    {
      w->pending_maximized = false;
      if(![w->window isZoomed])
      {
        [w->window zoom:nil];
      }
    }
    if(w->pending_fullscreen)
    {
      w->pending_fullscreen = false;
      if(!([w->window styleMask] & NSWindowStyleMaskFullScreen))
      {
        [w->window toggleFullScreen:nil];
      }
    }
  }
}

void
mac_plat_window_focus(MAC_PLAT_Window *w)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    [NSApp activateIgnoringOtherApps:YES];
    [w->window makeKeyAndOrderFront:nil];
  }
}

bool
mac_plat_window_is_focused(MAC_PLAT_Window *w)
{
  if(w == 0) { return false; }
  return [w->window isKeyWindow] ? true : false;
}

bool
mac_plat_window_is_fullscreen(MAC_PLAT_Window *w)
{
  if(w == 0) { return false; }
  if(!w->shown && w->pending_fullscreen)
  {
    return true;
  }
  return ([w->window styleMask] & NSWindowStyleMaskFullScreen) ? true : false;
}

void
mac_plat_window_set_fullscreen(MAC_PLAT_Window *w, bool fullscreen)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    if(!w->shown)
    {
      w->pending_fullscreen = fullscreen;
    }
    else
    {
      bool is_fullscreen = ([w->window styleMask] & NSWindowStyleMaskFullScreen) ? true : false;
      if(is_fullscreen != fullscreen)
      {
        [w->window toggleFullScreen:nil];
      }
    }
  }
}

bool
mac_plat_window_is_maximized(MAC_PLAT_Window *w)
{
  if(w == 0) { return false; }
  if(!w->shown && w->pending_maximized)
  {
    return true;
  }
  return [w->window isZoomed] ? true : false;
}

void
mac_plat_window_set_maximized(MAC_PLAT_Window *w, bool maximized)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    if(!w->shown)
    {
      w->pending_maximized = maximized;
    }
    else
    {
      bool is_zoomed = [w->window isZoomed] ? true : false;
      if(is_zoomed != maximized)
      {
        [w->window zoom:nil];
      }
    }
  }
}

bool
mac_plat_window_is_minimized(MAC_PLAT_Window *w)
{
  if(w == 0) { return false; }
  return [w->window isMiniaturized] ? true : false;
}

void
mac_plat_window_set_minimized(MAC_PLAT_Window *w, bool minimized)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    bool is_minimized = [w->window isMiniaturized] ? true : false;
    if(minimized && !is_minimized)
    {
      [w->window miniaturize:nil];
    }
    else if(!minimized && is_minimized)
    {
      [w->window deminiaturize:nil];
    }
  }
}

void
mac_plat_window_bring_to_front(MAC_PLAT_Window *w)
{
  if(w == 0) { return; }
  @autoreleasepool
  {
    [NSApp activateIgnoringOtherApps:YES];
    [w->window orderFront:nil];
  }
}

void
mac_plat_window_move_to_screen(MAC_PLAT_Window *w, MAC_PLAT_Screen *screen)
{
  if(w == 0 || screen == 0) { return; }
  @autoreleasepool
  {
    NSScreen *ns_screen = (NSScreen *)screen;
    NSRect visible = [ns_screen visibleFrame];
    NSRect frame = [w->window frame];
    frame.origin.x = visible.origin.x + (visible.size.width - frame.size.width)/2;
    frame.origin.y = visible.origin.y + (visible.size.height - frame.size.height)/2;
    [w->window setFrame:frame display:YES];
  }
}

void
mac_plat_window_get_frame_rect(MAC_PLAT_Window *w, float *x0, float *y0, float *x1, float *y1)
{
  *x0 = *y0 = *x1 = *y1 = 0;
  if(w == 0) { return; }
  @autoreleasepool
  {
    mac_plat_pixel_rect_from_ns_rect([w->window frame], x0, y0, x1, y1);
  }
}

void
mac_plat_window_get_client_size(MAC_PLAT_Window *w, float *width, float *height)
{
  *width = *height = 0;
  if(w == 0) { return; }
  @autoreleasepool
  {
    NSRect bounds_backing = [w->view convertRectToBacking:[w->view bounds]];
    *width = (float)bounds_backing.size.width;
    *height = (float)bounds_backing.size.height;
  }
}

float
mac_plat_window_get_scale(MAC_PLAT_Window *w)
{
  float result = 1.f;
  if(w != 0)
  {
    result = (float)[w->window backingScaleFactor];
    if(result <= 0) { result = 1.f; }
  }
  return result;
}

void
mac_plat_window_get_mouse(MAC_PLAT_Window *w, float *x, float *y)
{
  *x = *y = 0;
  if(w == 0) { return; }
  @autoreleasepool
  {
    NSPoint p = [w->window mouseLocationOutsideOfEventStream];
    p = [w->view convertPoint:p fromView:nil];
    mac_plat_pixel_from_view_point(w, p, x, y);
  }
}

void
mac_plat_window_clear_custom_border_data(MAC_PLAT_Window *w)
{
  if(w == 0) { return; }
  w->client_rect_count = 0;
  w->custom_title_bar_thickness = 0;
}

void
mac_plat_window_set_custom_title_bar(MAC_PLAT_Window *w, float thickness)
{
  if(w == 0) { return; }
  w->custom_title_bar_thickness = thickness;
}

void
mac_plat_window_push_custom_title_bar_client_rect(MAC_PLAT_Window *w, float x0, float y0, float x1, float y1)
{
  if(w == 0) { return; }
  if(w->client_rect_count >= w->client_rect_cap)
  {
    uint32_t new_cap = w->client_rect_cap ? w->client_rect_cap*2 : 32;
    float *new_rects = (float *)realloc(w->client_rects, sizeof(float)*4*new_cap);
    if(new_rects == 0)
    {
      return;
    }
    w->client_rects = new_rects;
    w->client_rect_cap = new_cap;
  }
  float *r = &w->client_rects[w->client_rect_count*4];
  r[0] = x0;
  r[1] = y0;
  r[2] = x1;
  r[3] = y1;
  w->client_rect_count += 1;
}

////////////////////////////////
//~ rjf: External Window API

int32_t
mac_plat_frontmost_app_pid(void)
{
  int32_t result = 0;
  @autoreleasepool
  {
    NSRunningApplication *app = [[NSWorkspace sharedWorkspace] frontmostApplication];
    if(app != nil)
    {
      result = (int32_t)[app processIdentifier];
    }
  }
  return result;
}

void
mac_plat_activate_app_pid(int32_t pid)
{
  if(pid <= 0) { return; }
  @autoreleasepool
  {
    NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:(pid_t)pid];
    if(app != nil)
    {
      [app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
    }
  }
}

////////////////////////////////
//~ rjf: Screen API

static MAC_PLAT_Screen *
mac_plat_screen_handle_from_ns_screen(NSScreen *screen)
{
  if(screen == nil)
  {
    return 0;
  }
  // rjf: NSScreen objects are recreated when the display configuration
  // changes; keep every one we ever handed out alive so handles stay valid.
  if(mac_plat_retained_screens == nil)
  {
    mac_plat_retained_screens = [[NSMutableSet alloc] init];
  }
  [mac_plat_retained_screens addObject:screen];
  return (MAC_PLAT_Screen *)screen;
}

uint32_t
mac_plat_screen_count(void)
{
  uint32_t result = 0;
  @autoreleasepool
  {
    result = (uint32_t)[[NSScreen screens] count];
  }
  return result;
}

MAC_PLAT_Screen *
mac_plat_screen_at(uint32_t idx)
{
  MAC_PLAT_Screen *result = 0;
  @autoreleasepool
  {
    NSArray *screens = [NSScreen screens];
    if(idx < [screens count])
    {
      result = mac_plat_screen_handle_from_ns_screen([screens objectAtIndex:idx]);
    }
  }
  return result;
}

MAC_PLAT_Screen *
mac_plat_screen_primary(void)
{
  MAC_PLAT_Screen *result = 0;
  @autoreleasepool
  {
    result = mac_plat_screen_handle_from_ns_screen(mac_plat_primary_screen());
  }
  return result;
}

MAC_PLAT_Screen *
mac_plat_screen_from_window(MAC_PLAT_Window *w)
{
  MAC_PLAT_Screen *result = 0;
  if(w == 0) { return 0; }
  @autoreleasepool
  {
    NSScreen *screen = [w->window screen];
    if(screen == nil)
    {
      screen = mac_plat_primary_screen();
    }
    result = mac_plat_screen_handle_from_ns_screen(screen);
  }
  return result;
}

size_t
mac_plat_screen_name(MAC_PLAT_Screen *screen, char *buffer, size_t capacity)
{
  size_t result = 0;
  if(capacity > 0)
  {
    buffer[0] = 0;
  }
  if(screen == 0) { return 0; }
  @autoreleasepool
  {
    NSScreen *ns_screen = (NSScreen *)screen;
    NSString *name = nil;
    if([ns_screen respondsToSelector:@selector(localizedName)])
    {
      name = [ns_screen localizedName];
    }
    if(name == nil || [name length] == 0)
    {
      NSNumber *number = [[ns_screen deviceDescription] objectForKey:@"NSScreenNumber"];
      NSString *number_string = (number != nil) ? [number stringValue] : @"0";
      name = [NSString stringWithFormat:@"Display %@", number_string];
    }
    const char *utf8 = [name UTF8String];
    if(utf8 != 0)
    {
      result = strlen(utf8);
      if(capacity > 0)
      {
        size_t copy_size = result < capacity-1 ? result : capacity-1;
        memcpy(buffer, utf8, copy_size);
        buffer[copy_size] = 0;
      }
    }
  }
  return result;
}

void
mac_plat_screen_get_pixel_size(MAC_PLAT_Screen *screen, float *width, float *height)
{
  *width = *height = 0;
  if(screen == 0) { return; }
  @autoreleasepool
  {
    NSScreen *ns_screen = (NSScreen *)screen;
    NSRect frame = [ns_screen convertRectToBacking:[ns_screen frame]];
    *width = (float)frame.size.width;
    *height = (float)frame.size.height;
  }
}

float
mac_plat_screen_get_scale(MAC_PLAT_Screen *screen)
{
  float result = 1.f;
  if(screen != 0)
  {
    result = (float)[(NSScreen *)screen backingScaleFactor];
    if(result <= 0) { result = 1.f; }
  }
  return result;
}

////////////////////////////////
//~ rjf: Event API

void
mac_plat_poll_events(bool wait)
{
  @autoreleasepool
  {
    mac_plat_ensure_app();
    NSDate *until = (wait && mac_plat_events_count == 0) ? [NSDate distantFuture] : [NSDate distantPast];
    for(;;)
    {
      NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:until
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
      if(event == nil)
      {
        break;
      }
      until = nil;
      if([event type] == NSEventTypeApplicationDefined &&
         [event subtype] == MAC_PLAT_WAKEUP_SUBTYPE &&
         [event data1] == MAC_PLAT_WAKEUP_DATA1)
      {
        mac_plat_push_event(MAC_PLAT_EventKind_Wakeup, 0, nil);
      }
      else
      {
        [NSApp sendEvent:event];
      }
    }
  }
}

uint32_t
mac_plat_event_count(void)
{
  return mac_plat_events_count;
}

const MAC_PLAT_Event *
mac_plat_event_at(uint32_t idx)
{
  const MAC_PLAT_Event *result = 0;
  if(idx < mac_plat_events_count)
  {
    result = &mac_plat_events[idx];
  }
  return result;
}

void
mac_plat_clear_events(void)
{
  for(uint32_t idx = 0; idx < mac_plat_events_count; idx += 1)
  {
    if(mac_plat_events[idx].paths != 0)
    {
      free((void *)mac_plat_events[idx].paths);
      mac_plat_events[idx].paths = 0;
    }
  }
  mac_plat_events_count = 0;
}

void
mac_plat_send_wakeup_event(void)
{
  @autoreleasepool
  {
    if(NSApp != nil)
    {
      NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                          location:NSMakePoint(0, 0)
                                     modifierFlags:0
                                         timestamp:0
                                      windowNumber:0
                                           context:nil
                                           subtype:MAC_PLAT_WAKEUP_SUBTYPE
                                             data1:MAC_PLAT_WAKEUP_DATA1
                                             data2:0];
      [NSApp postEvent:event atStart:YES];
    }
  }
}

MAC_PLAT_Modifiers
mac_plat_get_modifiers(void)
{
  MAC_PLAT_Modifiers result = 0;
  @autoreleasepool
  {
    result = mac_plat_modifiers_from_flags([NSEvent modifierFlags]);
  }
  return result;
}

uint32_t
mac_plat_get_pressed_mouse_buttons(void)
{
  uint32_t result = 0;
  @autoreleasepool
  {
    NSUInteger buttons = [NSEvent pressedMouseButtons];
    if(buttons & (1<<0)) { result |= (1<<MAC_PLAT_MouseButton_Left); }
    if(buttons & (1<<1)) { result |= (1<<MAC_PLAT_MouseButton_Right); }
    if(buttons & (1<<2)) { result |= (1<<MAC_PLAT_MouseButton_Middle); }
  }
  return result;
}

////////////////////////////////
//~ rjf: Cursor API

static NSCursor *
mac_plat_ns_cursor_from_cursor(MAC_PLAT_Cursor cursor)
{
  NSCursor *result = nil;
  switch(cursor)
  {
    default:
    case MAC_PLAT_Cursor_Arrow:           {result = [NSCursor arrowCursor];}break;
    case MAC_PLAT_Cursor_IBeam:           {result = [NSCursor IBeamCursor];}break;
    case MAC_PLAT_Cursor_ResizeLeftRight: {result = [NSCursor resizeLeftRightCursor];}break;
    case MAC_PLAT_Cursor_ResizeUpDown:    {result = [NSCursor resizeUpDownCursor];}break;
    case MAC_PLAT_Cursor_ResizeNWSE:
    {
      SEL sel = NSSelectorFromString(@"_windowResizeNorthWestSouthEastCursor");
      if([NSCursor respondsToSelector:sel])
      {
        result = [NSCursor performSelector:sel];
      }
      if(result == nil)
      {
        result = [NSCursor crosshairCursor];
      }
    }break;
    case MAC_PLAT_Cursor_ResizeNESW:
    {
      SEL sel = NSSelectorFromString(@"_windowResizeNorthEastSouthWestCursor");
      if([NSCursor respondsToSelector:sel])
      {
        result = [NSCursor performSelector:sel];
      }
      if(result == nil)
      {
        result = [NSCursor crosshairCursor];
      }
    }break;
    case MAC_PLAT_Cursor_Move:            {result = [NSCursor closedHandCursor];}break;
    case MAC_PLAT_Cursor_Hand:            {result = [NSCursor pointingHandCursor];}break;
    case MAC_PLAT_Cursor_NotAllowed:      {result = [NSCursor operationNotAllowedCursor];}break;
  }
  if(result == nil)
  {
    result = [NSCursor arrowCursor];
  }
  return result;
}

void
mac_plat_set_cursor(MAC_PLAT_Cursor cursor)
{
  @autoreleasepool
  {
    NSCursor *ns_cursor = [mac_plat_ns_cursor_from_cursor(cursor) retain];
    if(mac_plat_current_cursor != nil)
    {
      [mac_plat_current_cursor release];
    }
    mac_plat_current_cursor = ns_cursor;
    if(mac_plat_mouse_inside_count > 0)
    {
      [ns_cursor set];
    }
  }
}

////////////////////////////////
//~ rjf: Clipboard API

void
mac_plat_set_clipboard_text(const char *utf8, size_t size)
{
  @autoreleasepool
  {
    NSString *string = nil;
    if(utf8 != 0)
    {
      string = [[[NSString alloc] initWithBytes:utf8 length:size encoding:NSUTF8StringEncoding] autorelease];
    }
    if(string == nil)
    {
      string = @"";
    }
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:string forType:NSPasteboardTypeString];
  }
}

char *
mac_plat_get_clipboard_text(MAC_PLAT_AllocFunc *alloc, void *user, size_t *out_size)
{
  char *result = 0;
  size_t size = 0;
  @autoreleasepool
  {
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    NSString *string = [pasteboard stringForType:NSPasteboardTypeString];
    if(string != nil)
    {
      const char *utf8 = [string UTF8String];
      if(utf8 != 0)
      {
        size = strlen(utf8);
        result = (char *)alloc(user, size + 1);
        if(result != 0)
        {
          memcpy(result, utf8, size);
          result[size] = 0;
        }
        else
        {
          size = 0;
        }
      }
    }
  }
  if(out_size != 0)
  {
    *out_size = size;
  }
  return result;
}

////////////////////////////////
//~ rjf: OpenGL API

MAC_PLAT_GLContext *
mac_plat_gl_context_create(void)
{
  MAC_PLAT_GLContext *result = 0;
  @autoreleasepool
  {
    NSOpenGLPixelFormatAttribute attrs_accelerated[] =
    {
      NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
      NSOpenGLPFADoubleBuffer,
      NSOpenGLPFAColorSize,   24,
      NSOpenGLPFAAlphaSize,   8,
      NSOpenGLPFADepthSize,   24,
      NSOpenGLPFAStencilSize, 8,
      NSOpenGLPFAAccelerated,
      0
    };
    NSOpenGLPixelFormatAttribute attrs_fallback[] =
    {
      NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
      NSOpenGLPFADoubleBuffer,
      NSOpenGLPFAColorSize,   24,
      NSOpenGLPFAAlphaSize,   8,
      NSOpenGLPFADepthSize,   24,
      NSOpenGLPFAStencilSize, 8,
      0
    };
    NSOpenGLPixelFormat *pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs_accelerated];
    if(pixel_format == nil)
    {
      pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs_fallback];
    }
    if(pixel_format != nil)
    {
      NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:pixel_format shareContext:nil];
      if(ctx != nil)
      {
        GLint swap_interval = 1;
        [ctx setValues:&swap_interval forParameter:NSOpenGLContextParameterSwapInterval];
        result = (MAC_PLAT_GLContext *)calloc(1, sizeof(MAC_PLAT_GLContext));
        if(result != 0)
        {
          result->ctx = ctx;
          result->pixel_format = pixel_format;
        }
        else
        {
          [ctx release];
          [pixel_format release];
        }
      }
      else
      {
        [pixel_format release];
      }
    }
  }
  return result;
}

void
mac_plat_gl_context_make_current(MAC_PLAT_GLContext *ctx)
{
  if(ctx == 0) { return; }
  @autoreleasepool
  {
    [ctx->ctx makeCurrentContext];
  }
}

void
mac_plat_gl_context_set_window(MAC_PLAT_GLContext *ctx, MAC_PLAT_Window *w)
{
  if(ctx == 0) { return; }
  @autoreleasepool
  {
    if(w == 0)
    {
      [ctx->ctx clearDrawable];
      if(mac_plat_gl_current == ctx->ctx)
      {
        mac_plat_gl_current = nil;
      }
    }
    else
    {
      if([ctx->ctx view] != w->view)
      {
        [ctx->ctx setView:w->view];
      }
      [ctx->ctx update];
      mac_plat_gl_current = ctx->ctx;
    }
    [ctx->ctx makeCurrentContext];
  }
}

void
mac_plat_gl_context_update(MAC_PLAT_GLContext *ctx)
{
  if(ctx == 0) { return; }
  @autoreleasepool
  {
    [ctx->ctx update];
  }
}

void
mac_plat_gl_context_swap(MAC_PLAT_GLContext *ctx)
{
  if(ctx == 0) { return; }
  @autoreleasepool
  {
    [ctx->ctx flushBuffer];
  }
}

void
mac_plat_gl_context_set_swap_interval(MAC_PLAT_GLContext *ctx, int32_t interval)
{
  if(ctx == 0) { return; }
  @autoreleasepool
  {
    GLint value = (GLint)interval;
    [ctx->ctx setValues:&value forParameter:NSOpenGLContextParameterSwapInterval];
  }
}

////////////////////////////////
//~ rjf: Font API

static MAC_PLAT_Font *
mac_plat_font_from_provider(CGDataProviderRef provider)
{
  MAC_PLAT_Font *result = 0;
  if(provider != NULL)
  {
    CGFontRef cg_font = CGFontCreateWithDataProvider(provider);
    if(cg_font != NULL)
    {
      result = (MAC_PLAT_Font *)calloc(1, sizeof(MAC_PLAT_Font));
      if(result != 0)
      {
        result->cg_font = cg_font;
      }
      else
      {
        CGFontRelease(cg_font);
      }
    }
  }
  return result;
}

MAC_PLAT_Font *
mac_plat_font_open_from_path(const char *path)
{
  MAC_PLAT_Font *result = 0;
  if(path != 0 && path[0] != 0)
  {
    CGDataProviderRef provider = CGDataProviderCreateWithFilename(path);
    if(provider != NULL)
    {
      result = mac_plat_font_from_provider(provider);
      CGDataProviderRelease(provider);
    }
  }
  return result;
}

MAC_PLAT_Font *
mac_plat_font_open_from_memory(const void *data, size_t size)
{
  MAC_PLAT_Font *result = 0;
  if(data != 0 && size != 0)
  {
    // rjf: no release callback: the caller guarantees the data outlives the font
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, data, size, NULL);
    if(provider != NULL)
    {
      result = mac_plat_font_from_provider(provider);
      CGDataProviderRelease(provider);
    }
  }
  return result;
}

void
mac_plat_font_close(MAC_PLAT_Font *font)
{
  if(font == 0) { return; }
  for(uint32_t idx = 0; idx < font->sized_count; idx += 1)
  {
    if(font->sized[idx].ct_font != NULL)
    {
      CFRelease(font->sized[idx].ct_font);
    }
  }
  if(font->cg_font != NULL)
  {
    CGFontRelease(font->cg_font);
  }
  free(font);
}

static bool
mac_plat_font_get_metrics_impl(MAC_PLAT_Font *font, MAC_PLAT_FontMetrics *out)
{
  memset(out, 0, sizeof(*out));
  if(font == 0 || font->cg_font == NULL)
  {
    return false;
  }
  int units_per_em = CGFontGetUnitsPerEm(font->cg_font);
  if(units_per_em <= 0)
  {
    units_per_em = 1000;
  }
  out->units_per_em = (float)units_per_em;
  out->ascent       = (float)CGFontGetAscent(font->cg_font);
  out->descent      = (float)(-CGFontGetDescent(font->cg_font));
  out->line_gap     = (float)CGFontGetLeading(font->cg_font);
  out->cap_height   = (float)CGFontGetCapHeight(font->cg_font);
  if(out->cap_height <= 0)
  {
    out->cap_height = out->ascent;
  }
  return true;
}

static CTFontRef
mac_plat_font_ct_from_size(MAC_PLAT_Font *font, float size_px)
{
  CTFontRef result = NULL;
  for(uint32_t idx = 0; idx < font->sized_count; idx += 1)
  {
    if(font->sized[idx].size == size_px)
    {
      result = font->sized[idx].ct_font;
      break;
    }
  }
  if(result == NULL)
  {
    result = CTFontCreateWithGraphicsFont(font->cg_font, (CGFloat)size_px, NULL, NULL);
    if(result != NULL)
    {
      uint32_t slot = 0;
      if(font->sized_count < MAC_PLAT_FONT_SIZE_CACHE_COUNT)
      {
        slot = font->sized_count;
        font->sized_count += 1;
      }
      else
      {
        slot = font->sized_next;
        font->sized_next = (font->sized_next + 1) % MAC_PLAT_FONT_SIZE_CACHE_COUNT;
        if(font->sized[slot].ct_font != NULL)
        {
          CFRelease(font->sized[slot].ct_font);
        }
      }
      font->sized[slot].size = size_px;
      font->sized[slot].ct_font = result;
    }
  }
  return result;
}

static bool
mac_plat_font_raster_impl(MAC_PLAT_Font *font, float size_px, MAC_PLAT_RasterFlags flags,
                          const uint32_t *codepoints, size_t codepoint_count,
                          MAC_PLAT_AllocFunc *alloc, void *user, MAC_PLAT_RasterResult *out)
{
  memset(out, 0, sizeof(*out));
  if(font == 0 || font->cg_font == NULL || alloc == 0 || size_px <= 0)
  {
    return false;
  }
  CTFontRef ct_font = mac_plat_font_ct_from_size(font, size_px);
  if(ct_font == NULL)
  {
    return false;
  }

  bool result = false;
  UniChar *utf16 = 0;
  CGGlyph *utf16_glyphs = 0;
  CGGlyph *glyphs = 0;
  CGSize *advances = 0;
  CGPoint *positions = 0;

  //- rjf: codepoints -> UTF-16 (one glyph slot per codepoint, surrogate pairs map to their first unit)
  CFIndex utf16_count = 0;
  size_t *utf16_index_from_codepoint = 0;
  {
    utf16 = (UniChar *)malloc(sizeof(UniChar) * (codepoint_count*2 + 1));
    utf16_index_from_codepoint = (size_t *)malloc(sizeof(size_t) * (codepoint_count + 1));
    if(utf16 == 0 || utf16_index_from_codepoint == 0)
    {
      goto cleanup;
    }
    for(size_t idx = 0; idx < codepoint_count; idx += 1)
    {
      uint32_t cp = codepoints[idx];
      utf16_index_from_codepoint[idx] = (size_t)utf16_count;
      if(cp >= 0xD800 && cp <= 0xDFFF)
      {
        cp = 0xFFFD;
      }
      if(cp >= 0x10000 && cp <= 0x10FFFF)
      {
        cp -= 0x10000;
        utf16[utf16_count++] = (UniChar)(0xD800 + (cp >> 10));
        utf16[utf16_count++] = (UniChar)(0xDC00 + (cp & 0x3FF));
      }
      else if(cp > 0x10FFFF)
      {
        utf16[utf16_count++] = (UniChar)0xFFFD;
      }
      else
      {
        utf16[utf16_count++] = (UniChar)cp;
      }
    }
  }

  //- rjf: glyphs
  {
    utf16_glyphs = (CGGlyph *)malloc(sizeof(CGGlyph) * (utf16_count + 1));
    glyphs = (CGGlyph *)malloc(sizeof(CGGlyph) * (codepoint_count + 1));
    if(utf16_glyphs == 0 || glyphs == 0)
    {
      goto cleanup;
    }
    memset(utf16_glyphs, 0, sizeof(CGGlyph) * (utf16_count + 1));
    if(utf16_count > 0)
    {
      CTFontGetGlyphsForCharacters(ct_font, utf16, utf16_glyphs, utf16_count);
    }
    for(size_t idx = 0; idx < codepoint_count; idx += 1)
    {
      glyphs[idx] = utf16_glyphs[utf16_index_from_codepoint[idx]];
    }
  }

  //- rjf: advances
  double total_advance = 0;
  {
    advances = (CGSize *)malloc(sizeof(CGSize) * (codepoint_count + 1));
    if(advances == 0)
    {
      goto cleanup;
    }
    memset(advances, 0, sizeof(CGSize) * (codepoint_count + 1));
    if(codepoint_count > 0)
    {
      CTFontGetAdvancesForGlyphs(ct_font, kCTFontOrientationHorizontal, glyphs, advances, (CFIndex)codepoint_count);
    }
    for(size_t idx = 0; idx < codepoint_count; idx += 1)
    {
      total_advance += advances[idx].width;
    }
  }

  //- rjf: vertical metrics at this size (mirrors the FreeType backend: baseline at `ascent` rows from the top)
  int ascent  = (int)ceil(CTFontGetAscent(ct_font));
  int descent = (int)ceil(CTFontGetDescent(ct_font));
  int leading = (int)ceil(CTFontGetLeading(ct_font));
  if(ascent < 0)  { ascent = 0; }
  if(descent < 0) { descent = 0; }
  if(leading < 0) { leading = 0; }
  int height = ascent + descent + leading + 1;
  int width = (int)ceil(total_advance) + 1;
  if(width < 1)  { width = 1; }
  if(height < 1) { height = 1; }
  if(width > MAC_PLAT_MAX_ATLAS_DIM)  { width = MAC_PLAT_MAX_ATLAS_DIM; }
  if(height > MAC_PLAT_MAX_ATLAS_DIM) { height = MAC_PLAT_MAX_ATLAS_DIM; }

  //- rjf: allocate atlas on the caller's allocator
  size_t pixels_size = (size_t)width * (size_t)height * 4;
  uint8_t *pixels = (uint8_t *)alloc(user, pixels_size);
  if(pixels == 0)
  {
    goto cleanup;
  }
  memset(pixels, 0, pixels_size);

  //- rjf: rasterize: white glyphs on transparent, into the atlas memory directly
  {
    positions = (CGPoint *)malloc(sizeof(CGPoint) * (codepoint_count + 1));
    if(positions == 0)
    {
      goto cleanup;
    }
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    CGContextRef cg = CGBitmapContextCreate(pixels, (size_t)width, (size_t)height, 8, (size_t)width*4, color_space,
                                            kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(color_space);
    if(cg != NULL)
    {
      bool smooth = !!(flags & MAC_PLAT_RasterFlag_Smooth);
      bool subpixel_positioning = !(flags & MAC_PLAT_RasterFlag_Hinted);
      CGContextSetAllowsAntialiasing(cg, true);
      CGContextSetShouldAntialias(cg, true);
      CGContextSetAllowsFontSmoothing(cg, smooth);
      CGContextSetShouldSmoothFonts(cg, smooth);
      CGContextSetAllowsFontSubpixelPositioning(cg, subpixel_positioning);
      CGContextSetShouldSubpixelPositionFonts(cg, subpixel_positioning);
      CGContextSetAllowsFontSubpixelQuantization(cg, !subpixel_positioning);
      CGContextSetShouldSubpixelQuantizeFonts(cg, !subpixel_positioning);
      CGContextSetTextDrawingMode(cg, kCGTextFill);
      CGContextSetRGBFillColor(cg, 1, 1, 1, 1);
      CGContextSetTextMatrix(cg, CGAffineTransformIdentity);
      {
        double x = 0;
        CGFloat baseline_y = (CGFloat)(height - ascent); // CG y grows upward
        for(size_t idx = 0; idx < codepoint_count; idx += 1)
        {
          positions[idx] = CGPointMake((CGFloat)x, baseline_y);
          x += advances[idx].width;
        }
      }
      if(codepoint_count > 0)
      {
        CTFontDrawGlyphs(ct_font, glyphs, positions, codepoint_count, cg);
      }
      CGContextRelease(cg);
    }
  }

  //- rjf: RGB = 255, A = coverage (same encoding as the FreeType backend)
  for(size_t idx = 0; idx < (size_t)width * (size_t)height; idx += 1)
  {
    pixels[idx*4 + 0] = 255;
    pixels[idx*4 + 1] = 255;
    pixels[idx*4 + 2] = 255;
  }

  //- rjf: fill result
  out->width   = (uint32_t)width;
  out->height  = (uint32_t)height;
  out->pixels  = pixels;
  out->advance = (float)round(total_advance);
  result = true;

  cleanup:;
  if(utf16 != 0)                      { free(utf16); }
  if(utf16_index_from_codepoint != 0) { free(utf16_index_from_codepoint); }
  if(utf16_glyphs != 0)               { free(utf16_glyphs); }
  if(glyphs != 0)                     { free(glyphs); }
  if(advances != 0)                   { free(advances); }
  if(positions != 0)                  { free(positions); }
  return result;
}

////////////////////////////////
//~ rjf: Shell API

bool
mac_plat_message_box(bool error, const char *title, const char *message)
{
  if(![NSThread isMainThread])
  {
    return false;
  }
  @autoreleasepool
  {
    mac_plat_ensure_app();
    NSAlert *alert = [[[NSAlert alloc] init] autorelease];
    [alert setAlertStyle:(error ? NSAlertStyleCritical : NSAlertStyleInformational)];
    [alert setMessageText:mac_plat_nsstring_from_cstring(title)];
    [alert setInformativeText:mac_plat_nsstring_from_cstring(message)];
    [alert addButtonWithTitle:@"OK"];
    [NSApp activateIgnoringOtherApps:YES];
    [alert runModal];
  }
  return true;
}

char *
mac_plat_pick_file(const char *title, const char *initial_path, MAC_PLAT_AllocFunc *alloc, void *user, size_t *out_size)
{
  char *result = 0;
  size_t size = 0;
  if([NSThread isMainThread] && alloc != 0)
  {
    @autoreleasepool
    {
      mac_plat_ensure_app();
      NSOpenPanel *panel = [NSOpenPanel openPanel];
      [panel setCanChooseFiles:YES];
      [panel setCanChooseDirectories:NO];
      [panel setAllowsMultipleSelection:NO];
      [panel setResolvesAliases:YES];
      if(title != 0 && title[0] != 0)
      {
        NSString *ns_title = mac_plat_nsstring_from_cstring(title);
        [panel setTitle:ns_title];
        [panel setMessage:ns_title];
      }
      if(initial_path != 0 && initial_path[0] != 0)
      {
        NSString *ns_path = mac_plat_nsstring_from_cstring(initial_path);
        BOOL is_directory = NO;
        if([[NSFileManager defaultManager] fileExistsAtPath:ns_path isDirectory:&is_directory])
        {
          NSURL *url = [NSURL fileURLWithPath:ns_path];
          if(is_directory)
          {
            [panel setDirectoryURL:url];
          }
          else
          {
            [panel setDirectoryURL:[url URLByDeletingLastPathComponent]];
            [panel setNameFieldStringValue:[ns_path lastPathComponent]];
          }
        }
      }
      [NSApp activateIgnoringOtherApps:YES];
      NSModalResponse response = [panel runModal];
      if(response == NSModalResponseOK)
      {
        NSURL *url = [[panel URLs] firstObject];
        if(url != nil)
        {
          const char *path = [[url path] fileSystemRepresentation];
          if(path != 0)
          {
            size = strlen(path);
            result = (char *)alloc(user, size + 1);
            if(result != 0)
            {
              memcpy(result, path, size);
              result[size] = 0;
            }
            else
            {
              size = 0;
            }
          }
        }
      }
    }
  }
  if(out_size != 0)
  {
    *out_size = size;
  }
  return result;
}

void
mac_plat_show_in_file_browser(const char *path)
{
  @autoreleasepool
  {
    NSString *ns_path = mac_plat_nsstring_from_cstring(path);
    [[NSWorkspace sharedWorkspace] selectFile:ns_path inFileViewerRootedAtPath:@""];
  }
}

bool
mac_plat_open_url(const char *url)
{
  bool result = false;
  @autoreleasepool
  {
    NSString *ns_url_string = mac_plat_nsstring_from_cstring(url);
    NSURL *ns_url = [NSURL URLWithString:ns_url_string];
    if(ns_url == nil)
    {
      ns_url = [NSURL fileURLWithPath:ns_url_string];
    }
    if(ns_url != nil)
    {
      result = [[NSWorkspace sharedWorkspace] openURL:ns_url] ? true : false;
    }
  }
  return result;
}

#pragma clang diagnostic pop

////////////////////////////////
//~ rjf: CoreText Entry Points (autorelease-pooled wrappers)
//
// NOTE: these are called from the application's frame loop, where no
// autorelease pool is active, and CoreText autoreleases on some paths.

bool
mac_plat_font_get_metrics(MAC_PLAT_Font *font, MAC_PLAT_FontMetrics *out)
{
  bool result = false;
  @autoreleasepool
  {
    result = mac_plat_font_get_metrics_impl(font, out);
  }
  return result;
}

bool
mac_plat_font_raster(MAC_PLAT_Font *font, float size_px, MAC_PLAT_RasterFlags flags,
                     const uint32_t *codepoints, size_t codepoint_count,
                     MAC_PLAT_AllocFunc *alloc, void *user, MAC_PLAT_RasterResult *out)
{
  bool result = false;
  @autoreleasepool
  {
    result = mac_plat_font_raster_impl(font, size_px, flags, codepoints, codepoint_count, alloc, user, out);
  }
  return result;
}
