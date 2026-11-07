#include <string.h>
#include "vkgl.h"
#include "engine.h"

const BYTE msec_orig[MSEC_ORIG_LEN + 1] =
    "\xdd\x05\xd8\xde\x80\x02\xdc\x0d\x18\x76\xe8\x01\x83\xc4\x08\xe8\x3c\x73\x14\x00"
    "\x89\x44\x24\x24\xdb\x44\x24\x24\xdd\x5c\x24\x2c\xdd\x05\x90\x75\xe8\x01\xdc\x5c"
    "\x24\x2c\xdf\xe0\xf6\xc4\x05\x7a\x12\xc7\x44\x24\x24\x00\x00\x00\x00\xc7\x44\x24"
    "\x28\x00\xe0\x6f\x40\xeb\x10\x8b\x44\x24\x2c\x8b\x4c\x24\x30\x89\x44\x24\x24\x89"
    "\x4c\x24\x28\xdd\x44\x24\x24\xe8\xf4\x72\x14\x00";

#define CAP_SITE 0x01d5708f
static const BYTE cap_orig[] = "\xf6\xc4\x44\x7a\x35";

BOOL readable(DWORD addr, DWORD len)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!VirtualQuery((void *)(ULONG_PTR)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return FALSE;
    return addr + len <= (DWORD)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
}

#define CVAR_FIND_VAR 0x01d30f90
#define CVAR_VARS 0x020806dc

static const BYTE cvar_vars_load[] = "\x8b\x35\xdc\x06\x08\x02";

float *engine_cvar(const char *name)
{
    BYTE *v;
    size_t n = strlen(name) + 1;
    int i;

    if (!readable(CVAR_FIND_VAR + 12, sizeof(cvar_vars_load) - 1) ||
        memcmp((void *)(CVAR_FIND_VAR + 12), cvar_vars_load, sizeof(cvar_vars_load) - 1))
        return NULL;
    v = readable(CVAR_VARS, 4) ? *(BYTE **)CVAR_VARS : NULL;
    for (i = 0; v && i < 8192 && readable((DWORD)(ULONG_PTR)v, 20); i++, v = *(BYTE **)(v + 16))
        if (readable(*(DWORD *)v, n) && !memcmp(*(char **)v, name, n))
            return (float *)(v + 12);
    return NULL;
}

void write_code(DWORD addr, const void *bytes, DWORD len)
{
    DWORD old;

    VirtualProtect((void *)(ULONG_PTR)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)(ULONG_PTR)addr, bytes, len);
    VirtualProtect((void *)(ULONG_PTR)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(ULONG_PTR)addr, len);
}

BYTE *put(BYTE *p, const char *bytes, int len)
{
    memcpy(p, bytes, len);
    return p + len;
}

BYTE *put32(BYTE *p, DWORD v)
{
    memcpy(p, &v, 4);
    return p + 4;
}

BYTE *rel32(BYTE *p, DWORD target)
{
    return put32(p, target - ((DWORD)(ULONG_PTR)p + 4));
}

static void fps_unlock(void)
{
    if (!readable(CAP_SITE, 5))
        return;
    if (!memcmp((void *)CAP_SITE, cap_orig, 5)) {
        write_code(CAP_SITE + 3, "\xeb", 1);
        vkgl_log("engine: fps_max cap of 100 removed");
    } else if (memcmp((void *)CAP_SITE, "\xf6\xc4\x44\xeb\x35", 5)) {
        const BYTE *b = (const BYTE *)CAP_SITE;
        vkgl_log("engine: Host_FilterTime is not the expected build (%02x %02x %02x %02x %02x), "
                 "fps unlock skipped", b[0], b[1], b[2], b[3], b[4]);
    }
}

int vkgl_ini_int(const char *section, const char *key, int def)
{
    static char path[MAX_PATH];

    if (!path[0]) {
        DWORD n = GetModuleFileNameA(vkgl_module, path, MAX_PATH);

        while (n && path[n - 1] != '\\' && path[n - 1] != '/')
            n--;
        lstrcpynA(path + n, "vkgl.ini", MAX_PATH - n);
    }
    return GetPrivateProfileIntA(section, key, def, path);
}

BOOL vkgl_engine_image(BYTE **base, SIZE_T *size)
{
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *p;

    if (!readable(MSEC_SITE, MSEC_ORIG_LEN) ||
        (memcmp((void *)MSEC_SITE, msec_orig, MSEC_ORIG_LEN) &&
         memcmp((void *)MSEC_SITE, "\x83\xc4\x08\xe9", 4)))
        return FALSE;
    VirtualQuery((void *)MSEC_SITE, &mbi, sizeof(mbi));
    *base = p = mbi.AllocationBase;
    while (VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.AllocationBase == *base)
        p = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
    *size = p - *base;
    return TRUE;
}

static void dump_image(void)
{
    char path[MAX_PATH];
    BYTE *base;
    SIZE_T size;
    HANDLE f;
    DWORD n;

    if (!GetEnvironmentVariableA("VKGL_DUMP_ENGINE", path, sizeof(path)) || !vkgl_engine_image(&base, &size))
        return;
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    for (SIZE_T off = 0; off < size; off += 4096) {
        static const BYTE zero[4096];
        WriteFile(f, readable((DWORD)(ULONG_PTR)(base + off), 4096) ? base + off : zero, 4096, &n, NULL);
    }
    CloseHandle(f);
    vkgl_log("engine: image %p + 0x%lx written to %s", base, (unsigned long)size, path);
}

void vkgl_engine_patches(void)
{
    static BOOL done;

    if (done)
        return;
    done = TRUE;
    dump_image();
    vkgl_physics_setup(vkgl_ini_int("engine", "physics_fix", 1), vkgl_ini_int("engine", "physics_interp", 1));
    if (vkgl_ini_int("engine", "fps_unlock", 1))
        fps_unlock();
}
