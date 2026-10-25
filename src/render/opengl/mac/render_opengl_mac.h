// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#ifndef RENDER_OPENGL_MAC_H
#define RENDER_OPENGL_MAC_H

////////////////////////////////
//~ rjf: Includes
//
// <OpenGL/gl.h> is an Apple framework header and cannot be included from the
// unity build (see src/mac/mac_platform.h). The GL types, constants and the
// fixed-function entry points that render_opengl.c uses directly are declared
// here instead; the symbols resolve from OpenGL.framework at link time, and
// the extension-style procedures in R_OGL_ProcedureXList are loaded with dlsym.

#include "mac/mac_platform.h"
#include <dlfcn.h>

#ifndef RTLD_DEFAULT
# define RTLD_DEFAULT ((void *)-2)
#endif

////////////////////////////////
//~ rjf: OpenGL Types (GLchar, GLintptr, GLsizeiptr come from render_opengl.h)

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;

////////////////////////////////
//~ rjf: OpenGL Constants

#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_NONE                           0
#define GL_NO_ERROR                       0
#define GL_ZERO                           0
#define GL_ONE                            1

#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_COLOR_BUFFER_BIT               0x00004000

#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006

#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_NOTEQUAL                       0x0205
#define GL_GEQUAL                         0x0206
#define GL_ALWAYS                         0x0207

#define GL_SRC_COLOR                      0x0300
#define GL_ONE_MINUS_SRC_COLOR            0x0301
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308

#define GL_FRONT_LEFT                     0x0400
#define GL_FRONT_RIGHT                    0x0401
#define GL_BACK_LEFT                      0x0402
#define GL_BACK_RIGHT                     0x0403
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_LEFT                           0x0406
#define GL_RIGHT                          0x0407
#define GL_FRONT_AND_BACK                 0x0408

#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_OUT_OF_MEMORY                  0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION  0x0506

#define GL_CW                             0x0900
#define GL_CCW                            0x0901

#define GL_CULL_FACE                      0x0B44
#define GL_DEPTH_TEST                     0x0B71
#define GL_STENCIL_TEST                   0x0B90
#define GL_VIEWPORT                       0x0BA2
#define GL_BLEND                          0x0BE2
#define GL_SCISSOR_BOX                    0x0C10
#define GL_SCISSOR_TEST                   0x0C11
#define GL_UNPACK_ROW_LENGTH              0x0CF2
#define GL_UNPACK_SKIP_ROWS               0x0CF3
#define GL_UNPACK_SKIP_PIXELS             0x0CF4
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_PACK_ROW_LENGTH                0x0D02
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_MAX_TEXTURE_SIZE               0x0D33
#define GL_TEXTURE_1D                     0x0DE0
#define GL_TEXTURE_2D                     0x0DE1

#define GL_DONT_CARE                      0x1100
#define GL_FASTEST                        0x1101
#define GL_NICEST                         0x1102

#define GL_BYTE                           0x1400
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_SHORT                          0x1402
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_INT                            0x1404
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406
#define GL_DOUBLE                         0x140A
#define GL_HALF_FLOAT                     0x140B

#define GL_DEPTH_COMPONENT                0x1902
#define GL_RED                            0x1903
#define GL_GREEN                          0x1904
#define GL_BLUE                           0x1905
#define GL_ALPHA                          0x1906
#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908

#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03

#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_NEAREST_MIPMAP_NEAREST         0x2700
#define GL_LINEAR_MIPMAP_NEAREST          0x2701
#define GL_NEAREST_MIPMAP_LINEAR          0x2702
#define GL_LINEAR_MIPMAP_LINEAR           0x2703
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_REPEAT                         0x2901
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_TEXTURE_BASE_LEVEL             0x813C

#define GL_RGB8                           0x8051
#define GL_RGBA8                          0x8058
#define GL_BGRA                           0x80E1
#define GL_MULTISAMPLE                    0x809D
#define GL_UNSIGNED_INT_8_8_8_8_REV       0x8367
#define GL_RG                             0x8227
#define GL_RG8                            0x822B
#define GL_R16F                           0x822D
#define GL_R32F                           0x822E
#define GL_RGBA16F                        0x881A
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_LINK_STATUS                    0x8B82
#define GL_VALIDATE_STATUS                0x8B83
#define GL_ACTIVE_UNIFORMS                0x8B86
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#define GL_SRGB8_ALPHA8                   0x8C43
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_MAX_COLOR_ATTACHMENTS          0x8CDF
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_STENCIL_ATTACHMENT             0x8D20
#define GL_RENDERBUFFER                   0x8D41
#define GL_RED_INTEGER                    0x8D94
#define GL_TEXTURE_SWIZZLE_RGBA           0x8E46

////////////////////////////////
//~ rjf: Fixed-Function Entry Points (exported by OpenGL.framework)

C_LINKAGE_BEGIN
extern void           glBindTexture(GLenum target, GLuint texture);
extern void           glBlendFunc(GLenum sfactor, GLenum dfactor);
extern void           glClear(GLbitfield mask);
extern void           glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
extern void           glClearDepth(GLclampd depth);
extern void           glClearStencil(GLint s);
extern void           glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
extern void           glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
extern void           glCullFace(GLenum mode);
extern void           glDeleteTextures(GLsizei n, const GLuint *textures);
extern void           glDepthFunc(GLenum func);
extern void           glDepthMask(GLboolean flag);
extern void           glDisable(GLenum cap);
extern void           glDrawArrays(GLenum mode, GLint first, GLsizei count);
extern void           glDrawBuffer(GLenum mode);
extern void           glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
extern void           glEnable(GLenum cap);
extern void           glFinish(void);
extern void           glFlush(void);
extern void           glFrontFace(GLenum mode);
extern void           glGenTextures(GLsizei n, GLuint *textures);
extern void           glGetBooleanv(GLenum pname, GLboolean *params);
extern GLenum         glGetError(void);
extern void           glGetFloatv(GLenum pname, GLfloat *params);
extern void           glGetIntegerv(GLenum pname, GLint *params);
extern const GLubyte *glGetString(GLenum name);
extern void           glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels);
extern void           glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
extern void           glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
extern void           glHint(GLenum target, GLenum mode);
extern GLboolean      glIsEnabled(GLenum cap);
extern GLboolean      glIsTexture(GLuint texture);
extern void           glLineWidth(GLfloat width);
extern void           glPixelStorei(GLenum pname, GLint param);
extern void           glPolygonMode(GLenum face, GLenum mode);
extern void           glReadBuffer(GLenum mode);
extern void           glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
extern void           glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
extern void           glStencilFunc(GLenum func, GLint ref, GLuint mask);
extern void           glStencilMask(GLuint mask);
extern void           glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
extern void           glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
extern void           glTexParameterf(GLenum target, GLenum pname, GLfloat param);
extern void           glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
extern void           glTexParameteri(GLenum target, GLenum pname, GLint param);
extern void           glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
extern void           glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
extern void           glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
C_LINKAGE_END

////////////////////////////////
//~ rjf: Globals

global MAC_PLAT_GLContext *r_ogl_mac_ctx = 0;
global void *r_ogl_mac_framework_handle = 0;

////////////////////////////////
//~ rjf: Helpers

internal void r_ogl_mac_stub_debug_message_callback(void (*callback)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam), void *user_data);

#endif // RENDER_OPENGL_MAC_H
