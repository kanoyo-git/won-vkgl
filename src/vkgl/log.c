#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkgl.h"
#include "engine.h"

HMODULE vkgl_module;
static HANDLE log_file = INVALID_HANDLE_VALUE;

extern const char *const vkgl_names[VKGL_NUM_FUNCS];

static void log_open(void)
{
    char path[MAX_PATH], buf[4];
    DWORD n = GetModuleFileNameA(vkgl_module, path, MAX_PATH);

    BOOL reload = GetEnvironmentVariableA("VKGL_LOG_OPEN", buf, sizeof(buf)) > 0;

    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    lstrcpynA(path + n, "vkgl.log", MAX_PATH - n);
    log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           reload ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    SetEnvironmentVariableA("VKGL_LOG_OPEN", "1");
}

static int log_time = -1;
static LARGE_INTEGER log_t0, log_freq;

static void log_va(const char *fmt, va_list ap)
{
    char line[1024];
    int n = 0, m;
    DWORD written;

    if (log_time < 0) {
        log_time = GetEnvironmentVariableA("VKGL_LOG_TIME", NULL, 0) > 0;
        QueryPerformanceFrequency(&log_freq);
        QueryPerformanceCounter(&log_t0);
    }
    if (log_time) {
        LARGE_INTEGER now;

        QueryPerformanceCounter(&now);
        n = snprintf(line, sizeof(line), "[%9.3f] ", (double)(now.QuadPart - log_t0.QuadPart) / log_freq.QuadPart);
    }
    m = vsnprintf(line + n, sizeof(line) - 1 - n, fmt, ap);
    if (m < 0)
        return;
    n += m;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    if (log_file != INVALID_HANDLE_VALUE)
        WriteFile(log_file, line, n, &written, NULL);
    line[n - 1] = 0;
    OutputDebugStringA(line);
}

void vkgl_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_va(fmt, ap);
    va_end(ap);
}

void vkgl_fatal(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    vkgl_log("FATAL: %s", msg);
    MessageBoxA(NULL, msg, "vkgl", MB_ICONERROR);
    ExitProcess(1);
}

static void where(DWORD a, char *out, int size)
{
    MEMORY_BASIC_INFORMATION mi;
    HMODULE m;
    char path[MAX_PATH];
    const char *name;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(ULONG_PTR)a, &m) && GetModuleFileNameA(m, path, sizeof(path))) {
        name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
        snprintf(out, size, "%s+%lx", name, a - (DWORD)(ULONG_PTR)m);
    } else if (VirtualQuery((void *)(ULONG_PTR)a, &mi, sizeof(mi))) {
        snprintf(out, size, "%08lx+%lx", (DWORD)(ULONG_PTR)mi.AllocationBase, a - (DWORD)(ULONG_PTR)mi.AllocationBase);
    } else {
        snprintf(out, size, "?");
    }
}

static LONG CALLBACK crash_log(EXCEPTION_POINTERS *ep)
{
    static volatile LONG left = 4;
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    const CONTEXT *c = ep->ContextRecord;
    const DWORD *sp = (const DWORD *)(ULONG_PTR)c->Esp;
    char at[MAX_PATH + 16];

    switch (r->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_IN_PAGE_ERROR:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedDecrement(&left) < 0)
        return EXCEPTION_CONTINUE_SEARCH;
    where(c->Eip, at, sizeof(at));
    vkgl_log("exception %08lx at %08lx (%s), thread %lu, %s %08lx", r->ExceptionCode, c->Eip, at,
             GetCurrentThreadId(), r->NumberParameters >= 2 && r->ExceptionInformation[0] ? "write" : "read",
             r->NumberParameters >= 2 ? (DWORD)r->ExceptionInformation[1] : 0);
    if (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0, n = 0; i < 1024 && n < 20; i++) {
        MEMORY_BASIC_INFORMATION mi;
        DWORD v;

        if (!VirtualQuery(sp + i, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD))
            break;
        v = sp[i];
        if (v < 0x10000 || !VirtualQuery((void *)(ULONG_PTR)v, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
            !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        where(v, at, sizeof(at));
        vkgl_log("  [esp+%03x] %08lx %s", i * 4, v, at);
        n++;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void __cdecl vkgl_stub_hit(int id)
{
    static volatile LONG seen[VKGL_NUM_FUNCS];

    if (id >= 0 && id < VKGL_NUM_FUNCS && !InterlockedExchange(&seen[id], 1))
        vkgl_log("unimplemented: %s", vkgl_names[id]);
}

void *vkgl_traced[VKGL_NUM_FUNCS];
static volatile LONG trace_left;
static LONG trace_n;

void vkgl_trace_rearm(void)
{
    if (trace_n)
        InterlockedExchange(&trace_left, trace_n);
}

void __cdecl vkgl_trace_hit(int id)
{
    if (trace_left > 0 && InterlockedDecrement(&trace_left) >= 0)
        vkgl_log("call %s", vkgl_names[id]);
}

static void trace_calls(void)
{
    extern void *vkgl_dispatch[VKGL_NUM_FUNCS];
    extern void *const vkgl_trace_thunks[VKGL_NUM_FUNCS];
    char buf[16];

    if (!GetEnvironmentVariableA("VKGL_TRACE_CALLS", buf, sizeof(buf)))
        return;
    trace_left = trace_n = atoi(buf);
    for (int i = 0; i < VKGL_NUM_FUNCS; i++) {
        vkgl_traced[i] = vkgl_dispatch[i];
        vkgl_dispatch[i] = vkgl_trace_thunks[i];
    }
    vkgl_log("tracing the first %ld calls", trace_left);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        vkgl_module = inst;
        DisableThreadLibraryCalls(inst);
        log_open();
        vkgl_log("vkgl loaded");
        AddVectoredExceptionHandler(0, crash_log);
        vkgl_select_renderer();
        trace_calls();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved)
            vkgl_physics_exit();
        vkgl_log("vkgl unloaded%s", reserved ? " (process exit)" : "");
        CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}
