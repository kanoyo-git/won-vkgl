#include "vkgl.h"
#include <tlhelp32.h>

BOOL WINAPI impl_wglSwapBuffers(HDC dc) __asm__("_impl_wglSwapBuffers");

static LONG direct_frames;
static void *gdi_swap, *export_swap;

static void module_name(const void *addr, char *out, DWORD size)
{
    HMODULE mod;
    DWORD n;

    out[0] = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, addr, &mod) ||
        !(n = GetModuleFileNameA(mod, out, size))) {
        lstrcpynA(out, "(not a module)", size);
        return;
    }
    while (n && out[n - 1] != '\\' && out[n - 1] != '/')
        n--;
    memmove(out, out + n, lstrlenA(out + n) + 1);
}

static const BYTE *hook_target(const BYTE *f)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!f || !VirtualQuery(f, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return NULL;
    if (f[0] == 0xe9)
        return f + 5 + *(const INT32 *)(f + 1);
    if (f[0] == 0xff && f[1] == 0x25)
        return **(const BYTE *const *const *)(f + 2);
    if (f[0] == 0x68 && f[5] == 0xc3)
        return *(const BYTE *const *)(f + 1);
    if (f[0] == 0x8b && f[1] == 0xff && f[2] == 0xe9)
        return f + 7 + *(const INT32 *)(f + 3);
    return NULL;
}

static void report_hook(const char *what, const void *f)
{
    char self[MAX_PATH], by[MAX_PATH];
    const BYTE *to = hook_target(f);

    if (!to)
        return;
    module_name(f, self, sizeof(self));
    module_name(to, by, sizeof(by));
    if (lstrcmpiA(self, by))
        vkgl_log("capture: %s is hooked, jumps into %s", what, by);
}

static BOOL WINAPI direct_swap(HDC dc)
{
    LONG n = InterlockedIncrement(&direct_frames);

    if (n == 600)
        vkgl_log("capture: 600 frames presented directly");

    if (!vkgl_renderer_vulkan())
        return ((BOOL (WINAPI *)(HDC))gdi_swap)(dc);
    return impl_wglSwapBuffers(dc);
}

static int redirect_imports(BYTE *lo, BYTE *hi)
{
    int found = 0;

    for (BYTE *p = lo; p < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        BYTE *end;

        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            break;
        end = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
        if (end > hi)
            end = hi;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_EXECUTE)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_READ))) {
            for (void **slot = (void **)p; (BYTE *)(slot + 1) <= end; slot++) {
                DWORD old;
                void *to = (void *)direct_swap;

                if (*slot != gdi_swap && *slot != export_swap)
                    continue;
                if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
                    *slot = to;
                    VirtualProtect(slot, sizeof(*slot), old, &old);
                    found++;
                }
            }
        }
        p = end;
    }
    return found;
}

void vkgl_capture_setup(void)
{
    static BOOL done;
    BYTE *base;
    SIZE_T size;
    int n;

    if (done)
        return;
    done = TRUE;
    gdi_swap = (void *)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SwapBuffers");
    export_swap = (void *)GetProcAddress(vkgl_module, "wglSwapBuffers");
    report_hook("gdi32 SwapBuffers", gdi_swap);
    report_hook("opengl32 wglSwapBuffers", export_swap);
    if (!vkgl_ini_int("capture", "direct_present", 1))
        return;
    if (!vkgl_engine_image(&base, &size)) {
        vkgl_log("capture: hw.dll is not the expected build, SwapBuffers left to the hooks");
        return;
    }
    n = redirect_imports(base, base + size);
    vkgl_log("capture: %s", n ? "engine presents directly, past GL-level hooks"
                              : "no SwapBuffers import found in hw.dll");
}

static int scan_modules(void)
{
    static const char *const known[] = {
        "graphics-hook", "discord", "overlay", "rtss", "obs", "vkcapture", "mangohud",
        "nvspcap", "nvcamera", "fraps", "bandicam", "action_x86", "medal",
    };
    static char seen[32][MAX_MODULE_NAME32 + 1];
    static int nseen;
    MODULEENTRY32 me = { .dwSize = sizeof(me) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    int fresh = 0;

    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        char lower[MAX_MODULE_NAME32 + 1];
        int i, k;

        lstrcpynA(lower, me.szModule, sizeof(lower));
        CharLowerA(lower);
        for (k = 0; k < (int)(sizeof(known) / sizeof(known[0])) && !strstr(lower, known[k]); k++)
            ;
        if (k == (int)(sizeof(known) / sizeof(known[0])))
            continue;
        for (i = 0; i < nseen && strcmp(seen[i], lower); i++)
            ;
        if (i < nseen || nseen == 32)
            continue;
        lstrcpyA(seen[nseen++], lower);
        vkgl_log("capture: %s loaded (%s)", me.szModule, me.szExePath);
        fresh++;
    }
    CloseHandle(snap);
    return fresh;
}

void vkgl_capture_frame(LONG frame)
{
    static const char *const vk_funcs[] = {
        "vkQueuePresentKHR", "vkCreateSwapchainKHR", "vkAcquireNextImageKHR",
        "vkGetDeviceProcAddr", "vkCreateDevice",
    };
    char what[64];

    if (frame != 600 && frame != 6000 && frame != 60000)
        return;
    if (scan_modules()) {
        vkgl_log("capture: a capture module came in after the swapchain, making it anew");
        vkgl_swapchain_renew();
    }
    report_hook("gdi32 SwapBuffers", gdi_swap);
    report_hook("opengl32 wglSwapBuffers", export_swap);
    for (unsigned i = 0; i < sizeof(vk_funcs) / sizeof(vk_funcs[0]); i++) {
        wsprintfA(what, "vulkan-1 %s", vk_funcs[i]);
        report_hook(what, vkgl_vk_export(vk_funcs[i]));
    }
    report_hook("the driver's vkQueuePresentKHR", vkgl_vk_driver_present());
}

void vkgl_capture_report(PFN_vkGetInstanceProcAddr gipa)
{
    PFN_vkEnumerateInstanceLayerProperties enum_layers =
        (PFN_vkEnumerateInstanceLayerProperties)gipa(NULL, "vkEnumerateInstanceLayerProperties");
    VkLayerProperties props[32];
    uint32_t count = 32;
    char line[1024];
    int len = 0;

    if (enum_layers && enum_layers(&count, props) >= 0) {
        line[0] = 0;
        for (uint32_t i = 0; i < count && len < (int)sizeof(line) - 80; i++)
            len += wsprintfA(line + len, "%s%s", i ? ", " : "", props[i].layerName);
        vkgl_log("vulkan layers: %s", count ? line : "none");
    }
    scan_modules();
}
