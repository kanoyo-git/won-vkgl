#include "vkgl.h"
#include <stdlib.h>
#include "engine.h"

#define RVA_CTOR_LINES 0x3d7ad
#define RVA_CTOR_ADDED 0x3d7e1
#define RVA_SET_ROW    0x3db10
#define RVA_APPLY      0x3dd40
#define RVA_ENGINE     0x16decc
#define RVA_OPENGL_STR 0x1563a0
#define RVA_PANEL      0x16ded4
#define RVA_OPEN_OPTIONS 0x2cbe0

#define OFS_OLD       0xb8
#define OFS_SETTINGS  0x148
#define SETTINGS_SIZE 0x90
#define OFS_RENDERER  0x158
#define OFS_COMBO     0x1e0
#define OFS_OPENGL    0x21c
#define OFS_ROW       0x29c
#define ROW_GL        1
#define ROW_VULKAN    3

#define VFUNC(obj, ofs) (*(void **)(*(BYTE **)(obj) + (ofs)))
typedef void (__fastcall *get_text_fn)(void *self, void *edx, char *buf, int size);
typedef int (__fastcall *add_item_fn)(void *self, void *edx, const char *text, void *data);
typedef void (__fastcall *activate_row_fn)(void *self, void *edx, int row);
#define VT_GET_TEXT     0x224
#define VT_ADD_ITEM     0x310
#define VT_ACTIVATE_ROW 0x340

static const char vulkan_label[] = "Vulkan";
static BYTE *gameui;
static void (__fastcall *set_row_orig)(BYTE *self, void *edx);
static void (__fastcall *apply_orig)(BYTE *self, void *edx, int force);

static BYTE *video_page;

static void __cdecl add_vulkan_row(BYTE *self)
{
    void *combo = *(void **)(self + OFS_COMBO);

    video_page = self;

    if (combo)
        ((add_item_fn)VFUNC(combo, VT_ADD_ITEM))(combo, NULL, vulkan_label, NULL);
}

static void __fastcall set_row_hook(BYTE *self, void *edx)
{
    void *combo = *(void **)(self + OFS_COMBO);

    set_row_orig(self, edx);
    if (combo && vkgl_renderer_vulkan() && *(int *)(self + OFS_ROW) == ROW_GL) {
        *(int *)(self + OFS_ROW) = ROW_VULKAN;
        ((activate_row_fn)VFUNC(combo, VT_ACTIVATE_ROW))(combo, NULL, ROW_VULKAN);
    }
}

static void __fastcall apply_hook(BYTE *self, void *edx, int force)
{
    void *combo = *(void **)(self + OFS_COMBO);
    void (*client_cmd)(const char *);
    char text[256] = "";
    BOOL vulkan, gl;

    if (combo)
        ((get_text_fn)VFUNC(combo, VT_GET_TEXT))(combo, NULL, text, sizeof(text));
    vulkan = !strcmp(text, vulkan_label);
    gl = !strcmp(text, (const char *)self + OFS_OPENGL);
    if (vulkan)
        lstrcpyA((char *)self + OFS_RENDERER, "gl");
    apply_orig(self, edx, force);
    if ((!vulkan && !gl) || vulkan == vkgl_renderer_wanted())
        return;
    vkgl_renderer_request(vulkan);

    if ((force & 0xff) || memcmp(self + OFS_SETTINGS, self + OFS_OLD, SETTINGS_SIZE))
        return;
    client_cmd = *(void **)(*(BYTE **)(gameui + RVA_ENGINE) + 0x50);
    if (client_cmd)
        client_cmd("_restart\n");
}

static BYTE *code(int len)
{
    static BYTE *page, *end;
    BYTE *p;

    if (!page || end - page < len) {
        if (!(page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
            return NULL;
        end = page + 4096;
    }
    p = page;
    page += len;
    return p;
}

static void jump(BYTE *site, const void *to)
{
    BYTE jmp[5] = { 0xe9 };

    *(DWORD *)(jmp + 1) = (DWORD)(ULONG_PTR)to - (DWORD)(ULONG_PTR)(site + 5);
    write_code((DWORD)(ULONG_PTR)site, jmp, 5);
}

static void *trampoline(BYTE *site, int len)
{
    BYTE *t = code(len + 5);

    if (!t)
        return NULL;
    memcpy(t, site, len);
    t[len] = 0xe9;
    rel32(t + len + 1, (DWORD)(ULONG_PTR)(site + len));
    FlushInstructionCache(GetCurrentProcess(), t, len + 5);
    return t;
}

static BOOL matches(DWORD rva, const char *bytes, int len)
{
    return readable((DWORD)(ULONG_PTR)(gameui + rva), len) && !memcmp(gameui + rva, bytes, len);
}

void vkgl_menu_patch(void *module)
{
    HMODULE self;
    BYTE *stub, *p, *site;

    gameui = module;
    if (!matches(RVA_CTOR_LINES, "\x6a\x03\x68", 3) ||
        !matches(RVA_CTOR_ADDED, "\x68\x50\x01\x00\x00\xe8", 6) ||
        !matches(RVA_SET_ROW, "\x56\x8b\xf1\x57\x68", 5) ||
        !matches(RVA_SET_ROW + 9, "\x8d\xbe\x58\x01\x00\x00", 6) ||
        !matches(RVA_APPLY, "\x81\xec\x08\x03\x00\x00\x53\x56\x8b\xf1", 10) ||
        !matches(RVA_OPENGL_STR, "#GameUI_OpenGL", 15)) {
        vkgl_log("menu: GameUI.dll is not the expected build, no Vulkan entry");
        return;
    }

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)apply_hook, &self) || !(stub = code(32)))
        return;

    site = gameui + RVA_CTOR_ADDED;
    p = put(stub, "\x60\x55\xb8", 3);
    p = put32(p, (DWORD)(ULONG_PTR)add_vulkan_row);
    p = put(p, "\xff\xd0\x83\xc4\x04\x61\x68\x50\x01\x00\x00\xe9", 12);
    p = rel32(p, (DWORD)(ULONG_PTR)(site + 5));
    FlushInstructionCache(GetCurrentProcess(), stub, (DWORD)(p - stub));
    if (!(set_row_orig = trampoline(gameui + RVA_SET_ROW, 9)) ||
        !(apply_orig = trampoline(gameui + RVA_APPLY, 6)))
        return;
    write_code((DWORD)(ULONG_PTR)(gameui + RVA_CTOR_LINES + 1), "\x04", 1);
    jump(site, stub);
    jump(gameui + RVA_SET_ROW, (void *)set_row_hook);
    jump(gameui + RVA_APPLY, (void *)apply_hook);
    vkgl_log("menu: Vulkan added to the video options' renderer list");
}

void vkgl_menu_test_frame(void)
{
    static int frames, row = -2;
    char buf[8];
    void *combo;
    BYTE *panel;

    if (row == -2)
        row = GetEnvironmentVariableA("VKGL_MENU_TEST", buf, sizeof(buf)) ? atoi(buf) : -1;
    if (row < 0 || !apply_orig)
        return;
    if (++frames == 300 && (panel = *(BYTE **)(gameui + RVA_PANEL))) {
        ((void (__fastcall *)(void *, void *))(gameui + RVA_OPEN_OPTIONS))(panel, NULL);
        vkgl_log("menu test: options open, renderer row %d", video_page ? *(int *)(video_page + OFS_ROW) : -1);
    } else if (frames == 400 && video_page && (combo = *(void **)(video_page + OFS_COMBO))) {
        ((activate_row_fn)VFUNC(combo, VT_ACTIVATE_ROW))(combo, NULL, row);
        vkgl_log("menu test: row %d picked", row);
    } else if (frames == 450 && video_page) {
        vkgl_log("menu test: applying");
        apply_hook(video_page, NULL, 0);
        row = -1;
    }
}
