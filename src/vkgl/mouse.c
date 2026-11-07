#include <stdio.h>
#include <stdlib.h>
#include "vkgl.h"
#include "engine.h"

#define IDLE_MS 500
#define WARP_MS 250
#define WARP_COUNTS 150
#define CHECK_FRAMES 128

static BOOL(WINAPI *real_get)(POINT *);
static BOOL(WINAPI *real_set)(int, int);
static BOOL(WINAPI *real_ui_get)(POINT *);
static void **slot_get, **slot_set, **slot_ui_get;
static BYTE *patched, *ui_patched;
static BOOL enabled, stopped;
static BOOL virtual_only;

static volatile LONG raw_dx, raw_dy, raw_events;
static POINT base;
static BOOL base_set;
static LONG virt_x, virt_y;
static LONG drift;
static DWORD last_call, last_warp;
static unsigned reads, warps;

static BOOL WINAPI raw_get(POINT *pt)
{
    DWORD now = GetTickCount();
    LONG dx, dy;

    if (!raw_events || !pt || !base_set)
        return real_get(pt);
    reads++;
    dx = InterlockedExchange(&raw_dx, 0);
    dy = InterlockedExchange(&raw_dy, 0);
    if (now - last_call > IDLE_MS) {

        virt_x = virt_y = drift = 0;
        dx = dy = 0;
        if (!virtual_only)
            real_set(base.x, base.y);
        last_warp = now;
    }
    last_call = now;
    virt_x += dx;
    virt_y += dy;
    drift += abs(dx) + abs(dy);
    pt->x = base.x + virt_x;
    pt->y = base.y + virt_y;
    return TRUE;
}

void vkgl_mouse_feed(LONG dx, LONG dy)
{
    InterlockedExchangeAdd(&raw_dx, dx);
    InterlockedExchangeAdd(&raw_dy, dy);
}

static BOOL WINAPI raw_set(int x, int y)
{
    DWORD now = GetTickCount();

    base.x = x;
    base.y = y;
    base_set = TRUE;
    virt_x = virt_y = 0;
    if (!raw_events)
        return real_set(x, y);
    last_call = now;
    if (virtual_only || (drift < WARP_COUNTS && now - last_warp < WARP_MS))
        return TRUE;
    drift = 0;
    last_warp = now;
    warps++;
    return real_set(x, y);
}

static BOOL WINAPI ui_get(POINT *pt)
{
    if (raw_events && pt && GetTickCount() - last_call < 100) {
        *pt = base;
        return TRUE;
    }
    return real_ui_get(pt);
}

static LRESULT CALLBACK raw_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        RAWINPUT ri;
        UINT size = sizeof(ri);

        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
            (ri.data.mouse.lLastX || ri.data.mouse.lLastY)) {
            InterlockedExchangeAdd(&raw_dx, ri.data.mouse.lLastX);
            InterlockedExchangeAdd(&raw_dy, ri.data.mouse.lLastY);
            if (InterlockedIncrement(&raw_events) == 1)
                vkgl_log("mouse: raw input arriving, client.dll cursor calls answered from it");
        }
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static DWORD WINAPI raw_thread(void *arg)
{
    WNDCLASSA wc = { .lpfnWndProc = raw_wndproc, .hInstance = vkgl_module, .lpszClassName = "vkgl_raw_mouse" };
    RAWINPUTDEVICE dev = { .usUsagePage = 1, .usUsage = 2, .dwFlags = RIDEV_INPUTSINK };
    MSG msg;

    (void)arg;
    RegisterClassA(&wc);
    dev.hwndTarget = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, vkgl_module, NULL);
    if (!dev.hwndTarget || !RegisterRawInputDevices(&dev, 1, sizeof(dev))) {
        vkgl_log("mouse: raw input not available (error %lu), cursor calls left alone", GetLastError());
        return 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (GetMessageA(&msg, NULL, 0, 0) > 0)
        DispatchMessageA(&msg);
    return 0;
}

static void **import_slot(BYTE *mod, const char *dll, const char *fn)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(mod + ((IMAGE_DOS_HEADER *)mod)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (!dir->VirtualAddress)
        return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(mod + dir->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *iat = (void *)(mod + d->FirstThunk);

        if (lstrcmpiA((char *)(mod + d->Name), dll) || !d->OriginalFirstThunk)
            continue;
        names = (void *)(mod + d->OriginalFirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; i++)
            if (!IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal) &&
                !strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(mod + names[i].u1.AddressOfData))->Name, fn))
                return (void **)&iat[i].u1.Function;
    }
    return NULL;
}

static void put_slot(void **slot, void *fn)
{
    DWORD old;

    if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        *slot = fn;
        VirtualProtect(slot, sizeof(*slot), old, &old);
    }
}

static void redirect(BYTE *mod)
{
    void **g = import_slot(mod, "user32.dll", "GetCursorPos");
    void **s = import_slot(mod, "user32.dll", "SetCursorPos");

    patched = mod;
    slot_get = slot_set = NULL;
    if (!g || !s) {
        vkgl_log("mouse: client.dll does not import GetCursorPos/SetCursorPos, raw_mouse off for it");
        return;
    }
    if (*g != (void *)raw_get)
        real_get = *g;
    if (*s != (void *)raw_set)
        real_set = *s;
    put_slot(g, (void *)raw_get);
    put_slot(s, (void *)raw_set);
    slot_get = g;
    slot_set = s;
    vkgl_log("mouse: client.dll %p cursor imports redirected", (void *)mod);
}

void vkgl_mouse_frame(void)
{
    static unsigned frame;
    BYTE *mod;

    if (!enabled || stopped || frame++ % CHECK_FRAMES)
        return;
    if (GetModuleHandleA("rinput.dll")) {
        stopped = TRUE;
        if (patched == (BYTE *)GetModuleHandleA("client.dll") && slot_get) {
            put_slot(slot_get, (void *)real_get);
            put_slot(slot_set, (void *)real_set);
        }
        if (ui_patched == (BYTE *)GetModuleHandleA("vgui2.dll") && slot_ui_get)
            put_slot(slot_ui_get, (void *)real_ui_get);
        vkgl_log("mouse: RInput is loaded, raw_mouse left to it");
        return;
    }
    mod = (BYTE *)GetModuleHandleA("client.dll");
    if (mod && (mod != patched || (slot_get && *slot_get != (void *)raw_get)))
        redirect(mod);
    mod = (BYTE *)GetModuleHandleA("vgui2.dll");
    if (mod && real_get && (mod != ui_patched || (slot_ui_get && *slot_ui_get != (void *)ui_get))) {
        ui_patched = mod;
        if ((slot_ui_get = import_slot(mod, "user32.dll", "GetCursorPos"))) {
            if (*slot_ui_get != (void *)ui_get)
                real_ui_get = *slot_ui_get;
            put_slot(slot_ui_get, (void *)ui_get);
        }
    }
}

static DWORD WINAPI test_thread(void *arg)
{
    int *t = arg;
    volatile float *yaw = (float *)0x02de10c8;
    float before;
    INPUT in = { .type = INPUT_MOUSE, .mi = { .dwFlags = MOUSEEVENTF_MOVE } };

    while (*(volatile int *)0x02daf900 != 5)
        Sleep(100);
    Sleep(t[0]);
    if (GetModuleHandleA("client.dll") != (HMODULE)0x27000000 || !readable(0x270748ec, 4))
        return 0;
    *(float *)(*(BYTE **)0x270748ec + 0xc) = 1.0f;
    Sleep(100);
    before = *yaw;
    in.mi.dx = t[2];
    for (int i = 0; i < t[1]; i++) {
        SendInput(1, &in, sizeof(in));
        Sleep(t[3]);
    }
    Sleep(500);
    vkgl_log("mouse test: %d moves of %d counts turned the view %.4f degrees (yaw %.4f -> %.4f), "
             "%ld raw events, %u reads, %u real re-centres",
             t[1], t[2], before - *yaw, before, *yaw, raw_events, reads, warps);
    return 0;
}

void vkgl_mouse_setup(void)
{

    static BOOL done;
    static int test[4];
    char buf[64];

    if (done)
        return;
    done = TRUE;
    if (GetEnvironmentVariableA("VKGL_MOUSE_TEST", buf, sizeof(buf)) &&
        sscanf(buf, "%d,%d,%d,%d", &test[0], &test[1], &test[2], &test[3]) == 4)
        CloseHandle(CreateThread(NULL, 0, test_thread, test, 0, NULL));

    virtual_only = GetEnvironmentVariableA("VKGL_MOUSE_VIRTUAL", NULL, 0) > 0;
    if (virtual_only)
        raw_events = 1;
    if (!vkgl_ini_int("engine", "raw_mouse", 1) && !virtual_only)
        return;
    enabled = TRUE;
    if (virtual_only) {
        vkgl_log("mouse: virtual cursor only (VKGL_MOUSE_VIRTUAL)");
        return;
    }
    CloseHandle(CreateThread(NULL, 0, raw_thread, NULL, 0, NULL));
}
