#include <stdlib.h>
#include "vkgl.h"

#define GL_TEXTURE_2D          0x0DE1
#define GL_BLEND               0x0BE2
#define GL_ALPHA_TEST          0x0BC0
#define GL_DEPTH_TEST          0x0B71
#define GL_CULL_FACE           0x0B44
#define GL_SCISSOR_TEST        0x0C11
#define GL_FOG                 0x0B60
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_DITHER              0x0BD0
#define GL_NEVER               0x0200
#define GL_LESS                0x0201
#define GL_LEQUAL              0x0203
#define GL_ALWAYS              0x0207
#define GL_ZERO                0
#define GL_ONE                 1
#define GL_FRONT               0x0404
#define GL_BACK                0x0405
#define GL_FRONT_AND_BACK      0x0408
#define GL_CW                  0x0900
#define GL_CCW                 0x0901
#define GL_FLAT                0x1D00
#define GL_SMOOTH              0x1D01
#define GL_FILL                0x1B02
#define GL_MODULATE            0x2100
#define GL_TEXTURE_ENV         0x2300
#define GL_TEXTURE_ENV_MODE    0x2200
#define GL_TEXTURE_ENV_COLOR   0x2201
#define GL_MODELVIEW           0x1700
#define GL_TEXTURE0_ARB        0x84C0
#define GL_INVALID_ENUM        0x0500
#define GL_INVALID_VALUE       0x0501
#define GL_FOG_DENSITY         0x0B62
#define GL_FOG_START           0x0B63
#define GL_FOG_END             0x0B64
#define GL_FOG_MODE            0x0B65
#define GL_FOG_COLOR           0x0B66
#define GL_EXP                 0x0800
#define GL_EXP2                0x0801
#define GL_LINEAR              0x2601

void gl_init_state(struct gl_context *ctx)
{
    ctx->clear_depth = 1.0f;
    ctx->pack_alignment = ctx->unpack_alignment = 4;
    ctx->blend_src = GL_ONE;
    ctx->blend_dst = GL_ZERO;
    ctx->alpha_func = GL_ALWAYS;
    ctx->depth_func = GL_LESS;
    ctx->cull_mode = GL_BACK;
    ctx->front_face = GL_CCW;
    ctx->shade_model = GL_SMOOTH;
    ctx->polygon_mode = GL_FILL;
    ctx->depth_mask = TRUE;
    ctx->dither = TRUE;
    for (int i = 0; i < 4; i++)
        ctx->color_mask[i] = TRUE;
    ctx->depth_range[1] = 1.0f;
    ctx->fog_mode = GL_EXP;
    ctx->fog_density = ctx->fog_end = 1.0f;
    ctx->matrix_mode = GL_MODELVIEW;
    for (int m = 0; m < 3; m++)
        for (int i = 0; i < 16; i++)
            ctx->matrix[m][0][i] = (i % 5) ? 0.0f : 1.0f;
    ctx->mvp_dirty = TRUE;
    for (int u = 0; u < MAX_TEX_UNITS; u++) {
        ctx->unit[u].env_mode = GL_MODULATE;
        ctx->unit[u].texcoord[3] = 1.0f;
    }
    for (int i = 0; i < 4; i++) {
        ctx->color[i] = 1.0f;
        ctx->color_ub[i] = 255;
    }
}

#define MAX_OTHER_CAPS 32
static struct { GLenum cap; BOOL on; } other_caps[MAX_OTHER_CAPS];

static BOOL *cap_flag(struct gl_context *ctx, GLenum cap)
{
    switch (cap) {
    case GL_TEXTURE_2D: return &ctx->unit[ctx->active_texture].enabled;
    case GL_BLEND: return &ctx->blend;
    case GL_ALPHA_TEST: return &ctx->alpha_test;
    case GL_DEPTH_TEST: return &ctx->depth_test;
    case GL_CULL_FACE: return &ctx->cull_face;
    case GL_SCISSOR_TEST: return &ctx->scissor_test;
    case GL_FOG: return &ctx->fog;
    case GL_POLYGON_OFFSET_FILL: return &ctx->polygon_offset_fill;
    case GL_DITHER: return &ctx->dither;
    }
    for (int i = 0; i < MAX_OTHER_CAPS; i++) {
        if (other_caps[i].cap == cap)
            return &other_caps[i].on;
        if (!other_caps[i].cap) {
            vkgl_log("capability 0x%04x is accepted but has no effect", cap);
            other_caps[i].cap = cap;
            return &other_caps[i].on;
        }
    }
    return NULL;
}

static void set_cap(GLenum cap, BOOL on)
{
    BOOL *flag;

    if (!gl_current)
        return;
    flag = cap_flag(gl_current, cap);
    if (!flag) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    *flag = on;
}

GLFUNC(void, glEnable, (GLenum cap)) { set_cap(cap, TRUE); }
GLFUNC(void, glDisable, (GLenum cap)) { set_cap(cap, FALSE); }

GLFUNC(GLboolean, glIsEnabled, (GLenum cap))
{
    BOOL *flag = gl_current ? cap_flag(gl_current, cap) : NULL;
    return flag && *flag;
}

GLFUNC(void, glBlendFunc, (GLenum src, GLenum dst))
{
    if (!gl_current)
        return;
    gl_current->blend_src = src;
    gl_current->blend_dst = dst;
}

GLFUNC(void, glAlphaFunc, (GLenum func, GLclampf ref))
{
    if (!gl_current)
        return;
    if (func < GL_NEVER || func > GL_ALWAYS) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    gl_current->alpha_func = func;
    gl_current->alpha_ref = ref < 0 ? 0 : ref > 1 ? 1 : ref;
}

GLFUNC(void, glDepthFunc, (GLenum func))
{
    if (!gl_current)
        return;
    if (func < GL_NEVER || func > GL_ALWAYS) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    gl_current->depth_func = func;
}

GLFUNC(void, glDepthMask, (GLboolean flag))
{
    if (gl_current)
        gl_current->depth_mask = flag != 0;
}

GLFUNC(void, glDepthRange, (GLclampd n, GLclampd f))
{
    if (!gl_current)
        return;
    gl_current->depth_range[0] = n < 0 ? 0 : n > 1 ? 1 : (float)n;
    gl_current->depth_range[1] = f < 0 ? 0 : f > 1 ? 1 : (float)f;
}

GLFUNC(void, glCullFace, (GLenum mode))
{
    if (gl_current)
        gl_current->cull_mode = mode;
}

GLFUNC(void, glFrontFace, (GLenum mode))
{
    if (gl_current)
        gl_current->front_face = mode;
}

GLFUNC(void, glShadeModel, (GLenum mode))
{
    if (gl_current)
        gl_current->shade_model = mode;
}

GLFUNC(void, glPolygonMode, (GLenum face, GLenum mode))
{
    if (gl_current)
        gl_current->polygon_mode = mode;
}

GLFUNC(void, glPolygonOffset, (GLfloat factor, GLfloat units))
{
    if (!gl_current)
        return;
    gl_current->polygon_offset[0] = factor;
    gl_current->polygon_offset[1] = units;
}

GLFUNC(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))
{
    if (!gl_current)
        return;
    if (w < 0 || h < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    gl_current->scissor[0] = x;
    gl_current->scissor[1] = y;
    gl_current->scissor[2] = w;
    gl_current->scissor[3] = h;
}

GLFUNC(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))
{
    if (!gl_current)
        return;
    gl_current->color_mask[0] = r != 0;
    gl_current->color_mask[1] = g != 0;
    gl_current->color_mask[2] = b != 0;
    gl_current->color_mask[3] = a != 0;
}

GLFUNC(void, glClearStencil, (GLint s))
{
    if (gl_current)
        gl_current->clear_stencil = s;
}

static void tex_env(GLenum target, GLenum pname, const float *v)
{
    struct gl_texunit *u;

    if (!gl_current)
        return;
    if (target != GL_TEXTURE_ENV) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    u = &gl_current->unit[gl_current->active_texture];
    if (pname == GL_TEXTURE_ENV_MODE)
        u->env_mode = (GLenum)v[0];
    else if (pname == GL_TEXTURE_ENV_COLOR)
        memcpy(u->env_color, v, sizeof(u->env_color));
}

GLFUNC(void, glTexEnvf, (GLenum target, GLenum pname, GLfloat value))
{
    float v[4] = { value };
    tex_env(target, pname, v);
}

GLFUNC(void, glTexEnvi, (GLenum target, GLenum pname, GLint value))
{
    float v[4] = { (float)value };
    tex_env(target, pname, v);
}

GLFUNC(void, glTexEnvfv, (GLenum target, GLenum pname, const GLfloat *value))
{
    tex_env(target, pname, value);
}

GLFUNC(void, glTexEnviv, (GLenum target, GLenum pname, const GLint *value))
{
    float v[4] = { (float)value[0] };
    if (pname == GL_TEXTURE_ENV_COLOR)
        for (int i = 0; i < 4; i++)
            v[i] = value[i] / 2147483647.0f;
    tex_env(target, pname, v);
}

static void fog(GLenum pname, const float *v)
{
    struct gl_context *ctx = gl_current;

    if (!ctx)
        return;
    switch (pname) {
    case GL_FOG_MODE:
        if (v[0] == GL_LINEAR || v[0] == GL_EXP || v[0] == GL_EXP2)
            ctx->fog_mode = (GLenum)v[0];
        else
            gl_set_error(GL_INVALID_ENUM);
        break;
    case GL_FOG_DENSITY:
        if (v[0] < 0)
            gl_set_error(GL_INVALID_VALUE);
        else
            ctx->fog_density = v[0];
        break;
    case GL_FOG_START: ctx->fog_start = v[0]; break;
    case GL_FOG_END: ctx->fog_end = v[0]; break;
    case GL_FOG_COLOR:
        for (int i = 0; i < 4; i++)
            ctx->fog_color[i] = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i];
        break;
    case 0x0B61: break;
    default: gl_set_error(GL_INVALID_ENUM);
    }
}

GLFUNC(void, glFogf, (GLenum pname, GLfloat value)) { fog(pname, &value); }
GLFUNC(void, glFogfv, (GLenum pname, const GLfloat *value)) { fog(pname, value); }

GLFUNC(void, glFogi, (GLenum pname, GLint value))
{
    float v = (float)value;
    fog(pname, &v);
}

GLFUNC(void, glFogiv, (GLenum pname, const GLint *value))
{
    float v[4] = { (float)value[0] };
    if (pname == GL_FOG_COLOR)
        for (int i = 0; i < 4; i++)
            v[i] = value[i] / 2147483647.0f;
    fog(pname, v);
}

GLFUNC(void, glHint, (GLenum target, GLenum mode)) {}
GLFUNC(void, glLineWidth, (GLfloat width)) {}
GLFUNC(void, glPointSize, (GLfloat size)) {}

static int get_values(GLenum pname, float *out)
{
    struct gl_context *ctx = gl_current;
    int m;

    switch (pname) {
    case 0x0BA2:
        for (int i = 0; i < 4; i++)
            out[i] = (float)ctx->viewport[i];
        return 4;
    case 0x0BA6:
    case 0x0BA7:
    case 0x0BA8:
        m = pname - 0x0BA6;
        memcpy(out, ctx->matrix[m][ctx->matrix_depth[m]], 16 * sizeof(float));
        return 16;
    case 0x0BA3: out[0] = (float)ctx->matrix_depth[0] + 1; return 1;
    case 0x0BA4: out[0] = (float)ctx->matrix_depth[1] + 1; return 1;
    case 0x0BA5: out[0] = (float)ctx->matrix_depth[2] + 1; return 1;
    case 0x0BA0: out[0] = (float)ctx->matrix_mode; return 1;
    case 0x0D36: case 0x0D38: case 0x0D39: out[0] = MATRIX_STACK_DEPTH; return 1;
    case 0x0D33: out[0] = 4096; return 1;
    case 0x84E2: out[0] = MAX_TEX_UNITS; return 1;
    case 0x84E0: out[0] = (float)(GL_TEXTURE0_ARB + ctx->active_texture); return 1;
    case 0x84E1: out[0] = (float)(GL_TEXTURE0_ARB + ctx->client_active_texture); return 1;
    case 0x8069: out[0] = (float)ctx->unit[ctx->active_texture].bound; return 1;
    case 0x0B00: memcpy(out, ctx->color, 4 * sizeof(float)); return 4;
    case 0x0C22: memcpy(out, ctx->clear_color, 4 * sizeof(float)); return 4;
    case 0x0B73: out[0] = ctx->clear_depth; return 1;
    case 0x0B70: out[0] = ctx->depth_range[0]; out[1] = ctx->depth_range[1]; return 2;
    case 0x0B74: out[0] = (float)ctx->depth_func; return 1;
    case 0x0B72: out[0] = (float)ctx->depth_mask; return 1;
    case 0x0BE1: out[0] = (float)ctx->blend_src; return 1;
    case 0x0BE0: out[0] = (float)ctx->blend_dst; return 1;
    case 0x0BC1: out[0] = (float)ctx->alpha_func; return 1;
    case 0x0BC2: out[0] = ctx->alpha_ref; return 1;
    case 0x0B45: out[0] = (float)ctx->cull_mode; return 1;
    case 0x0B46: out[0] = (float)ctx->front_face; return 1;
    case 0x0B54: out[0] = (float)ctx->shade_model; return 1;
    case 0x0C10:
        for (int i = 0; i < 4; i++)
            out[i] = (float)ctx->scissor[i];
        return 4;
    case 0x0C23:
        for (int i = 0; i < 4; i++)
            out[i] = (float)ctx->color_mask[i];
        return 4;
    case GL_FOG_MODE: out[0] = (float)ctx->fog_mode; return 1;
    case GL_FOG_DENSITY: out[0] = ctx->fog_density; return 1;
    case GL_FOG_START: out[0] = ctx->fog_start; return 1;
    case GL_FOG_END: out[0] = ctx->fog_end; return 1;
    case GL_FOG_COLOR: memcpy(out, ctx->fog_color, 4 * sizeof(float)); return 4;
    case 0x0D05: out[0] = (float)ctx->pack_alignment; return 1;
    case 0x0CF5: out[0] = (float)ctx->unpack_alignment; return 1;
    case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: out[0] = 8; return 1;
    case 0x0D56: out[0] = 24; return 1;
    case 0x0D57: out[0] = 8; return 1;
    case 0x0C32: out[0] = 1; return 1;
    case 0x0D3A: out[0] = out[1] = 16384; return 2;
    case 0x8038: out[0] = ctx->polygon_offset[0]; return 1;
    case 0x2A00: out[0] = ctx->polygon_offset[1]; return 1;
    case 0x2200:
        out[0] = (float)ctx->unit[ctx->active_texture].env_mode;
        return 1;
    }
    {
        BOOL *flag = NULL;
        switch (pname) {
        case GL_TEXTURE_2D: case GL_BLEND: case GL_ALPHA_TEST: case GL_DEPTH_TEST:
        case GL_CULL_FACE: case GL_SCISSOR_TEST: case GL_FOG: case GL_POLYGON_OFFSET_FILL:
        case GL_DITHER:
            flag = cap_flag(ctx, pname);
        }
        if (flag) {
            out[0] = (float)*flag;
            return 1;
        }
    }
    return 0;
}

static int query(GLenum pname, float *out)
{
    static GLenum logged[64];
    int n;

    if (!gl_current)
        return 0;
    n = get_values(pname, out);
    if (!n) {
        for (int i = 0; i < 64; i++) {
            if (logged[i] == pname)
                break;
            if (!logged[i]) {
                logged[i] = pname;
                vkgl_log("glGet: unhandled 0x%04x", pname);
                break;
            }
        }
        out[0] = 0;
        gl_set_error(GL_INVALID_ENUM);
        return 1;
    }
    return n;
}

GLFUNC(void, glGetFloatv, (GLenum pname, GLfloat *out))
{
    float v[16];
    int n = query(pname, v);
    memcpy(out, v, n * sizeof(float));
}

GLFUNC(void, glGetDoublev, (GLenum pname, GLdouble *out))
{
    float v[16];
    int n = query(pname, v);
    for (int i = 0; i < n; i++)
        out[i] = v[i];
}

GLFUNC(void, glGetIntegerv, (GLenum pname, GLint *out))
{
    float v[16];
    int n = query(pname, v);
    for (int i = 0; i < n; i++)
        out[i] = (GLint)(v[i] + (v[i] >= 0 ? 0.5f : -0.5f));
}

GLFUNC(void, glGetBooleanv, (GLenum pname, GLboolean *out))
{
    float v[16];
    int n = query(pname, v);
    for (int i = 0; i < n; i++)
        out[i] = v[i] != 0;
}

GLFUNC(void, glGetTexEnviv, (GLenum target, GLenum pname, GLint *out))
{
    if (gl_current && pname == GL_TEXTURE_ENV_MODE)
        *out = (GLint)gl_current->unit[gl_current->active_texture].env_mode;
}
