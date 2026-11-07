#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winternl.h>
#include "vkgl.h"

extern const char *const vkgl_names[VKGL_NUM_FUNCS];
extern void *vkgl_dispatch[VKGL_NUM_FUNCS];

static HMODULE system_gl;

static const char vkgl_key[] = "Software\\vkgl";

static HMODULE load_system_gl(void)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);

    if (system_gl)
        return system_gl;
    lstrcpynA(path + n, "\\opengl32.dll", MAX_PATH - n);
    system_gl = LoadLibraryA(path);
    if (system_gl == vkgl_module)
        system_gl = NULL;
    if (!system_gl)
        vkgl_log("cannot load %s", path);
    return system_gl;
}

static void *vulkan_table[VKGL_NUM_FUNCS], *system_table[VKGL_NUM_FUNCS];
static BOOL vulkan_on, want_vulkan, system_ready;
static int create_id = -1, delete_id = -1;
static int live_contexts;

static BOOL build_system_table(void)
{
    if (system_ready)
        return TRUE;
    if (!load_system_gl())
        return FALSE;
    for (int i = 0; i < VKGL_NUM_FUNCS; i++) {
        void *p = (void *)GetProcAddress(system_gl, vkgl_names[i]);
        system_table[i] = p ? p : vulkan_table[i];
    }
    system_ready = TRUE;
    return TRUE;
}

static HGLRC WINAPI select_create_context(HDC dc);
static BOOL WINAPI select_delete_context(HGLRC rc);

static void use_table(BOOL vulkan)
{
    memcpy(vkgl_dispatch, vulkan ? vulkan_table : system_table, sizeof(vulkan_table));
    if (create_id >= 0 && delete_id >= 0) {
        vkgl_dispatch[create_id] = (void *)select_create_context;
        vkgl_dispatch[delete_id] = (void *)select_delete_context;
    }
    vulkan_on = vulkan;
    vkgl_log("renderer: %s", vulkan ? "Vulkan" : "system OpenGL");
}

static HGLRC WINAPI select_create_context(HDC dc)
{
    HGLRC (WINAPI *create)(HDC) = (vulkan_on ? vulkan_table : system_table)[create_id];
    HGLRC rc;

    vkgl_engine_patches();
    if ((rc = create(dc)))
        live_contexts++;
    else
        vkgl_log("wglCreateContext failed (%s)", vulkan_on ? "Vulkan" : "system OpenGL");
    return rc;
}

static BOOL WINAPI select_delete_context(HGLRC rc)
{
    BOOL (WINAPI *delete)(HGLRC) = (vulkan_on ? vulkan_table : system_table)[delete_id];
    BOOL ok = delete(rc);

    if (ok && live_contexts > 0 && --live_contexts == 0 && want_vulkan != vulkan_on)
        use_table(want_vulkan);
    return ok;
}

BOOL vkgl_renderer_vulkan(void)
{
    return vulkan_on;
}

BOOL vkgl_renderer_wanted(void)
{
    return want_vulkan;
}

static DWORD reg_dword(const char *path, const char *name, DWORD def)
{
    DWORD value = def, size = sizeof(value);
    HKEY key;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExA(key, name, NULL, NULL, (BYTE *)&value, &size) != ERROR_SUCCESS)
            value = def;
        RegCloseKey(key);
    }
    return value;
}

static void set_reg_dword(const char *path, const char *name, DWORD value)
{
    HKEY key;

    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
        RegCloseKey(key);
    }
}

static BOOL choose_vulkan(void)
{
    char buf[16];

    if (GetEnvironmentVariableA("VKGL_RENDERER", buf, sizeof(buf)))
        return lstrcmpiA(buf, "gl") != 0;
    return reg_dword(vkgl_key, "Vulkan", 1) != 0;
}

void vkgl_renderer_request(BOOL vulkan)
{
    if (!vulkan && !build_system_table()) {
        vkgl_log("renderer: no system OpenGL, staying on Vulkan");
        return;
    }
    want_vulkan = vulkan;
    set_reg_dword(vkgl_key, "Vulkan", vulkan);
    if (!live_contexts && vulkan != vulkan_on)
        use_table(vulkan);
    else if (vulkan != vulkan_on)
        vkgl_log("renderer: %s once the engine restarts", vulkan ? "Vulkan" : "system OpenGL");
}

static int game_path(char *path, const char *rel)
{
    int n = GetModuleFileNameA(vkgl_module, path, MAX_PATH);

    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    lstrcpynA(path + n, rel, MAX_PATH - n);
    return n;
}

static void mod_dir(char *out, int size)
{
    const char *cmd = GetCommandLineA(), *p = strstr(cmd, " -game ");
    int n = 0;

    if (p) {
        for (p += 7; *p == ' '; p++)
            ;
        while (p[n] && p[n] != ' ' && n < size - 1)
            n++;
        lstrcpynA(out, p, n + 1);
    } else {
        lstrcpynA(out, "valve", size);
    }
}

static char *read_file(const char *path, DWORD *size)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char *data = NULL;
    DWORD got;

    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    *size = GetFileSize(f, NULL);
    if (*size != INVALID_FILE_SIZE && *size < (1 << 22) && (data = malloc(*size + 2))) {
        if (!ReadFile(f, data, *size, &got, NULL) || got != *size) {
            free(data);
            data = NULL;
        } else {
            data[*size] = data[*size + 1] = 0;
        }
    }
    CloseHandle(f);
    return data;
}

static WCHAR *set_token(WCHAR *text, int *len, const WCHAR *token, const WCHAR *value)
{
    WCHAR *p = wcsstr(text, token), *q, *out;
    int vlen = lstrlenW(value), start, end;

    if (!p)
        return text;
    p += lstrlenW(token);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"' || !(q = wcschr(p + 1, '"')))
        return text;
    start = p + 1 - text;
    end = q - text;
    if (end - start == vlen && !memcmp(text + start, value, vlen * sizeof(WCHAR)))
        return text;
    if (!(out = malloc((*len - (end - start) + vlen + 1) * sizeof(WCHAR))))
        return text;
    memcpy(out, text, start * sizeof(WCHAR));
    memcpy(out + start, value, vlen * sizeof(WCHAR));
    memcpy(out + start + vlen, text + end, (*len - end + 1) * sizeof(WCHAR));
    *len += vlen - (end - start);
    free(text);
    return out;
}

static void restore_menu_labels(void)
{
    char path[MAX_PATH], mod[64], rel[128];
    char *marker, *data;
    DWORD size;
    WCHAR *text;
    int len;
    BOOL ours;
    HANDLE f;

    mod_dir(mod, sizeof(mod));
    wsprintfA(rel, "%s/resource/gameui_english.txt", mod);
    game_path(path, ".vkgl-installed");
    if (!(marker = read_file(path, &size)))
        return;
    ours = strstr(marker, rel) != NULL;
    free(marker);
    game_path(path, rel);
    if (!ours || !(data = read_file(path, &size)))
        return;
    if (size < 2 || (BYTE)data[0] != 0xFF || (BYTE)data[1] != 0xFE) {
        free(data);
        return;
    }
    len = (size - 2) / 2;
    if (!(text = malloc((len + 1) * sizeof(WCHAR)))) {
        free(data);
        return;
    }
    memcpy(text, data + 2, len * sizeof(WCHAR));
    text[len] = 0;
    free(data);

    WCHAR *orig = malloc((len + 1) * sizeof(WCHAR));
    int orig_len = len;
    if (orig)
        memcpy(orig, text, (len + 1) * sizeof(WCHAR));
    text = set_token(text, &len, L"\"GameUI_OpenGL\"", L"OpenGL");
    text = set_token(text, &len, L"\"GameUI_D3D\"", L"D3D");
    if (orig && (len != orig_len || memcmp(orig, text, len * sizeof(WCHAR)))) {
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(f, "\xFF\xFE", 2, &written, NULL);
            WriteFile(f, text, len * sizeof(WCHAR), &written, NULL);
            CloseHandle(f);
        }
    }
    free(orig);
    free(text);
}

static FARPROC WINAPI wined3d_get_proc(HMODULE mod, LPCSTR name)
{
    if (mod == vkgl_module)
        mod = system_gl;
    return GetProcAddress(mod, name);
}

static void *wined3d_import(const char *dll, const char *fn)
{
    static const char *const gdi[][2] = {
        { "ChoosePixelFormat", "wglChoosePixelFormat" },
        { "DescribePixelFormat", "wglDescribePixelFormat" },
        { "GetPixelFormat", "wglGetPixelFormat" },
        { "SetPixelFormat", "wglSetPixelFormat" },
        { "SwapBuffers", "wglSwapBuffers" },
    };

    if (!lstrcmpiA(dll, "opengl32.dll"))
        return (void *)GetProcAddress(system_gl, fn);
    if (!lstrcmpiA(dll, "gdi32.dll"))
        for (int i = 0; i < 5; i++)
            if (!strcmp(fn, gdi[i][0]))
                return (void *)GetProcAddress(system_gl, gdi[i][1]);
    if (!lstrcmpiA(dll, "kernel32.dll") && !strcmp(fn, "GetProcAddress"))
        return (void *)wined3d_get_proc;
    return NULL;
}

static void patch_wined3d(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (void *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;

    if (!dir->VirtualAddress)
        return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir->VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)base + d->Name;
        IMAGE_THUNK_DATA *names = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                                           : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (void *)(base + d->FirstThunk);

        for (; names->u1.AddressOfData; names++, iat++) {
            IMAGE_IMPORT_BY_NAME *by_name;
            DWORD old;
            void *to;

            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            by_name = (void *)(base + names->u1.AddressOfData);
            if (!(to = wined3d_import(dll, (const char *)by_name->Name)))
                continue;
            if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                iat->u1.Function = (DWORD_PTR)to;
                VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                patched++;
            }
        }
    }
    vkgl_log("wined3d: %d imports point at the system opengl32", patched);
}

typedef struct {
    ULONG flags;
    const UNICODE_STRING *full_name, *base_name;
    void *base;
    ULONG size;
} dll_notification;

static BOOL named(const UNICODE_STRING *name, const WCHAR *want)
{
    int n = lstrlenW(want);

    return name && name->Length == n * sizeof(WCHAR) && !_wcsnicmp(name->Buffer, want, n);
}

static void CALLBACK on_dll_load(ULONG reason, const dll_notification *n, void *ctx)
{
    (void)ctx;
    if (reason != 1)
        return;
    if (named(n->base_name, L"wined3d.dll") && load_system_gl())
        patch_wined3d(n->base);
    else if (named(n->base_name, L"GameUI.dll"))
        vkgl_menu_patch(n->base);
}

static void watch_modules(void)
{
    LONG (NTAPI *reg)(ULONG, void *, void *, void **) =
        (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
    HMODULE wined3d = GetModuleHandleA("wined3d.dll"), gameui = GetModuleHandleA("GameUI.dll");
    static void *cookie;

    if (wined3d && load_system_gl())
        patch_wined3d(wined3d);
    if (gameui)
        vkgl_menu_patch(gameui);
    if (reg)
        reg(0, (void *)on_dll_load, NULL, &cookie);
}

void vkgl_select_renderer(void)
{
    memcpy(vulkan_table, vkgl_dispatch, sizeof(vulkan_table));
    for (int i = 0; i < VKGL_NUM_FUNCS; i++) {
        if (!strcmp(vkgl_names[i], "wglCreateContext"))
            create_id = i;
        else if (!strcmp(vkgl_names[i], "wglDeleteContext"))
            delete_id = i;
    }
    want_vulkan = choose_vulkan();
    if (!want_vulkan && !build_system_table())
        want_vulkan = TRUE;
    use_table(want_vulkan);
    watch_modules();
    restore_menu_labels();
}
