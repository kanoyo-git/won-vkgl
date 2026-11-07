#include <stdio.h>
#include "vkgl.h"
#include <mmsystem.h>
#include <psapi.h>

#define SLOTS 32768
#define MAX_MODULES 128
#define BATCH 64
#define STACK_WORDS 4096

volatile LONG vkgl_frame_count;
volatile LONG vkgl_upload_count;

struct hist { DWORD addr, n; };
static struct hist leaf[SLOTS], owner[SLOTS], entry[SLOTS];
static DWORD total, dropped;

struct module { DWORD base, end; BOOL system; char name[64]; };
static struct module modules[MAX_MODULES];
static int module_count;

static HANDLE target;
static DWORD target_id, stack_base;
static BOOL stall_mode, upload_mode, slow_mode;

static void hist_add(struct hist *h, DWORD addr, DWORD n)
{
    DWORD i = (addr * 2654435761u) & (SLOTS - 1);

    for (int probe = 0; probe < SLOTS; probe++, i = (i + 1) & (SLOTS - 1)) {
        if (h[i].addr == addr || !h[i].n) {
            h[i].addr = addr;
            h[i].n += n;
            return;
        }
    }
}

static struct module *module_of(DWORD addr, BOOL add)
{
    MEMORY_BASIC_INFORMATION mbi;
    struct module *m;
    char path[MAX_PATH];
    const char *base;
    DWORD n;

    for (int i = 0; i < module_count; i++)
        if (addr >= modules[i].base && addr < modules[i].end)
            return &modules[i];
    if (!add || module_count == MAX_MODULES || !addr ||
        !VirtualQuery((void *)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return NULL;
    m = &modules[module_count++];
    m->base = (DWORD)mbi.AllocationBase;
    m->end = (DWORD)mbi.BaseAddress + mbi.RegionSize;

    while (VirtualQuery((void *)m->end, &mbi, sizeof(mbi)) &&
           (DWORD)mbi.AllocationBase == m->base && mbi.RegionSize)
        m->end = (DWORD)mbi.BaseAddress + mbi.RegionSize;
    n = GetModuleFileNameA((HMODULE)m->base, path, MAX_PATH);
    if (n) {
        base = path + n;
        while (base > path && base[-1] != '\\' && base[-1] != '/')
            base--;
        lstrcpynA(m->name, (HMODULE)m->base == vkgl_module ? "vkgl" : base, sizeof(m->name));
        CharLowerA(m->name);
        m->system = !_strnicmp(path, "C:\\windows\\", 11);
    } else {
        wsprintfA(m->name, "anon_%08lx", m->base);
    }
    return m;
}

static BOOL looks_like_return(DWORD addr)
{
    const BYTE *p = (const BYTE *)addr;

    return p[-5] == 0xE8 ||
           (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) ||
           (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
           (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) ||
           (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
}

static DWORD find_owner(const DWORD *stack, int words, DWORD *via)
{
    for (int i = 0; i < words; i++) {
        struct module *m = module_of(stack[i], FALSE);
        if (!m || stack[i] - m->base <= 8 || !looks_like_return(stack[i]))
            continue;
        if (!m->system)
            return stack[i];
        *via = stack[i];
    }
    return 0;
}

static const char *export_name(const struct module *m, DWORD addr)
{
    const BYTE *base = (const BYTE *)m->base;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_EXPORT_DIRECTORY *ex;
    const DWORD *funcs, *names;
    const WORD *ords;
    DWORD rva = addr - m->base, best = 0, dir;
    const char *name = "?";

    if (((const IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE)
        return name;
    nt = (const void *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (!(dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress))
        return name;
    ex = (const void *)(base + dir);
    funcs = (const void *)(base + ex->AddressOfFunctions);
    names = (const void *)(base + ex->AddressOfNames);
    ords = (const void *)(base + ex->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ex->NumberOfNames; i++) {
        DWORD f = funcs[ords[i]];
        if (f <= rva && f > best) {
            best = f;
            name = (const char *)base + names[i];
        }
    }
    return name;
}

static void scan_modules(void)
{
    HMODULE mods[MAX_MODULES];
    DWORD bytes;

    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes))
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < MAX_MODULES; i++)
            module_of((DWORD)mods[i], TRUE);
}

static void write_report(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(vkgl_module, path, MAX_PATH);
    FILE *f;

    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    lstrcpynA(path + n, "vkgl_profile.txt", MAX_PATH - n);
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "samples %lu dropped %lu\n", total, dropped);
    for (int i = 0; i < module_count; i++)
        fprintf(f, "module %s %08lx %08lx %d\n", modules[i].name, modules[i].base,
                modules[i].end, modules[i].system);
    for (int k = 0; k < 3; k++) {
        static const char *const kinds[] = { "leaf", "owner", "entry" };
        struct hist *h = k == 0 ? leaf : k == 1 ? owner : entry;
        for (int i = 0; i < SLOTS; i++) {
            struct module *m;
            if (!h[i].n)
                continue;
            m = module_of(h[i].addr, FALSE);
            fprintf(f, "%s %s %lx %lu", kinds[k], m ? m->name : "?",
                    m ? h[i].addr - m->base : h[i].addr, h[i].n);
            if (m && m->system)
                fprintf(f, " %s", export_name(m, h[i].addr));
            fputc('\n', f);
        }
    }
    fclose(f);
}

static void print_addr(char *out, size_t size, DWORD addr)
{
    struct module *m = module_of(addr, FALSE);

    if (!m)
        snprintf(out, size, " %08lx", addr);
    else if (m->system)
        snprintf(out, size, " %s!%s+%lx", m->name, export_name(m, addr), addr - m->base);
    else
        snprintf(out, size, " %s+%lx", m->name, addr - m->base);
}

static void log_stall(double ms, DWORD eip, const DWORD *stack, int words)
{
    char line[1024];
    size_t n = snprintf(line, sizeof(line), "stall %.1f ms:", ms);
    int found = 0;

    print_addr(line + n, sizeof(line) - n, eip);
    n = strlen(line);
    for (int i = 0; i < words && found < 12 && n < sizeof(line) - 80; i++) {
        struct module *m = module_of(stack[i], FALSE);
        if (!m || stack[i] - m->base <= 8 || !looks_like_return(stack[i]))
            continue;
        print_addr(line + n, sizeof(line) - n, stack[i]);
        n = strlen(line);
        found++;
    }
    vkgl_log("profile: %s", line);
}

static DWORD WINAPI sampler(void *arg)
{
    static DWORD stack[STACK_WORDS];
    DWORD batch_leaf[BATCH], batch_owner[BATCH], batch_entry[BATCH];
    DWORD last_write = GetTickCount();
    int count = 0, words;
    LONG frames = vkgl_frame_count, stall_frame = vkgl_frame_count, uploads = vkgl_upload_count;
    LARGE_INTEGER freq, now, stall_since, stall_logged = { 0 }, batch_start;
    CONTEXT ctx;

    (void)arg;
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&stall_since);
    batch_start = stall_since;
    scan_modules();
    for (;;) {
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        Sleep(1);
        QueryPerformanceCounter(&t1);
        if (SuspendThread(target) == (DWORD)-1)
            break;
        QueryPerformanceCounter(&t2);
        if (stall_mode && (t2.QuadPart - t0.QuadPart) * 1000 > freq.QuadPart * 5)
            vkgl_log("profile: sampler late: sleep %.1f ms, suspend %.1f ms",
                     (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart,
                     (t2.QuadPart - t1.QuadPart) * 1000.0 / freq.QuadPart);
        ctx.ContextFlags = CONTEXT_CONTROL;
        words = 0;
        if (GetThreadContext(target, &ctx) && ctx.Esp < stack_base) {
            words = (stack_base - ctx.Esp) / 4;
            if (words > STACK_WORDS)
                words = STACK_WORDS;
            memcpy(stack, (void *)ctx.Esp, words * 4);
        }
        ResumeThread(target);
        if (stall_mode) {
            QueryPerformanceCounter(&now);
            if (vkgl_frame_count != stall_frame) {
                stall_frame = vkgl_frame_count;
                stall_since = now;
            } else if (words && now.QuadPart - stall_since.QuadPart >= freq.QuadPart / 100 &&
                       now.QuadPart - stall_logged.QuadPart >= freq.QuadPart / 200) {
                log_stall((now.QuadPart - stall_since.QuadPart) * 1000.0 / freq.QuadPart,
                          ctx.Eip, stack, words);
                stall_logged = now;
            }
            if (GetTickCount() - last_write > 5000) {
                scan_modules();
                last_write = GetTickCount();
            }
            continue;
        }
        if (words) {
            struct module *m = module_of(ctx.Eip, TRUE);
            DWORD via = 0;
            batch_leaf[count] = ctx.Eip;
            batch_owner[count] = m && !m->system ? ctx.Eip : find_owner(stack, words, &via);

            batch_entry[count] = m && m->system ? (via ? via : ctx.Eip) : 0;
            count++;
        }
        if (count == BATCH) {

            LONG nf = vkgl_frame_count - frames, nu = vkgl_upload_count - uploads;
            QueryPerformanceCounter(&now);
            if (nf >= 4 && (!upload_mode || nu >= nf * 4) &&
                (!slow_mode || (now.QuadPart - batch_start.QuadPart) * 10000 >= freq.QuadPart * 17 * nf)) {
                for (int i = 0; i < BATCH; i++) {
                    hist_add(leaf, batch_leaf[i], 1);
                    hist_add(owner, batch_owner[i], 1);
                    if (batch_entry[i])
                        hist_add(entry, batch_entry[i], 1);
                }
                total += BATCH;
            } else {
                dropped += BATCH;
            }
            frames = vkgl_frame_count;
            uploads = vkgl_upload_count;
            batch_start = now;
            count = 0;
        }
        if (GetTickCount() - last_write > 5000) {
            scan_modules();
            write_report();
            last_write = GetTickCount();
        }
    }
    return 0;
}

void vkgl_profile_thread(void)
{
    static int enabled = -1;
    char buf[8];

    if (enabled < 0) {
        enabled = GetEnvironmentVariableA("VKGL_PROFILE", buf, sizeof(buf)) > 0;
        stall_mode = enabled && !lstrcmpiA(buf, "stall");
        upload_mode = enabled && !lstrcmpiA(buf, "uploads");
        slow_mode = enabled && !lstrcmpiA(buf, "slow");
    }
    if (!enabled || target_id == GetCurrentThreadId())
        return;
    if (target) {
        vkgl_log("profile: rendering moved to thread %lu, still sampling %lu",
                 GetCurrentThreadId(), target_id);
        return;
    }
    target_id = GetCurrentThreadId();
    stack_base = (DWORD)((NT_TIB *)NtCurrentTeb())->StackBase;
    target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, target_id);
    if (target && CreateThread(NULL, 0, sampler, NULL, 0, NULL))
        vkgl_log("profile: sampling thread %lu into %s", target_id,
                 stall_mode ? "vkgl.log (long frames)" : "vkgl_profile.txt");
}
