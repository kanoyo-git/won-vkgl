#include <stdio.h>
#include <stdlib.h>
#include "vkgl.h"

#define GL_NO_ERROR          0
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_COLOR_BUFFER_BIT  0x4000
#define GL_DEPTH_BUFFER_BIT  0x0100
#define GL_UNPACK_ALIGNMENT  0x0CF5
#define GL_PACK_ALIGNMENT    0x0D05
#define GL_VIEWPORT          0x0BA2
#define GL_MAX_TEXTURE_SIZE  0x0D33
#define GL_RGB               0x1907
#define GL_RGBA              0x1908
#define GL_UNSIGNED_BYTE     0x1401

#define GL_EXTENSIONS_STRING \
    "GL_ARB_multitexture GL_EXT_texture_object GL_EXT_texture_env_add GL_EXT_bgra "

void gl_set_error(GLenum e)
{
    if (gl_current && !gl_current->error)
        gl_current->error = e;
}

static float clampf(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

GLFUNC(const GLubyte *, glGetString, (GLenum name))
{
    static char renderer[300];

    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"vkgl";
    case GL_RENDERER:
        snprintf(renderer, sizeof(renderer), "vkgl on %s", vk_device_name());
        return (const GLubyte *)renderer;
    case GL_VERSION:
        return (const GLubyte *)"1.1.0 vkgl";
    case GL_EXTENSIONS:
        return (const GLubyte *)GL_EXTENSIONS_STRING;
    }
    gl_set_error(GL_INVALID_ENUM);
    return NULL;
}

GLFUNC(GLenum, glGetError, (void))
{
    GLenum e;

    if (!gl_current)
        return GL_NO_ERROR;
    e = gl_current->error;
    gl_current->error = GL_NO_ERROR;
    return e;
}

GLFUNC(void, glClearColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a))
{
    if (!gl_current)
        return;
    gl_current->clear_color[0] = clampf(r);
    gl_current->clear_color[1] = clampf(g);
    gl_current->clear_color[2] = clampf(b);
    gl_current->clear_color[3] = clampf(a);
}

GLFUNC(void, glClearDepth, (GLclampd z))
{
    if (gl_current)
        gl_current->clear_depth = clampf((float)z);
}

GLFUNC(void, glClear, (GLbitfield mask))
{
    struct gl_context *ctx = gl_current;
    const int *r;
    BOOL color, depth;

    if (!ctx)
        return;
    if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | 0x0400 |
                 0x0200)) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    color = (mask & GL_COLOR_BUFFER_BIT) &&
            (ctx->color_mask[0] || ctx->color_mask[1] || ctx->color_mask[2] || ctx->color_mask[3]);
    depth = (mask & GL_DEPTH_BUFFER_BIT) && ctx->depth_mask;
    if (ctx->scissor_test) {
        r = ctx->scissor;
        if (r[2] <= 0 || r[3] <= 0)
            return;
        vk_clear(color, ctx->clear_color, depth, ctx->clear_depth, r[0], r[1], r[2], r[3]);
    } else {
        vk_clear(color, ctx->clear_color, depth, ctx->clear_depth, 0, 0, 0, 0);
    }
}

GLFUNC(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))
{
    if (!gl_current)
        return;
    if (w < 0 || h < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    gl_current->viewport[0] = x;
    gl_current->viewport[1] = y;
    gl_current->viewport[2] = w;
    gl_current->viewport[3] = h;
}

GLFUNC(void, glPixelStorei, (GLenum pname, GLint param))
{
    if (!gl_current)
        return;
    if (pname != GL_PACK_ALIGNMENT && pname != GL_UNPACK_ALIGNMENT) {

        static BOOL logged;
        if (param && !logged++)
            vkgl_log("glPixelStorei(0x%04x, %d) is not supported", pname, param);
        return;
    }
    if (param != 1 && param != 2 && param != 4 && param != 8) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (pname == GL_PACK_ALIGNMENT)
        gl_current->pack_alignment = param;
    else
        gl_current->unpack_alignment = param;
}

GLFUNC(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                            GLenum type, GLvoid *pixels))
{
    int bpp, stride, align;
    unsigned char *rgba, *dst = pixels;

    if (!gl_current || !pixels || w <= 0 || h <= 0)
        return;
    if ((format != GL_RGB && format != GL_RGBA) || type != GL_UNSIGNED_BYTE) {
        vkgl_log("glReadPixels: unsupported format 0x%04x type 0x%04x", format, type);
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    rgba = malloc((size_t)w * h * 4);
    if (!rgba || !vk_read_rgba(x, y, w, h, rgba)) {
        free(rgba);
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    bpp = format == GL_RGB ? 3 : 4;
    align = gl_current->pack_alignment;
    stride = (w * bpp + align - 1) / align * align;
    for (int row = 0; row < h; row++) {
        const unsigned char *s = rgba + (size_t)row * w * 4;
        unsigned char *d = dst + (size_t)row * stride;
        if (bpp == 4) {
            memcpy(d, s, (size_t)w * 4);
            continue;
        }
        for (int i = 0; i < w; i++) {
            d[i * 3 + 0] = s[i * 4 + 0];
            d[i * 3 + 1] = s[i * 4 + 1];
            d[i * 3 + 2] = s[i * 4 + 2];
        }
    }
    free(rgba);
}

GLFUNC(void, glFlush, (void))
{
}

GLFUNC(void, glFinish, (void))
{
    vk_wait_idle();
}

GLFUNC(void, glDrawBuffer, (GLenum mode))
{
}

GLFUNC(void, glReadBuffer, (GLenum mode))
{
}
