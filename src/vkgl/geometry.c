#include <math.h>
#include <stdlib.h>
#include "vkgl.h"

#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_LINE_LOOP      0x0002
#define GL_LINE_STRIP     0x0003
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006
#define GL_QUADS          0x0007
#define GL_QUAD_STRIP     0x0008
#define GL_POLYGON        0x0009
#define GL_MODELVIEW      0x1700
#define GL_PROJECTION     0x1701
#define GL_TEXTURE        0x1702
#define GL_BYTE           0x1400
#define GL_UNSIGNED_BYTE  0x1401
#define GL_SHORT          0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT            0x1404
#define GL_UNSIGNED_INT   0x1405
#define GL_FLOAT          0x1406
#define GL_DOUBLE         0x140A
#define GL_VERTEX_ARRAY   0x8074
#define GL_NORMAL_ARRAY   0x8075
#define GL_COLOR_ARRAY    0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_TEXTURE0_ARB   0x84C0
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW    0x0503
#define GL_STACK_UNDERFLOW   0x0504

static struct vertex *verts, *expanded;
static uint32_t vert_count, vert_cap, expanded_cap;

static void reserve(struct vertex **buf, uint32_t *cap, uint32_t n)
{
    if (n <= *cap)
        return;
    *cap = n < 256 ? 256 : n * 2;
    *buf = realloc(*buf, *cap * sizeof(**buf));
    if (!*buf)
        vkgl_fatal("out of memory for %u vertices", n);
}

static unsigned char to_ubyte(float f)
{
    return f <= 0 ? 0 : f >= 1 ? 255 : (unsigned char)(f * 255.0f + 0.5f);
}

static void set_color(float r, float g, float b, float a)
{
    struct gl_context *ctx = gl_current;

    if (!ctx)
        return;
    ctx->color[0] = r; ctx->color[1] = g; ctx->color[2] = b; ctx->color[3] = a;
    ctx->color_ub[0] = to_ubyte(r);
    ctx->color_ub[1] = to_ubyte(g);
    ctx->color_ub[2] = to_ubyte(b);
    ctx->color_ub[3] = to_ubyte(a);
}

static void set_color_ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    struct gl_context *ctx = gl_current;

    if (!ctx)
        return;
    ctx->color[0] = r / 255.0f; ctx->color[1] = g / 255.0f;
    ctx->color[2] = b / 255.0f; ctx->color[3] = a / 255.0f;
    ctx->color_ub[0] = r; ctx->color_ub[1] = g; ctx->color_ub[2] = b; ctx->color_ub[3] = a;
}

static inline void emit(float x, float y, float z)
{
    struct gl_context *ctx = gl_current;
    struct vertex *v;

    if (!ctx || !ctx->in_begin)
        return;
    reserve(&verts, &vert_cap, vert_count + 1);
    v = &verts[vert_count++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    memcpy(v->color, ctx->color_ub, 4);
    v->uv[0][0] = ctx->unit[0].texcoord[0];
    v->uv[0][1] = ctx->unit[0].texcoord[1];
    v->uv[1][0] = ctx->unit[1].texcoord[0];
    v->uv[1][1] = ctx->unit[1].texcoord[1];
}

static void draw_primitive(GLenum mode, const struct vertex *v, uint32_t n)
{
    struct vertex *o;
    uint32_t m = 0;
    enum prim_kind kind = PRIM_TRIANGLES;

    reserve(&expanded, &expanded_cap, n * 3 + 6);
    o = expanded;
    switch (mode) {
    case GL_TRIANGLES:
        m = n - n % 3;
        memcpy(o, v, m * sizeof(*v));
        break;
    case GL_TRIANGLE_STRIP:

        for (uint32_t i = 2; i < n; i++) {
            o[m++] = v[i & 1 ? i - 1 : i - 2];
            o[m++] = v[i & 1 ? i - 2 : i - 1];
            o[m++] = v[i];
        }
        break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        for (uint32_t i = 2; i < n; i++) {
            o[m++] = v[0];
            o[m++] = v[i - 1];
            o[m++] = v[i];
        }
        break;
    case GL_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            o[m++] = v[i]; o[m++] = v[i + 1]; o[m++] = v[i + 2];
            o[m++] = v[i]; o[m++] = v[i + 2]; o[m++] = v[i + 3];
        }
        break;
    case GL_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            o[m++] = v[i]; o[m++] = v[i + 1]; o[m++] = v[i + 3];
            o[m++] = v[i]; o[m++] = v[i + 3]; o[m++] = v[i + 2];
        }
        break;
    case GL_LINES:
        kind = PRIM_LINES;
        m = n & ~1u;
        memcpy(o, v, m * sizeof(*v));
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        kind = PRIM_LINES;
        for (uint32_t i = 1; i < n; i++) {
            o[m++] = v[i - 1];
            o[m++] = v[i];
        }
        if (mode == GL_LINE_LOOP && n > 2) {
            o[m++] = v[n - 1];
            o[m++] = v[0];
        }
        break;
    case GL_POINTS:
        kind = PRIM_POINTS;
        m = n;
        memcpy(o, v, m * sizeof(*v));
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    render_draw(kind, o, m);
}

GLFUNC(void, glBegin, (GLenum mode))
{
    if (!gl_current)
        return;
    if (gl_current->in_begin || mode > GL_POLYGON) {
        gl_set_error(gl_current->in_begin ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
        return;
    }
    gl_current->in_begin = TRUE;
    gl_current->begin_mode = mode;
    vert_count = 0;
}

GLFUNC(void, glEnd, (void))
{
    if (!gl_current)
        return;
    if (!gl_current->in_begin) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    gl_current->in_begin = FALSE;
    draw_primitive(gl_current->begin_mode, verts, vert_count);
}

GLFUNC(void, glVertex2f, (GLfloat x, GLfloat y)) { emit(x, y, 0); }
GLFUNC(void, glVertex2fv, (const GLfloat *v)) { emit(v[0], v[1], 0); }
GLFUNC(void, glVertex2i, (GLint x, GLint y)) { emit((float)x, (float)y, 0); }
GLFUNC(void, glVertex2d, (GLdouble x, GLdouble y)) { emit((float)x, (float)y, 0); }
GLFUNC(void, glVertex3f, (GLfloat x, GLfloat y, GLfloat z)) { emit(x, y, z); }
GLFUNC(void, glVertex3fv, (const GLfloat *v)) { emit(v[0], v[1], v[2]); }
GLFUNC(void, glVertex3d, (GLdouble x, GLdouble y, GLdouble z)) { emit((float)x, (float)y, (float)z); }
GLFUNC(void, glVertex3dv, (const GLdouble *v)) { emit((float)v[0], (float)v[1], (float)v[2]); }
GLFUNC(void, glVertex3i, (GLint x, GLint y, GLint z)) { emit((float)x, (float)y, (float)z); }
GLFUNC(void, glVertex4f, (GLfloat x, GLfloat y, GLfloat z, GLfloat w))
{
    if (w != 0 && w != 1)
        emit(x / w, y / w, z / w);
    else
        emit(x, y, z);
}

GLFUNC(void, glColor3f, (GLfloat r, GLfloat g, GLfloat b)) { set_color(r, g, b, 1); }
GLFUNC(void, glColor3fv, (const GLfloat *v)) { set_color(v[0], v[1], v[2], 1); }
GLFUNC(void, glColor4f, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) { set_color(r, g, b, a); }
GLFUNC(void, glColor4fv, (const GLfloat *v)) { set_color(v[0], v[1], v[2], v[3]); }
GLFUNC(void, glColor3d, (GLdouble r, GLdouble g, GLdouble b)) { set_color((float)r, (float)g, (float)b, 1); }
GLFUNC(void, glColor3ub, (GLubyte r, GLubyte g, GLubyte b)) { set_color_ub(r, g, b, 255); }
GLFUNC(void, glColor3ubv, (const GLubyte *v)) { set_color_ub(v[0], v[1], v[2], 255); }
GLFUNC(void, glColor4ub, (GLubyte r, GLubyte g, GLubyte b, GLubyte a)) { set_color_ub(r, g, b, a); }
GLFUNC(void, glColor4ubv, (const GLubyte *v)) { set_color_ub(v[0], v[1], v[2], v[3]); }

static void set_texcoord(int unit, float s, float t)
{
    if (!gl_current || unit < 0 || unit >= MAX_TEX_UNITS)
        return;
    gl_current->unit[unit].texcoord[0] = s;
    gl_current->unit[unit].texcoord[1] = t;
}

GLFUNC(void, glTexCoord2f, (GLfloat s, GLfloat t)) { set_texcoord(0, s, t); }
GLFUNC(void, glTexCoord2fv, (const GLfloat *v)) { set_texcoord(0, v[0], v[1]); }
GLFUNC(void, glTexCoord2d, (GLdouble s, GLdouble t)) { set_texcoord(0, (float)s, (float)t); }
GLFUNC(void, glTexCoord2i, (GLint s, GLint t)) { set_texcoord(0, (float)s, (float)t); }
GLFUNC(void, glTexCoord1f, (GLfloat s)) { set_texcoord(0, s, 0); }

GLFUNC(void, glNormal3f, (GLfloat x, GLfloat y, GLfloat z)) {}
GLFUNC(void, glNormal3fv, (const GLfloat *v)) {}

static void read_array(const struct gl_array *a, int index, float *out, int n, BOOL normalize)
{
    int size = a->size, comp;
    const unsigned char *p;

    switch (a->type) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: comp = 4; break;
    case GL_DOUBLE: comp = 8; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: comp = 2; break;
    default: comp = 1;
    }
    p = a->ptr + (size_t)index * (a->stride ? a->stride : size * comp);
    for (int i = 0; i < n; i++) {
        float v, scale = 1;
        if (i >= size) {
            out[i] = i == 3 ? 1.0f : 0.0f;
            continue;
        }
        switch (a->type) {
        case GL_FLOAT: out[i] = ((const float *)p)[i]; continue;
        case GL_DOUBLE: out[i] = (float)((const double *)p)[i]; continue;
        case GL_BYTE: v = ((const signed char *)p)[i]; scale = 127.0f; break;
        case GL_UNSIGNED_BYTE: v = p[i]; scale = 255.0f; break;
        case GL_SHORT: v = ((const short *)p)[i]; scale = 32767.0f; break;
        case GL_UNSIGNED_SHORT: v = ((const unsigned short *)p)[i]; scale = 65535.0f; break;
        case GL_INT: v = (float)((const int *)p)[i]; scale = 2147483647.0f; break;
        case GL_UNSIGNED_INT: v = (float)((const unsigned *)p)[i]; scale = 4294967295.0f; break;
        default: v = 0;
        }
        out[i] = normalize ? v / scale : v;
    }
}

static void array_element(int i)
{
    struct gl_context *ctx = gl_current;
    float f[4];

    if (ctx->color_array.enabled && ctx->color_array.ptr) {
        if (ctx->color_array.type == GL_UNSIGNED_BYTE) {
            const unsigned char *p = ctx->color_array.ptr + (size_t)i *
                (ctx->color_array.stride ? ctx->color_array.stride : ctx->color_array.size);
            set_color_ub(p[0], p[1], p[2], ctx->color_array.size > 3 ? p[3] : 255);
        } else {
            read_array(&ctx->color_array, i, f, 4, TRUE);
            set_color(f[0], f[1], f[2], f[3]);
        }
    }
    for (int u = 0; u < MAX_TEX_UNITS; u++)
        if (ctx->texcoord_array[u].enabled && ctx->texcoord_array[u].ptr) {
            read_array(&ctx->texcoord_array[u], i, f, 2, FALSE);
            set_texcoord(u, f[0], f[1]);
        }
    if (ctx->vertex_array.enabled && ctx->vertex_array.ptr) {
        read_array(&ctx->vertex_array, i, f, 3, FALSE);
        emit(f[0], f[1], f[2]);
    }
}

static void set_array(struct gl_array *a, GLint size, GLenum type, GLsizei stride,
                      const GLvoid *ptr)
{
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->ptr = ptr;
}

GLFUNC(void, glVertexPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr))
{
    if (gl_current)
        set_array(&gl_current->vertex_array, size, type, stride, ptr);
}

GLFUNC(void, glColorPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr))
{
    if (gl_current)
        set_array(&gl_current->color_array, size, type, stride, ptr);
}

GLFUNC(void, glTexCoordPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr))
{
    if (gl_current)
        set_array(&gl_current->texcoord_array[gl_current->client_active_texture],
                  size, type, stride, ptr);
}

GLFUNC(void, glNormalPointer, (GLenum type, GLsizei stride, const GLvoid *ptr)) {}

static struct gl_array *client_state(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY: return &gl_current->vertex_array;
    case GL_COLOR_ARRAY: return &gl_current->color_array;
    case GL_TEXTURE_COORD_ARRAY:
        return &gl_current->texcoord_array[gl_current->client_active_texture];
    }
    return NULL;
}

GLFUNC(void, glEnableClientState, (GLenum cap))
{
    struct gl_array *a = gl_current ? client_state(cap) : NULL;
    if (a)
        a->enabled = TRUE;
}

GLFUNC(void, glDisableClientState, (GLenum cap))
{
    struct gl_array *a = gl_current ? client_state(cap) : NULL;
    if (a)
        a->enabled = FALSE;
}

GLFUNC(void, glArrayElement, (GLint i))
{
    if (gl_current && gl_current->in_begin)
        array_element(i);
}

GLFUNC(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))
{
    if (!gl_current || count <= 0)
        return;
    if (gl_current->in_begin || first < 0) {
        gl_set_error(gl_current->in_begin ? GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    impl_glBegin(mode);
    for (int i = 0; i < count; i++)
        array_element(first + i);
    impl_glEnd();
}

GLFUNC(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices))
{
    if (!gl_current || count <= 0)
        return;
    if (gl_current->in_begin) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    impl_glBegin(mode);
    for (int i = 0; i < count; i++) {
        switch (type) {
        case GL_UNSIGNED_BYTE: array_element(((const unsigned char *)indices)[i]); break;
        case GL_UNSIGNED_SHORT: array_element(((const unsigned short *)indices)[i]); break;
        default: array_element(((const unsigned int *)indices)[i]); break;
        }
    }
    impl_glEnd();
}

static void WINAPI active_texture(GLenum unit)
{
    if (!gl_current)
        return;
    if (unit < GL_TEXTURE0_ARB || unit >= GL_TEXTURE0_ARB + MAX_TEX_UNITS) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    gl_current->active_texture = unit - GL_TEXTURE0_ARB;
}

static void WINAPI client_active_texture(GLenum unit)
{
    if (!gl_current)
        return;
    if (unit < GL_TEXTURE0_ARB || unit >= GL_TEXTURE0_ARB + MAX_TEX_UNITS) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    gl_current->client_active_texture = unit - GL_TEXTURE0_ARB;
}

static void WINAPI multi_texcoord2f(GLenum unit, GLfloat s, GLfloat t)
{
    set_texcoord(unit - GL_TEXTURE0_ARB, s, t);
}

static void WINAPI multi_texcoord2fv(GLenum unit, const GLfloat *v)
{
    set_texcoord(unit - GL_TEXTURE0_ARB, v[0], v[1]);
}

PROC gl_get_extension_proc(const char *name)
{
    static const struct { const char *name; PROC proc; } procs[] = {
        { "glActiveTextureARB", (PROC)active_texture },
        { "glClientActiveTextureARB", (PROC)client_active_texture },
        { "glMultiTexCoord2fARB", (PROC)multi_texcoord2f },
        { "glMultiTexCoord2fvARB", (PROC)multi_texcoord2fv },
    };

    static const char *const texture_object[] = {
        "glBindTextureEXT", "glGenTexturesEXT", "glDeleteTexturesEXT", "glIsTextureEXT",
        "glAreTexturesResidentEXT", "glPrioritizeTexturesEXT",
    };
    char core[64];
    size_t len;

    for (size_t i = 0; i < sizeof(procs) / sizeof(procs[0]); i++)
        if (!strcmp(name, procs[i].name))
            return procs[i].proc;
    for (size_t i = 0; i < sizeof(texture_object) / sizeof(texture_object[0]); i++)
        if (!strcmp(name, texture_object[i])) {
            len = strlen(name) - 3;
            memcpy(core, name, len);
            core[len] = 0;
            return GetProcAddress(vkgl_module, core);
        }
    return NULL;
}

static int matrix_index(GLenum mode)
{
    return mode == GL_PROJECTION ? 1 : mode == GL_TEXTURE ? 2 : 0;
}

static float *top(void)
{
    int m = matrix_index(gl_current->matrix_mode);
    return gl_current->matrix[m][gl_current->matrix_depth[m]];
}

static void touched(void)
{
    if (gl_current->matrix_mode != GL_TEXTURE)
        gl_current->mvp_dirty = TRUE;
}

static void mat_mul(float *r, const float *a, const float *b)
{
    float t[16];

    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            t[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] +
                             a[8 + row] * b[c * 4 + 2] + a[12 + row] * b[c * 4 + 3];
    memcpy(r, t, sizeof(t));
}

static void mult(const float *m)
{
    if (!gl_current)
        return;
    mat_mul(top(), top(), m);
    touched();
}

const float *gl_mvp(struct gl_context *ctx)
{
    if (ctx->mvp_dirty) {
        mat_mul(ctx->mvp, ctx->matrix[1][ctx->matrix_depth[1]],
                ctx->matrix[0][ctx->matrix_depth[0]]);
        ctx->mvp_dirty = FALSE;
    }
    return ctx->mvp;
}

static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

GLFUNC(void, glMatrixMode, (GLenum mode))
{
    if (!gl_current)
        return;
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    gl_current->matrix_mode = mode;
}

GLFUNC(void, glLoadIdentity, (void))
{
    if (!gl_current)
        return;
    memcpy(top(), identity, sizeof(identity));
    touched();
}

GLFUNC(void, glLoadMatrixf, (const GLfloat *m))
{
    if (!gl_current)
        return;
    memcpy(top(), m, 16 * sizeof(float));
    touched();
}

GLFUNC(void, glLoadMatrixd, (const GLdouble *m))
{
    float f[16];

    if (!gl_current)
        return;
    for (int i = 0; i < 16; i++)
        f[i] = (float)m[i];
    memcpy(top(), f, sizeof(f));
    touched();
}

GLFUNC(void, glMultMatrixf, (const GLfloat *m)) { mult(m); }

GLFUNC(void, glMultMatrixd, (const GLdouble *m))
{
    float f[16];

    for (int i = 0; i < 16; i++)
        f[i] = (float)m[i];
    mult(f);
}

GLFUNC(void, glPushMatrix, (void))
{
    int m;

    if (!gl_current)
        return;
    m = matrix_index(gl_current->matrix_mode);
    if (gl_current->matrix_depth[m] + 1 >= MATRIX_STACK_DEPTH) {
        gl_set_error(GL_STACK_OVERFLOW);
        return;
    }
    memcpy(gl_current->matrix[m][gl_current->matrix_depth[m] + 1],
           gl_current->matrix[m][gl_current->matrix_depth[m]], 16 * sizeof(float));
    gl_current->matrix_depth[m]++;
}

GLFUNC(void, glPopMatrix, (void))
{
    int m;

    if (!gl_current)
        return;
    m = matrix_index(gl_current->matrix_mode);
    if (!gl_current->matrix_depth[m]) {
        gl_set_error(GL_STACK_UNDERFLOW);
        return;
    }
    gl_current->matrix_depth[m]--;
    touched();
}

GLFUNC(void, glTranslatef, (GLfloat x, GLfloat y, GLfloat z))
{
    float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };
    mult(m);
}

GLFUNC(void, glTranslated, (GLdouble x, GLdouble y, GLdouble z))
{
    impl_glTranslatef((float)x, (float)y, (float)z);
}

GLFUNC(void, glScalef, (GLfloat x, GLfloat y, GLfloat z))
{
    float m[16] = { x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1 };
    mult(m);
}

GLFUNC(void, glScaled, (GLdouble x, GLdouble y, GLdouble z))
{
    impl_glScalef((float)x, (float)y, (float)z);
}

GLFUNC(void, glRotatef, (GLfloat angle, GLfloat x, GLfloat y, GLfloat z))
{
    float len = sqrtf(x * x + y * y + z * z), a = angle * 3.14159265358979f / 180.0f;
    float c = cosf(a), s = sinf(a), ic = 1 - c, m[16];

    if (len == 0)
        return;
    x /= len; y /= len; z /= len;
    m[0] = x * x * ic + c;     m[4] = x * y * ic - z * s; m[8] = x * z * ic + y * s;  m[12] = 0;
    m[1] = y * x * ic + z * s; m[5] = y * y * ic + c;     m[9] = y * z * ic - x * s;  m[13] = 0;
    m[2] = x * z * ic - y * s; m[6] = y * z * ic + x * s; m[10] = z * z * ic + c;     m[14] = 0;
    m[3] = 0;                  m[7] = 0;                  m[11] = 0;                  m[15] = 1;
    mult(m);
}

GLFUNC(void, glRotated, (GLdouble angle, GLdouble x, GLdouble y, GLdouble z))
{
    impl_glRotatef((float)angle, (float)x, (float)y, (float)z);
}

GLFUNC(void, glOrtho, (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f))
{
    float m[16] = { 0 };

    if (l == r || b == t || n == f) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    m[0] = (float)(2 / (r - l));
    m[5] = (float)(2 / (t - b));
    m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    m[15] = 1;
    mult(m);
}

GLFUNC(void, glFrustum, (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f))
{
    float m[16] = { 0 };

    if (n <= 0 || f <= 0 || l == r || b == t || n == f) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    m[0] = (float)(2 * n / (r - l));
    m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l));
    m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n));
    m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    mult(m);
}
