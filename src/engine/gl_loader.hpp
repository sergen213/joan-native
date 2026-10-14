#pragma once
// Minimal OpenGL 3.3 core loader: function pointers resolved through a caller-supplied
// getter (SDL_GL_GetProcAddress). Only the entry points the GL backend uses are listed.
#include <GL/glcorearb.h>

#define JOA_GL_FUNCTIONS(X)                                                         \
    X(PFNGLGETSTRINGPROC, glGetString)                                              \
    X(PFNGLGETERRORPROC, glGetError)                                                \
    X(PFNGLENABLEPROC, glEnable)                                                    \
    X(PFNGLDISABLEPROC, glDisable)                                                  \
    X(PFNGLVIEWPORTPROC, glViewport)                                                \
    X(PFNGLSCISSORPROC, glScissor)                                                  \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                            \
    X(PFNGLCLEARPROC, glClear)                                                      \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                              \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                              \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                                      \
    X(PFNGLCULLFACEPROC, glCullFace)                                                \
    X(PFNGLBLENDFUNCPROC, glBlendFunc)                                              \
    X(PFNGLCOLORMASKPROC, glColorMask)                                              \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc)                                          \
    X(PFNGLSTENCILOPPROC, glStencilOp)                                              \
    X(PFNGLSTENCILOPSEPARATEPROC, glStencilOpSeparate)                              \
    X(PFNGLSTENCILMASKPROC, glStencilMask)                                          \
    X(PFNGLCLEARSTENCILPROC, glClearStencil)                                        \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                          \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                            \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                          \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                    \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                          \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                      \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                            \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                      \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                                    \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                            \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                      \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                            \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                            \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                  \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                            \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                  \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                  \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                          \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer)                        \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                                        \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                            \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                        \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                        \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                      \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                          \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                        \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                      \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                        \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                          \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                        \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                              \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                      \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                            \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                            \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                              \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                              \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                              \
    X(PFNGLUNIFORM3FVPROC, glUniform3fv)                                            \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                            \
    X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv)                                \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                  \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                            \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                  \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                        \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                  \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                    \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                                \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)                          \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                                \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                          \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)

namespace joa::engine::gl {
#define JOA_GL_DECLARE(type, name) extern type name;
JOA_GL_FUNCTIONS(JOA_GL_DECLARE)
#undef JOA_GL_DECLARE

using ProcGetter = void (*(*)(const char*))();
// Resolves every entry point; returns the first missing name, or nullptr on success.
const char* load(ProcGetter get);
}  // namespace joa::engine::gl
