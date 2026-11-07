#include <stdio.h>
#include <stdlib.h>
#include "vkgl.h"

#define PIXEL_FORMAT_PROP "vkgl_pixel_format"

struct gl_context *gl_current;
static HDC current_dc;
static int context_count;

static void describe(PIXELFORMATDESCRIPTOR *pfd)
{
    memset(pfd, 0, sizeof(*pfd));
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER |
                   PFD_SWAP_EXCHANGE;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->cColorBits = 32;
    pfd->cRedBits = 8;   pfd->cRedShift = 16;
    pfd->cGreenBits = 8; pfd->cGreenShift = 8;
    pfd->cBlueBits = 8;  pfd->cBlueShift = 0;
    pfd->cAlphaBits = 8; pfd->cAlphaShift = 24;
    pfd->cDepthBits = 24;
    pfd->cStencilBits = 8;
    pfd->iLayerType = PFD_MAIN_PLANE;
}

GLFUNC(int, wglChoosePixelFormat, (HDC dc, const PIXELFORMATDESCRIPTOR *pfd))
{
    if (pfd)
        vkgl_log("wglChoosePixelFormat(flags=0x%lx color=%d depth=%d stencil=%d) -> 1",
                 pfd->dwFlags, pfd->cColorBits, pfd->cDepthBits, pfd->cStencilBits);
    return 1;
}

GLFUNC(int, wglDescribePixelFormat, (HDC dc, int fmt, UINT size, PIXELFORMATDESCRIPTOR *pfd))
{
    if (pfd && fmt == 1 && size >= sizeof(*pfd))
        describe(pfd);
    return 1;
}

GLFUNC(BOOL, wglSetPixelFormat, (HDC dc, int fmt, const PIXELFORMATDESCRIPTOR *pfd))
{
    HWND wnd = WindowFromDC(dc);

    if (fmt != 1)
        return FALSE;
    if (wnd)
        SetPropA(wnd, PIXEL_FORMAT_PROP, (HANDLE)1);
    return TRUE;
}

GLFUNC(int, wglGetPixelFormat, (HDC dc))
{
    HWND wnd = WindowFromDC(dc);

    return wnd && GetPropA(wnd, PIXEL_FORMAT_PROP) ? 1 : 0;
}

GLFUNC(HGLRC, wglCreateContext, (HDC dc))
{
    struct gl_context *ctx = calloc(1, sizeof(*ctx));
    HWND wnd = WindowFromDC(dc);

    vkgl_engine_patches();
    vkgl_capture_setup();
    vkgl_mouse_setup();
    if (!ctx || !wnd) {
        vkgl_log("wglCreateContext(%p): no window for this DC", (void *)dc);
        free(ctx);
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    ctx->wnd = wnd;
    ctx->dc = dc;
    gl_init_state(ctx);
    context_count++;
    vkgl_log("wglCreateContext(%p) -> %p, window %p", (void *)dc, (void *)ctx, (void *)wnd);
    return (HGLRC)ctx;
}

GLFUNC(BOOL, wglDeleteContext, (HGLRC rc))
{
    struct gl_context *ctx = (struct gl_context *)rc;

    if (!ctx)
        return FALSE;
    if (ctx == gl_current) {
        vk_wait_idle();
        gl_current = NULL;
        current_dc = NULL;
    }
    vkgl_log("wglDeleteContext(%p)", (void *)ctx);
    free(ctx);
    if (--context_count == 0) {
        vk_release_window();
        vkgl_trace_rearm();
    }
    return TRUE;
}

GLFUNC(BOOL, wglMakeCurrent, (HDC dc, HGLRC rc))
{
    struct gl_context *ctx = (struct gl_context *)rc;

    vkgl_log("wglMakeCurrent(%p, %p)", (void *)dc, (void *)ctx);
    if (!ctx) {
        gl_current = NULL;
        current_dc = NULL;
        return TRUE;
    }
    if (dc) {
        HWND wnd = WindowFromDC(dc);
        if (wnd)
            ctx->wnd = wnd;
        ctx->dc = dc;
    }
    vk_bind_window(ctx->wnd);

    if (!ctx->viewport[2] && !ctx->viewport[3])
        vk_backbuffer_size(&ctx->viewport[2], &ctx->viewport[3]);
    gl_current = ctx;
    current_dc = dc;
    vkgl_profile_thread();
    return TRUE;
}

GLFUNC(HGLRC, wglGetCurrentContext, (void))
{
    return (HGLRC)gl_current;
}

GLFUNC(HDC, wglGetCurrentDC, (void))
{
    return current_dc;
}

static void debug_shot(void)
{
    static char req[MAX_PATH], out[MAX_PATH];
    unsigned char *rgba;
    int w, h;
    FILE *f;

    if (vkgl_frame_count % 30)
        return;
    if (!req[0]) {
        DWORD n = GetModuleFileNameA(vkgl_module, req, MAX_PATH);

        while (n && req[n - 1] != '\\' && req[n - 1] != '/')
            n--;
        req[n] = 0;
        lstrcpynA(out, req, MAX_PATH);
        lstrcatA(req, "vkgl_shot.req");
        lstrcatA(out, "vkgl_shot.ppm");
    }
    if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES)
        return;
    DeleteFileA(req);
    vk_backbuffer_size(&w, &h);
    if (w <= 0 || h <= 0 || !(rgba = malloc((size_t)w * h * 4)))
        return;
    if (vk_read_rgba(0, 0, w, h, rgba) && (f = fopen(out, "wb"))) {
        fprintf(f, "P6 %d %d 255\n", w, h);
        for (int y = h - 1; y >= 0; y--)
            for (int x = 0; x < w; x++)
                fwrite(rgba + ((size_t)y * w + x) * 4, 1, 3, f);
        fclose(f);
    }
    free(rgba);
}

GLFUNC(BOOL, wglSwapBuffers, (HDC dc))
{
    static int orphan;

    if (!gl_current) {
        if (orphan++ < 3)
            vkgl_log("wglSwapBuffers(%p) without a current context", (void *)dc);
        return FALSE;
    }
    debug_shot();
    vkgl_mouse_frame();
    vk_present();
    vkgl_capture_frame(InterlockedIncrement(&vkgl_frame_count));
    return TRUE;
}

GLFUNC(BOOL, wglSwapLayerBuffers, (HDC dc, UINT planes))
{
    return (planes & WGL_SWAP_MAIN_PLANE) ? impl_wglSwapBuffers(dc) : TRUE;
}

GLFUNC(BOOL, wglShareLists, (HGLRC a, HGLRC b))
{
    return TRUE;
}

static const char wgl_extensions[] =
    "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_EXT_swap_control";

static const char *WINAPI wgl_get_extensions_arb(HDC dc)
{
    return wgl_extensions;
}

static const char *WINAPI wgl_get_extensions_ext(void)
{
    return wgl_extensions;
}

static BOOL WINAPI wgl_swap_interval(int interval)
{
    if (interval < 0) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    vk_set_swap_interval(interval);
    return TRUE;
}

static int WINAPI wgl_get_swap_interval(void)
{
    return vk_swap_interval();
}

static const struct { const char *name; PROC proc; } extension_procs[] = {
    { "wglGetExtensionsStringARB", (PROC)wgl_get_extensions_arb },
    { "wglGetExtensionsStringEXT", (PROC)wgl_get_extensions_ext },
    { "wglSwapIntervalEXT", (PROC)wgl_swap_interval },
    { "wglGetSwapIntervalEXT", (PROC)wgl_get_swap_interval },
};

GLFUNC(PROC, wglGetProcAddress, (LPCSTR name))
{
    PROC p = NULL;

    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(extension_procs) / sizeof(extension_procs[0]); i++)
        if (!strcmp(name, extension_procs[i].name))
            p = extension_procs[i].proc;
    if (!p && !strncmp(name, "gl", 2))
        p = gl_get_extension_proc(name);
    if (!p && !strncmp(name, "gl", 2))
        p = GetProcAddress(vkgl_module, name);
    vkgl_log("wglGetProcAddress(\"%s\") -> %s", name, p ? "ok" : "NULL");
    return p;
}
