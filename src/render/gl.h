/*
 * The OpenGL API the renderer draws with. Linux and Windows (through ANGLE)
 * use OpenGL ES 3.0. macOS has no OpenGL ES, so it uses its native OpenGL 4.1
 * core profile, which contains every ES 3.0 call the renderer makes except
 * glTexStorage2D; the shaders differ only in their #version line, because
 * desktop GLSL accepts and ignores the ES precision qualifiers.
 */
#ifndef TE_GL_H
#define TE_GL_H

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#define TE_GLSL_VERSION "#version 330 core\n"
#else
#include <GLES3/gl3.h>
#define TE_GLSL_VERSION "#version 300 es\n"
#endif

#endif
