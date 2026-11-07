#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkgl.h"
#include "engine.h"

#define HOST_FRAMETIME 0x0280ded8
#define HOST_FRAMERATE 0x01eca2a8
#define FPS_MAX 0x01eca1ec
#define CLS_STATE 0x02daf900
#define REALTIME 0x0280dfa0
#define OLDREALTIME 0x0280df58
#define CL_VIEWANGLES 0x02de10c4
#define SV_ACTIVE 0x02518840
#define SV_PAUSED 0x02518844
#define SV_TIME 0x02518850
#define SV_NAME 0x02518870
#define SV_NUM_EDICTS 0x02554498
#define SV_EDICTS 0x025544a0
#define SVS_MAXCLIENTS 0x02517c68
#define SVS_CLIENTS 0x02517c64
#define CL_CMDTIME 0x2590
#define STRING_BASE 0x02517df8
#define RNG_IV 0x02432a78
#define RNG_IDUM 0x02433250
#define RNG_IY 0x02433254
#define CBUF_ADDTEXT 0x01d2a880
#define CMD_TEXT_DATA 0x02d5e0a8
#define CMD_TEXT_MAXSIZE 0x02d5e0ac
#define CMD_TEXT_CURSIZE 0x02d5e0b0

#define PM_FLOATTIME_SITE 0x01d745a8
static const BYTE pm_floattime_orig[] = "\xc7\x82\x6c\xf5\x04\x00\xe0\x96\xdb\x01";

#define CALL_FILTERTIME 0x01d58363
#define HOST_FILTERTIME 0x01d56f80
#define CALL_CBUF 0x01d583b3
#define CBUF_EXECUTE 0x01d2aa00

#define CBUF_BODY 0x01d2aa06
static const BYTE cbuf_entry_orig[] = "\x8b\x15\xb0\xe0\xd5\x02";
static const BYTE cbuf_body_orig[] = "\x81\xec\x00\x04\x00\x00";

#define CL_ENGINEFUNCS 0x01eb72d8
#define ENG_GETVIEWANGLES 0x01d0eb20
#define ENG_SETVIEWANGLES 0x01d0eb60
#define CALL_SV_FRAME 0x01d583d6
#define SV_FRAME 0x01dacd30
#define CALL_SCREEN 0x01d58463
#define CALL_CLDATA 0x01d583b8
#define CL_UPDATECLIENTDATA 0x01d0e780
#define HOST_UPDATESCREEN 0x01d58170

#define SV_SPAWNSERVER 0x01daa900
static const DWORD spawn_calls[] = { 0x01d5a2e4, 0x01d5a865, 0x01d5a9f8, 0x01d5aa79, 0x01d5cf67 };

#define CL_ENTITIES 0x01ef9b04
#define CL_ENTITY_SIZE 0xbb8
#define CL_FLOORCOLOR 0xba8
#define CL_ORIGIN 0xb48
#define CL_MODEL 0xb94
#define CL_STATE 0x2b0
#define ES_MODELINDEX 40
#define ES_EFFECTS 60
#define ES_MOVETYPE 88
#define MOVETYPE_STEP 4
#define MOVETYPE_FOLLOW 12
#define EF_NOINTERP 32
#define FLOOR_MAX 2048

#define GET_ENTITY_ILLUM 0x01d77b50
static const BYTE illum_orig[] = "\x83\xec\x10\x56\x8b\x74\x24\x18";

#define CALCREFDEF_SITE 0x01d0e4c0
#define CALCREFDEF_PTR 0x02f712ec
static const BYTE calcrefdef_orig[] = "\xa1\xec\x12\xf7\x02\x85\xc0\x74\x02\xff\xe0\xc3";
static const BYTE ran1_orig[] = "\x8b\x0d\x50\x32\x43\x02\x56\x85\xc9\x7e\x0a\x8b\x35\x54\x32\x43\x02";
static const BYTE hfr_name[] = "host_framerate";
static const BYTE fps_max_name[] = "fps_max";

#define CMD_ANGLES 4
#define CMD_MOVE 0x10
#define CMD_LIGHTLEVEL 0x1c
#define CMD_BUTTONS 0x1e
#define CMD_IMPULSE 0x20
#define CMD_WEAPONSELECT 0x21

#define REF_SIMORG 100
#define REF_DEMOPLAYBACK 188

#define EDICT_SIZE 0x324
#define EV 0x80
#define EV_ORIGIN (EV + 0x08)
#define EV_VELOCITY (EV + 0x20)
#define EV_BASEVELOCITY (EV + 0x2c)
#define EV_ANGLES (EV + 0x50)
#define EV_AVELOCITY (EV + 0x5c)
#define EV_PUNCHANGLE (EV + 0x68)
#define EV_V_ANGLE (EV + 0x74)
#define EV_NEXTTHINK (EV + 0x104)
#define EV_MOVETYPE (EV + 0x108)
#define EV_HEALTH (EV + 0x160)
#define EV_VIEW_OFS (EV + 0x174)
#define EV_BUTTON (EV + 0x180)
#define EV_GROUNDENTITY (EV + 0x19c)
#define EV_FLAGS (EV + 0x1a4)
#define EV_WATERLEVEL (EV + 0x1c0)
#define EV_SIZE 0x2a4
#define FL_CLIENT 0x8
#define FL_DUCKING 0x4000

#define TICK 10

static double tick_sec;

enum { FRAME_SLOW, FRAME_SKIP, FRAME_TICK };

static BOOL fix_on, interp_on, tick_stats;
static BOOL first_step_draw;
static int frame_mode = FRAME_SLOW;
static unsigned frame_no, decided_frame = ~0u;
static unsigned level_frame = ~0u;
static BOOL level_reconnect;
static unsigned level_reconnect_steps;
static BOOL level_host_loading;
static double tick_acc;
static int tick_held, tick_pressed, tick_impulse, tick_weapon;
static unsigned tick_count;
static BOOL tick_regular;
static double tick_frac;
static float view_start[3];
static BOOL bench_mouse;
static double world_acc;
static double cmd_ahead;
static double world_slow;
static int slow_msec;
static float slow_turn = -1;
static BOOL fast;
static double ft_avg;
static BOOL ft_restart = TRUE;
#define FAST_ENTER 9.95
#define FAST_LEAVE 10.5
static BOOL fps_max_ok;
static BOOL low_restart;
static float *pitch_up, *pitch_down;
static int *kb_state[16];
static int kb_marks[16];
static int kb_count = -1;
static float adj_after[3];
static unsigned adj_frame = ~0u;
static WORD kb_bits;
static DWORD kb_module[3];
static double console_ft;
static double drop_carry;
static char held_text[16384];
static BOOL autofunc_ft;
static BOOL bench_play;

static BOOL autofunc_armed;
static BOOL autofunc_shown;
static double autofunc_real;
static int trace_on;
static int ent_log = -2;
static int trace_left;

static void(__cdecl *const cbuf_addtext)(const char *) = (void *)CBUF_ADDTEXT;

typedef struct { int iv[32], idum, iy; } rng_t;
static rng_t rng_shared;
static rng_t rng_draw;
static BOOL rng_aside;
static FILE *rng_log;
static BOOL in_sv_frame, in_cbuf;
static FILE *rng_rec_file;
static void rng_report(void);
static void events_start(void);
static BOOL vclock;
static BOOL bench_norender;
static double vclock_jitter;
static FILE *view_log;
static void events_load(const char *path);

static void rng_get(rng_t *r)
{
    memcpy(r->iv, (void *)RNG_IV, sizeof(r->iv));
    r->idum = *(int *)RNG_IDUM;
    r->iy = *(int *)RNG_IY;
}

static void rng_set(const rng_t *r)
{
    memcpy((void *)RNG_IV, r->iv, sizeof(r->iv));
    *(int *)RNG_IDUM = r->idum;
    *(int *)RNG_IY = r->iy;
}

static void rng_seed(rng_t *r, int seed)
{
    memset(r, 0, sizeof(*r));
    r->idum = -(seed > 0 ? seed : 1);
}

static void rng_between(BOOL on)
{
    if (on == rng_aside)
        return;
    if (on) {
        rng_get(&rng_shared);
        rng_set(&rng_draw);
    } else {
        rng_get(&rng_draw);
        rng_set(&rng_shared);
    }
    rng_aside = on;
}

static BYTE *edict(int i)
{
    return *(BYTE **)SV_EDICTS + i * EDICT_SIZE;
}

static BYTE *player(void)
{
    BYTE *e;

    if (!*(int *)SV_ACTIVE || *(int *)SV_NUM_EDICTS < 2 || !*(BYTE **)SV_EDICTS)
        return NULL;
    e = edict(1);
    return *(int *)e ? NULL : e;
}

static BOOL level_reconnecting(void)
{
    BYTE *p;

    if (!level_reconnect)
        return FALSE;
    p = player();

    if (*(int *)CLS_STATE != 5 || level_reconnect_steps >= 10 ||
        (p && (*(int *)(p + EV_FLAGS) & FL_CLIENT)))
        level_reconnect = FALSE;
    return level_reconnect;
}

static BOOL level_host_rate_fast(void)
{
    float ft = *(float *)HOST_FRAMERATE;

    return level_host_loading && ft > 0 && ft * 1000.0 < FAST_ENTER;
}

static int edict_index(BYTE *e)
{
    BYTE *base = *(BYTE **)SV_EDICTS;

    return e && base ? (int)((e - base) / EDICT_SIZE) : -1;
}

static BOOL client_active(void)
{
    return *(int *)CLS_STATE == 5 && *(int *)SV_ACTIVE && !*(int *)SV_PAUSED;
}

#pragma pack(push, 1)
typedef struct {
    float angles[3], move[3];
    WORD buttons;
    BYTE impulse, weaponselect, lightlevel, resync, pad[2];
    float origin[3], velocity[3];
} bench_cmd_t;
#pragma pack(pop)

typedef struct {
    char map[32];
    int ncmds;
    bench_cmd_t *cmds;
} bench_sec_t;

typedef struct {
    char name[16];
    int nsecs;
    bench_sec_t *secs;
} bench_seg_t;

enum { BENCH_OFF, BENCH_START, BENCH_LOAD, BENCH_RUN, BENCH_CHANGE, BENCH_DONE, BENCH_WATCH };

static struct {
    int state;
    int nsegs, seg, sec;
    bench_seg_t *segs;
    double t0, deadline;
    int last_idx, applied_idx;
    int seg_started;
    float fps_lo, fps_hi;
    unsigned rand;
    int wait_frames;
    BOOL debug, full_dump;
    int dump_idx[16], ndump_idx;
    int debug_frames;
    FILE *log;
    double log_time;
    double watch_t0;
    float pre_ft;
    double watch_ft;
    double watch_nominal;
    BOOL exact_view;
    BOOL watch_armed;
    BOOL watch_loaded;
    char watch_map[64];
    double watch_last_t;
    BOOL watch_started;
    int watch_starts;
    char log_map[64];
    int dump_slot;
    char classname[2048][24];
    double next_report;
} bench;

static void bench_text(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cbuf_addtext(buf);
}

static BOOL bench_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[8];
    int s, c;

    if (!f)
        return FALSE;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "VKTAS1\0\0", 8) ||
        fread(&bench.nsegs, 4, 1, f) != 1 || bench.nsegs <= 0 || bench.nsegs > 1000)
        goto bad;
    bench.segs = calloc(bench.nsegs, sizeof(*bench.segs));
    for (s = 0; s < bench.nsegs; s++) {
        bench_seg_t *g = &bench.segs[s];
        if (fread(g->name, 1, 16, f) != 16 || fread(&g->nsecs, 4, 1, f) != 1 || g->nsecs <= 0 || g->nsecs > 100)
            goto bad;
        g->name[15] = 0;
        g->secs = calloc(g->nsecs, sizeof(*g->secs));
        for (c = 0; c < g->nsecs; c++) {
            bench_sec_t *x = &g->secs[c];
            if (fread(x->map, 1, 32, f) != 32 || fread(&x->ncmds, 4, 1, f) != 1 || x->ncmds <= 0 ||
                x->ncmds > 10000000)
                goto bad;
            x->map[31] = 0;
            x->cmds = malloc(x->ncmds * sizeof(bench_cmd_t));
            if (fread(x->cmds, sizeof(bench_cmd_t), x->ncmds, f) != (size_t)x->ncmds)
                goto bad;
        }
    }
    fclose(f);
    return TRUE;
bad:
    fclose(f);
    vkgl_log("bench: %s is not a VKTAS1 script", path);
    return FALSE;
}

#pragma pack(push, 1)
typedef struct {
    char type, pad[3];
    int seg, sec, idx;
    double time;
    float frametime;
    float origin[3], velocity[3], basevelocity[3], v_angle[3], punchangle[3];
    float view_ofs_z, health;
    int flags, button, waterlevel, movetype, ground;
    unsigned long long hash;
    int nents;
} log_tick_t;

typedef struct {
    int index, movetype;
    float origin[3], velocity[3], angles[3], avelocity[3];
    float nextthink, health;
} log_ent_t;
#pragma pack(pop)

static BOOL log_through;

static void log_write(const void *p, size_t n)
{
    if (bench.log) {
        fwrite(p, 1, n, bench.log);
        if (log_through)
            fflush(bench.log);
    }
}

typedef struct {
    int starts, sec0;
    char log[MAX_PATH], rec[MAX_PATH], play[MAX_PATH];
    char map[64];
} split_t;

static BOOL split_level_ok(const split_t *sp)
{
    const char *m = (const char *)SV_NAME;

    if (!sp->map[0])
        return TRUE;
    return sp->map[0] == '!' ? strcmp(sp->map + 1, m) != 0 : !strcmp(sp->map, m);
}
static split_t *splits;
static int nsplits, cur_split = -1, split_end;
static char log_hdr[32];

static void rng_split(const split_t *sp);

static void log_open(const char *path)
{
    if (bench.log) {
        fclose(bench.log);
        bench.log = NULL;
    }
    if (!path[0] || !strcmp(path, "-"))
        return;
    bench.log = fopen(path, "wb");
    if (bench.log) {
        setvbuf(bench.log, NULL, _IOFBF, 1 << 20);
        log_write(log_hdr, sizeof(log_hdr));
    }
}

static void split_next(void)
{
    const split_t *sp;

    if (cur_split + 1 >= nsplits)
        return;
    sp = &splits[++cur_split];
    split_end = sp->starts ? bench.watch_starts + sp->starts : -1;
    log_open(sp->log);
    bench.sec = sp->sec0;
    bench.log_map[0] = 0;
    bench.log_time = 0;
    rng_split(sp);
    vkgl_log("bench: segment file %d of %d: %s", cur_split + 1, nsplits, sp->log);
}

static void splits_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4 * MAX_PATH + 16];
    int cap = 0;

    if (!f) {
        vkgl_log("bench: cannot read %s", path);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        split_t sp;
        char *tok[6], *q = line;
        int k;

        line[strcspn(line, "\r\n")] = 0;
        for (k = 0; k < 6 && q; k++) {
            tok[k] = q;
            q = strchr(q, '\t');
            if (q)
                *q++ = 0;
        }
        if (k < 4)
            continue;
        memset(&sp, 0, sizeof(sp));
        sp.starts = atoi(tok[0]);
        sp.sec0 = k > 4 ? atoi(tok[4]) : 0;
        if (k > 5)
            lstrcpynA(sp.map, tok[5], sizeof(sp.map));
        lstrcpynA(sp.log, tok[1], MAX_PATH);
        lstrcpynA(sp.rec, tok[2], MAX_PATH);
        lstrcpynA(sp.play, tok[3], MAX_PATH);
        if (nsplits == cap) {
            cap = cap ? cap * 2 : 16;
            splits = realloc(splits, cap * sizeof(*splits));
        }
        splits[nsplits++] = sp;
    }
    fclose(f);
    vkgl_log("bench: %d segment files", nsplits);
}

static void log_map(void)
{
    char rec[4 + 8 + 64 + 8] = "M";

    memcpy(rec + 4, &bench.seg, 4);
    memcpy(rec + 8, &bench.sec, 4);
    memcpy(rec + 12, (void *)SV_NAME, 64);
    rec[12 + 63] = 0;
    memcpy(rec + 76, (void *)SV_TIME, 8);
    log_write(rec, sizeof(rec));
}

static unsigned long long fnv(unsigned long long h, const BYTE *p, size_t n)
{
    while (n--)
        h = (h ^ *p++) * 0x100000001b3ULL;
    return h;
}

static const char *string_at(int ofs);

static void canon_entvars(BYTE *v, BYTE *e)
{
    static const int ptrs[] = { 0x188, 0x18c, 0x190, 0x194, 0x198, 0x19c, 0x208, 0x294, 0x298, 0x29c, 0x2a0 };
    static const int strs[] = { 0x000, 0x004, 0x0b8, 0x0bc, 0x0c0, 0x1c8, 0x1cc, 0x1d0, 0x1d4,
                                0x1e8, 0x1ec, 0x1f0, 0x1f4 };
    unsigned k;

    memcpy(v, e + EV, EV_SIZE);

    memset(v + 0x124, 0, 4);
    for (k = 0; k < sizeof(ptrs) / sizeof(ptrs[0]); k++) {
        int idx = edict_index(*(BYTE **)(v + ptrs[k]));
        memcpy(v + ptrs[k], &idx, 4);
    }
    for (k = 0; k < sizeof(strs) / sizeof(strs[0]); k++) {
        const char *str = string_at(*(int *)(v + strs[k]));
        unsigned x = (unsigned)fnv(0xcbf29ce484222325ULL, (const BYTE *)str, lstrlenA(str));
        memcpy(v + strs[k], &x, 4);
    }

    k = (unsigned)edict_index(e);
    if (k >= 1 && k <= (unsigned)*(int *)SVS_MAXCLIENTS)
        memset(v + 0x1d0, 0, 4);
}

static unsigned long long hash_entity(unsigned long long h, BYTE *e, int i)
{
    BYTE v[EV_SIZE];

    canon_entvars(v, e);
    h = fnv(h, (BYTE *)&i, 4);
    return fnv(h, v, EV_SIZE);
}

static DWORD str_lo[8], str_hi[8];
static int str_next;

static const char *string_at(int ofs)
{
    const char *base = *(const char **)STRING_BASE;
    const char *s = base + ofs;
    DWORD a = (DWORD)(ULONG_PTR)s;
    MEMORY_BASIC_INFORMATION mbi;
    int i;

    if (!ofs)
        return "";
    for (i = 0; i < 8; i++)
        if (a >= str_lo[i] && a < str_hi[i])
            return s;
    if (!readable(a, 1) || !VirtualQuery(s, &mbi, sizeof(mbi)))
        return "";
    i = str_next++ & 7;
    str_lo[i] = (DWORD)(ULONG_PTR)mbi.BaseAddress;
    str_hi[i] = str_lo[i] + (DWORD)mbi.RegionSize;
    return s;
}

static const char *classname(BYTE *e)
{
    return string_at(*(int *)(e + EV));
}

static BOOL dumped(BYTE *e, log_ent_t *r, int i)
{
    if (*(int *)e)
        return FALSE;
    r->index = i;
    r->movetype = *(int *)(e + EV_MOVETYPE);
    memcpy(r->origin, e + EV_ORIGIN, 12);
    memcpy(r->velocity, e + EV_VELOCITY, 12);
    memcpy(r->angles, e + EV_ANGLES, 12);
    memcpy(r->avelocity, e + EV_AVELOCITY, 12);
    r->nextthink = *(float *)(e + EV_NEXTTHINK);
    r->health = *(float *)(e + EV_HEALTH);
    return r->movetype || r->velocity[0] || r->velocity[1] || r->velocity[2] || r->avelocity[0] ||
           r->avelocity[1] || r->avelocity[2];
}

static void log_entities(void)
{
    int n = *(int *)SV_NUM_EDICTS, i, count = 0;
    char hdr[8] = "E";
    log_ent_t r;

    for (i = 1; i < n && i < 2048; i++) {
        BYTE *e = edict(i);
        const char *cn;

        if (!dumped(e, &r, i))
            continue;
        count++;
        cn = classname(e);
        if (strncmp(bench.classname[i], cn, sizeof(bench.classname[i]) - 1)) {
            char c[40] = "C";
            lstrcpynA(bench.classname[i], cn, sizeof(bench.classname[i]));
            memcpy(c + 4, &i, 4);
            lstrcpynA(c + 8, cn, 32);
            log_write(c, sizeof(c));
        }
    }
    memcpy(hdr + 4, &count, 4);
    log_write(hdr, 8);
    for (i = 1; i < n && i < 2048; i++)
        if (dumped(edict(i), &r, i))
            log_write(&r, sizeof(r));
}

static void log_full(int idx)
{
    int n = *(int *)SV_NUM_EDICTS, i;

    for (i = 0; i < n; i++) {
        BYTE *e = edict(i), v[EV_SIZE];
        char hdr[16] = "F";

        if (*(int *)e)
            continue;
        canon_entvars(v, e);
        memcpy(hdr + 4, &idx, 4);
        memcpy(hdr + 8, &i, 4);
        lstrcpynA(hdr + 12, "", 4);
        log_write(hdr, 16);
        log_write(v, EV_SIZE);
    }
}

static void log_frame(int idx)
{
    double t = *(double *)SV_TIME;
    BYTE *p = player(), *g;
    log_tick_t r;
    int n, i;
    unsigned long long h = 0xcbf29ce484222325ULL;

    if (!bench.log || !*(int *)SV_ACTIVE || (bench.state == BENCH_WATCH && !bench.watch_started))
        return;
    if (strncmp(bench.log_map, (char *)SV_NAME, sizeof(bench.log_map)) || t < bench.log_time) {
        if (bench.state == BENCH_WATCH) {

            if (bench.log_map[0])
                bench.sec++;
            bench.watch_t0 = t;
            fflush(bench.log);
        }
        lstrcpynA(bench.log_map, (char *)SV_NAME, sizeof(bench.log_map));
        memset(bench.classname, 0, sizeof(bench.classname));
        log_map();
        bench.dump_slot = -1;
    } else if (t == bench.log_time)
        return;
    bench.log_time = t;
    if (bench.state == BENCH_WATCH)
        idx = (int)floor((t - bench.watch_t0) / tick_sec + 0.5);
    memset(&r, 0, sizeof(r));
    r.type = 'T';
    r.seg = bench.seg;
    r.sec = bench.sec;
    r.idx = idx;
    r.time = t;
    r.frametime = *(float *)0x02517d64;
    if (p) {
        memcpy(r.origin, p + EV_ORIGIN, 12);
        memcpy(r.velocity, p + EV_VELOCITY, 12);
        memcpy(r.basevelocity, p + EV_BASEVELOCITY, 12);
        memcpy(r.v_angle, p + EV_V_ANGLE, 12);
        memcpy(r.punchangle, p + EV_PUNCHANGLE, 12);
        r.view_ofs_z = *(float *)(p + EV_VIEW_OFS + 8);
        r.health = *(float *)(p + EV_HEALTH);
        r.flags = *(int *)(p + EV_FLAGS);
        r.button = *(int *)(p + EV_BUTTON);
        r.waterlevel = *(int *)(p + EV_WATERLEVEL);
        r.movetype = *(int *)(p + EV_MOVETYPE);
        g = *(BYTE **)(p + EV_GROUNDENTITY);
        r.ground = edict_index(g);
    }
    n = *(int *)SV_NUM_EDICTS;
    for (i = 0; i < n; i++) {
        BYTE *e = edict(i);
        if (*(int *)e)
            continue;
        h = hash_entity(h, e, i);
        r.nents++;
        if (ent_log == -1) {
            vkgl_log("eh %.4f %d %08x", t, i, (unsigned)hash_entity(1469598103934665603ull, e, i));
        } else if (ent_log == i) {
            BYTE v[EV_SIZE];
            char hex[2 * 256 + 1];
            int k, o;
            canon_entvars(v, e);

            for (o = 0; o < EV_SIZE; o += 256) {
                for (k = 0; k < 256 && o + k < EV_SIZE; k++)
                    sprintf(hex + 2 * k, "%02x", v[o + k]);
                vkgl_log("ev %.4f %d %x %s", t, i, o, hex);
            }
        }
    }
    r.hash = h;
    log_write(&r, sizeof(r));
    if ((int)floor(t * 2) != bench.dump_slot) {
        bench.dump_slot = (int)floor(t * 2);
        log_entities();
    }
    if (bench.full_dump && idx >= 0) {
        BOOL dump = idx < 3;
        for (n = 0; n < bench.ndump_idx; n++)
            dump |= idx == bench.dump_idx[n];
        if (dump)
            log_full(idx);
    }
}

static float up_float(double s)
{
    float f = (float)s;

    return (double)f < s ? nextafterf(f, 1) : f;
}

static float frame_time(double fps)
{
    return up_float(1.0 / fps);
}

static float bench_frand(void)
{
    bench.rand ^= bench.rand << 13;
    bench.rand ^= bench.rand >> 17;
    bench.rand ^= bench.rand << 5;
    return (bench.rand >> 8) / 16777216.0f;
}

static void bench_seed(int salt)
{
    rng_t r;

    rng_seed(&r, 777 + salt);
    if (rng_aside)
        rng_shared = r;
    else
        rng_set(&r);
    rng_seed(&rng_draw, 12345 + salt);
}

static void bench_start_segment(void)
{
    bench_seg_t *g;

    if (bench.seg >= bench.nsegs) {
        bench.state = BENCH_DONE;
        if (bench.log) {
            char end[8] = "X";
            log_write(end, 8);
            fclose(bench.log);
            bench.log = NULL;
        }
        vkgl_log("bench: done");
        bench_text("quit\n");
        return;
    }
    g = &bench.segs[bench.seg];
    bench.sec = 0;
    bench.seg_started = 0;
    bench.state = BENCH_LOAD;
    bench_seed(bench.seg);
    vkgl_log("bench: segment %d (%s), map %s", bench.seg, g->name, g->secs[0].map);
    bench_text("sv_cheats 1\nmap %s\n", g->secs[0].map);
}

static void bench_frame_begin(void)
{
    if (bench.state == BENCH_OFF)
        return;
    if (bench.state == BENCH_WATCH) {
        double t = *(double *)SV_TIME;
        if (bench.log && (t >= bench.next_report || t < bench.next_report - 2)) {
            bench.next_report = t + 1;
            fflush(bench.log);
            if (rng_log)
                fflush(rng_log);
            if (rng_rec_file)
                fflush(rng_rec_file);
        }
        return;
    }

    if (bench.fps_hi > 0)
        *(float *)HOST_FRAMERATE = frame_time(bench.fps_lo + (bench.fps_hi - bench.fps_lo) * bench_frand());
    else if (bench.fps_lo > 0)
        *(float *)HOST_FRAMERATE = frame_time(bench.fps_lo);
    if (bench.debug && !(++bench.debug_frames % 500))
        vkgl_log("bench: frame %d state %d cls %d sv %d paused %d time %.3f mode %d acc %.2f ft %.6f "
                 "decided %u cmds %u", bench.debug_frames, bench.state, *(int *)CLS_STATE, *(int *)SV_ACTIVE,
                 *(int *)SV_PAUSED, *(double *)SV_TIME, frame_mode, tick_acc, *(double *)HOST_FRAMETIME,
                 decided_frame, tick_count);
    if (bench.state == BENCH_START) {

        if (++bench.wait_frames > 30)
            bench_start_segment();
        return;
    }
    if (bench.state == BENCH_RUN || bench.state == BENCH_CHANGE || bench.state == BENCH_LOAD) {
        double t = *(double *)SV_TIME;
        if (t >= bench.next_report) {
            bench.next_report = t + 60;
            if (bench.log)
                fflush(bench.log);
        }
    }
}

static bench_sec_t *cur_sec(void)
{
    return &bench.segs[bench.seg].secs[bench.sec];
}

static int find_sec(const char *map)
{
    bench_seg_t *g = &bench.segs[bench.seg];
    int c;

    for (c = bench.sec; c < g->nsecs; c++)
        if (!strcmp(g->secs[c].map, map))
            return c;
    return -1;
}

static void begin_section(int c)
{
    bench.sec = c;
    bench.state = BENCH_RUN;
    bench.t0 = *(double *)SV_TIME;
    bench.last_idx = -1;
    bench.applied_idx = -1;
    vkgl_log("bench: segment %d section %d on %s at %.3f", bench.seg, c, cur_sec()->map, bench.t0);
}

static const bench_cmd_t *bench_pick(int *idx)
{
    const char *map = (const char *)SV_NAME;
    int c, i;

    *idx = -1;
    if (bench.state < BENCH_LOAD || bench.state == BENCH_DONE || !client_active() || !player())
        return NULL;
    if (bench.state == BENCH_LOAD || strcmp(cur_sec()->map, map)) {
        c = find_sec(map);
        if (c < 0) {
            if (bench.state == BENCH_LOAD)
                return NULL;

            if (bench.sec + 1 >= bench.segs[bench.seg].nsecs) {
                vkgl_log("bench: unexpected level %s, segment over", map);
                bench.seg++;
                bench_start_segment();
                return NULL;
            }
            bench.sec++;
            vkgl_log("bench: unexpected level %s, loading %s", map, cur_sec()->map);
            bench.state = BENCH_LOAD;
            bench_text("map %s\n", cur_sec()->map);
            return NULL;
        }
        begin_section(c);
    }
    if (bench.state == BENCH_CHANGE) {
        bench_seg_t *g = &bench.segs[bench.seg];
        if (*(double *)SV_TIME < bench.deadline)
            return NULL;
        if (bench.sec + 1 < g->nsecs) {
            vkgl_log("bench: no level change, loading %s", g->secs[bench.sec + 1].map);
            bench.sec++;
            bench.state = BENCH_LOAD;
            bench_text("map %s\n", cur_sec()->map);
        } else {
            bench.seg++;
            bench_start_segment();
        }
        return NULL;
    }
    i = (int)floor((*(double *)SV_TIME - bench.t0) / tick_sec + 0.5);
    if (i >= cur_sec()->ncmds) {

        bench.state = BENCH_CHANGE;
        bench.deadline = *(double *)SV_TIME + 1.0;
        return NULL;
    }
    *idx = i;
    return &cur_sec()->cmds[i];
}

static void bench_cmd(BYTE *cmd, int *idx_out)
{
    int idx;
    const bench_cmd_t *c = bench_pick(&idx);

    *idx_out = idx;
    if (!c) {
        if (bench.state >= BENCH_LOAD && bench.state != BENCH_DONE) {

            memset(cmd + CMD_MOVE, 0, 12);
            *(WORD *)(cmd + CMD_BUTTONS) = 0;
            cmd[CMD_IMPULSE] = 0;
            cmd[CMD_WEAPONSELECT] = 0;
        }
        return;
    }
    memcpy(cmd + CMD_ANGLES, c->angles, 12);
    memcpy(cmd + CMD_MOVE, c->move, 12);
    *(WORD *)(cmd + CMD_BUTTONS) = c->buttons;
    cmd[CMD_LIGHTLEVEL] = c->lightlevel;
    cmd[CMD_WEAPONSELECT] = c->weaponselect;
    cmd[CMD_IMPULSE] = c->impulse;
    if (!bench.seg_started) {
        bench.seg_started = 1;
        if (!c->impulse)
            cmd[CMD_IMPULSE] = 101;
    }
    bench.last_idx = idx;
}

static void bench_before_server(int idx)
{
    const bench_cmd_t *c;
    BYTE *p = player();

    if (idx < 0 || idx == bench.applied_idx || !p || bench.state != BENCH_RUN)
        return;
    bench.applied_idx = idx;
    c = &cur_sec()->cmds[idx];
    if (idx == 0 || (c->resync && !(*(int *)(p + EV_FLAGS) & FL_DUCKING))) {
        memcpy(p + EV_ORIGIN, c->origin, 12);
        memcpy(p + EV_VELOCITY, c->velocity, 12);
        if (idx == 0)
            *(float *)(p + EV_HEALTH) = 10000;
    }
}

static void log_stack(const CONTEXT *c)
{
    const DWORD *sp = (const DWORD *)(ULONG_PTR)c->Esp;
    int i, n = 0;

    for (i = 0; i < 512 && n < 24 && readable((DWORD)(ULONG_PTR)(sp + i), 4); i++) {
        MEMORY_BASIC_INFORMATION mi;
        DWORD v = sp[i];

        if (v < 0x10000 || !VirtualQuery((void *)(ULONG_PTR)v, &mi, sizeof(mi)) ||
            !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        vkgl_log("bench:   [esp+%03x] %08lx (module %08lx)", i * 4, v, (DWORD)(ULONG_PTR)mi.AllocationBase);
        n++;
    }
}

static LONG CALLBACK bench_crash(EXCEPTION_POINTERS *ep)
{
    const CONTEXT *c = ep->ContextRecord;

    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    vkgl_log("bench: access violation at %08lx (%s %08lx), esp %08lx, sv.time %.3f, map %s",
             c->Eip, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
             (DWORD)ep->ExceptionRecord->ExceptionInformation[1], c->Esp, *(double *)SV_TIME,
             (const char *)SV_NAME);
    log_stack(c);
    return EXCEPTION_CONTINUE_SEARCH;
}

static HANDLE bench_main;

static DWORD WINAPI bench_watchdog(void *arg)
{
    unsigned last = ~0u;
    int still = 0, reports = 0;

    (void)arg;
    while (reports < 3) {
        CONTEXT c;

        Sleep(1000);
        if (frame_no != last || *(int *)CLS_STATE != 5) {
            last = frame_no;
            still = 0;
            continue;
        }
        if (++still < 5)
            continue;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(bench_main) == (DWORD)-1)
            break;
        if (GetThreadContext(bench_main, &c)) {
            vkgl_log("bench: no frame for %d s: eip %08lx esp %08lx, sv.time %.3f, map %s", still, c.Eip, c.Esp,
                     *(double *)SV_TIME, (const char *)SV_NAME);
            log_stack(&c);
        }
        ResumeThread(bench_main);
        reports++;
    }
    return 0;
}

static void bench_setup(void)
{
    char path[MAX_PATH], buf[64] = "";
    BOOL watch = FALSE;

    if (!GetEnvironmentVariableA("VKGL_BENCH_SCRIPT", path, sizeof(path))) {
        if (!GetEnvironmentVariableA("VKGL_BENCH_LOG", NULL, 0) && !GetEnvironmentVariableA("VKGL_BENCH_LOGSPLIT", NULL, 0))
            return;
        watch = TRUE;
    } else if (!bench_load(path))
        return;
    if (watch && GetEnvironmentVariableA("VKGL_BENCH_PRE", path, sizeof(path)))
        bench.pre_ft = (float)atof(path);
    if (watch && GetEnvironmentVariableA("VKGL_BENCH_FPS", path, sizeof(path)) && atof(path) > 0) {
        bench.watch_ft = frame_time(atof(path)) * 1000.0;
        bench.watch_nominal = 1000.0 / atof(path);
    }

    if (watch && GetEnvironmentVariableA("VKGL_BENCH_FT0", path, sizeof(path)) && atof(path) > 0) {
        bench.watch_ft = atof(path) * 1000.0;
        bench.watch_nominal = 0;
    }

    bench.exact_view = watch && GetEnvironmentVariableA("VKGL_BENCH_EXACTVIEW", NULL, 0) > 0;
    if (GetEnvironmentVariableA("VKGL_BENCH_FPS", buf, sizeof(buf)) && !watch) {
        char *dash = strchr(buf, '-');
        bench.fps_lo = (float)atof(buf);
        bench.fps_hi = dash ? (float)atof(dash + 1) : 0;
    }
    bench.rand = 0x9e3779b9;
    bench.debug = GetEnvironmentVariableA("VKGL_BENCH_DEBUG", NULL, 0) > 0;
    bench.full_dump = GetEnvironmentVariableA("VKGL_BENCH_FULLDUMP", NULL, 0) > 0;
    if (GetEnvironmentVariableA("VKGL_BENCH_DUMPIDX", path, sizeof(path))) {
        char *q = path;
        while (*q && bench.ndump_idx < 16) {
            bench.dump_idx[bench.ndump_idx++] = strtol(q, &q, 10);
            while (*q == ',')
                q++;
        }
    }
    bench_mouse = GetEnvironmentVariableA("VKGL_BENCH_MOUSE", NULL, 0) > 0;
    bench_norender = GetEnvironmentVariableA("VKGL_BENCH_NORENDER", NULL, 0) > 0;
    if (GetEnvironmentVariableA("VKGL_BENCH_VCLOCK", path, sizeof(path))) {
        vclock = TRUE;
        vclock_jitter = atof(path) > 1 ? atof(path) * 1e-6 : 0;
    }
    if (GetEnvironmentVariableA("VKGL_BENCH_EVENTS", path, sizeof(path)))
        events_load(path);
    if (GetEnvironmentVariableA("VKGL_BENCH_ENTLOG", path, sizeof(path)))
        ent_log = atoi(path);
    if (GetEnvironmentVariableA("VKGL_BENCH_VIEWLOG", path, sizeof(path)) && (view_log = fopen(path, "w")))
        setvbuf(view_log, NULL, _IONBF, 0);
    if (GetEnvironmentVariableA("VKGL_BENCH_TRACE", path, sizeof(path)))
        trace_on = atoi(path) > 2 ? 3 : atoi(path) > 1 ? 2 : 1;
    AddVectoredExceptionHandler(1, bench_crash);
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &bench_main, 0, FALSE,
                        DUPLICATE_SAME_ACCESS))
        CloseHandle(CreateThread(NULL, 0, bench_watchdog, NULL, 0, NULL));
    memcpy(log_hdr, "VKPHYS1", 8);
    lstrcpynA(log_hdr + 8, buf, 24);
    if (GetEnvironmentVariableA("VKGL_BENCH_LOGSPLIT", path, sizeof(path)))
        splits_load(path);
    else if (GetEnvironmentVariableA("VKGL_BENCH_LOG", path, sizeof(path)))
        log_open(path);
    bench.state = watch ? BENCH_WATCH : BENCH_START;
    if (watch)
        vkgl_log("bench: watching (%s), log %s", buf, bench.log ? path : "none");
    else
        vkgl_log("bench: %d segments, fps %s, log %s", bench.nsegs, buf, bench.log ? path : "none");
}

static void log_tick_rate(void)
{
    static DWORD since;
    static unsigned frames, cmds;
    DWORD now = GetTickCount();

    frames++;
    if (!since) {
        since = now;
        cmds = tick_count;
    } else if (now - since >= 5000) {
        vkgl_log("engine: %.0f fps, %.1f commands/s", frames * 1000.0 / (now - since),
                 (tick_count - cmds) * 1000.0 / (now - since));
        since = now;
        frames = 0;
        cmds = tick_count;
    }
}

static int decide(void)
{
    double ft = *(double *)HOST_FRAMETIME * 1000.0, raw_ft = ft;
    BOOL fps_drop;

    if (frame_no == decided_frame)
        return frame_mode;
    decided_frame = frame_no;

    if (bench.state == BENCH_WATCH) {
        if (*(int *)CLS_STATE != 5 || strncmp(bench.watch_map, (char *)SV_NAME, sizeof(bench.watch_map)) ||
            *(double *)SV_TIME < bench.watch_last_t)
            bench.watch_started = bench.watch_armed = FALSE;
        if (*(int *)CLS_STATE != 5)
            bench.watch_loaded = TRUE;
        bench.watch_last_t = *(double *)SV_TIME;
        lstrcpynA(bench.watch_map, (char *)SV_NAME, sizeof(bench.watch_map));
    }
    if (bench.state == BENCH_WATCH && !bench.watch_started && *(int *)CLS_STATE == 5 &&
        fabs(ft - bench.pre_ft * 1000.0) < 1e-4)
        bench.watch_armed = TRUE;

    if (bench.state == BENCH_WATCH && !bench.watch_started && *(int *)CLS_STATE == 5 &&
        (bench.watch_ft && bench.pre_ft ? bench.watch_armed && fabs(ft - bench.watch_ft) < 1e-4
                                        : fabs(ft - bench.pre_ft * 1000.0) > 1e-4)) {
        bench.watch_started = TRUE;
        bench.watch_loaded = FALSE;
        if (!nsplits)
            events_start();
        else if ((cur_split < 0 || bench.watch_starts == split_end) && cur_split + 1 < nsplits &&
                 split_level_ok(&splits[cur_split + 1])) {
            split_next();
            events_start();
        } else if (cur_split >= 0 && split_end == bench.watch_starts)
            split_end++;
        bench.watch_starts++;
        tick_acc = 0;
        if (level_frame != frame_no)
            ft_restart = TRUE;
        vkgl_log("bench: script started at %.3f, frame time %.6f ms", *(double *)SV_TIME, ft);
    }
    if (!fix_on)
        return frame_mode = FRAME_SLOW;
    if (tick_stats)
        log_tick_rate();

    if (bench.state == BENCH_WATCH && bench.watch_started && bench.watch_nominal && bench.pre_ft &&
        fabs(ft - bench.watch_ft) < 1e-4)
        ft = bench.watch_nominal;

    if (level_frame == frame_no) {
        tick_acc = world_acc = world_slow = 0;
        slow_msec = raw_ft < 255 ? (int)raw_ft : 255;
        slow_turn = -1;
        return frame_mode = FRAME_SLOW;
    }

    {
        double a = ft < 2 * TICK ? ft : 2 * TICK;
        BOOL low = fps_max_ok && *(float *)FPS_MAX <= 100 && (bench_play || *(float *)HOST_FRAMERATE == 0);

        fps_drop = fast && low;
        if (low) {
            fast = FALSE;
            low_restart = TRUE;
        } else if (!(low_restart && ft >= TICK)) {
            if (low_restart)
                ft_restart = TRUE;
            low_restart = FALSE;
            ft_avg = ft_restart ? a : ft_avg + (a - ft_avg) * 0.1;
            ft_restart = FALSE;
            if (fast ? ft_avg > FAST_LEAVE : ft_avg < FAST_ENTER)
                fast = !fast;
        }
    }

    if (*(int *)CLS_STATE != 5) {
        level_reconnect = FALSE;
        if (*(int *)CLS_STATE <= 1)
            level_host_loading = FALSE;
        tick_acc = world_acc = world_slow = 0;

        memset(kb_marks, 0, sizeof(kb_marks));
        tick_pressed = tick_impulse = tick_weapon = 0;
        return frame_mode = FRAME_SLOW;
    }

    if (level_reconnecting() && raw_ft < TICK) {
        float host_ft = *(float *)HOST_FRAMERATE;
        BOOL host_fast = !bench_play && host_ft > 0 && host_ft * 1000.0 < FAST_ENTER;

        if (fast || host_fast) {
            level_reconnect_steps++;
            tick_acc = world_acc = world_slow = 0;
            tick_frac = 1;
            cmd_ahead += TICK - raw_ft;
            return frame_mode = FRAME_TICK;
        }
    }
    tick_acc += ft;
    world_acc += *(double *)HOST_FRAMETIME;
    world_slow = 0;

    if (fps_drop) {
        frame_mode = FRAME_TICK;
        if (tick_acc >= TICK - 1e-9) {

            double over = tick_acc - TICK > 0 ? tick_acc - TICK : 0;

            tick_frac = (TICK - (tick_acc - ft)) / ft;
            if ((1 - tick_frac) * ft < 1e-3)
                tick_frac = 1;
            else if (tick_frac < 0)
                tick_frac = 0;
            if (over > 0) {
                if (vclock)
                    drop_carry = over;
                else
                    *(double *)OLDREALTIME -= over / 1000.0;
            }
        } else {

            tick_frac = 1;
            cmd_ahead += TICK - tick_acc;
        }
        tick_acc = world_acc = 0;
    } else if (!fast || tick_acc - TICK > 2 * TICK) {
        double t = tick_acc - ft > 1e-6 ? tick_acc : raw_ft;

        frame_mode = FRAME_SLOW;
        slow_msec = t < 255 ? (int)t : 255;
        slow_turn = tick_acc - ft > 1e-6 ? (float)(tick_acc / 1000.0) : -1;
        tick_acc = 0;
        world_slow = world_acc;
        world_acc = 0;
    } else if (tick_acc >= TICK - 1e-9) {
        frame_mode = FRAME_TICK;
        world_acc -= tick_sec;
        tick_frac = (TICK - (tick_acc - ft)) / ft;
        if ((1 - tick_frac) * ft < 1e-3)
            tick_frac = 1;
        else if (tick_frac < 0)
            tick_frac = 0;
        tick_acc -= TICK;
    } else
        frame_mode = FRAME_SKIP;
    return frame_mode;
}

static int frame_idx = -1;

static void trace(const char *what, int v)
{
    if (!trace_on || trace_on == 3)
        return;
    if (*(int *)CLS_STATE != 5 || trace_on > 1)
        trace_left = 150;
    if (trace_left) {
        BYTE *p = player();
        float *o = p ? (float *)(p + EV_ORIGIN) : NULL, *u = p ? (float *)(p + EV_VELOCITY) : NULL;

        vkgl_log("trace: frame %u cls %d mode %d %s %d sv.time %.4f realtime %.4f bench %d/%d "
                 "org %.2f %.2f %.2f vel %.2f %.2f %.2f hp %.0f", frame_no, *(int *)CLS_STATE, frame_mode, what,
                 v, *(double *)SV_TIME, *(double *)REALTIME, bench.state, frame_idx,
                 o ? o[0] : 0, o ? o[1] : 0, o ? o[2] : 0, u ? u[0] : 0, u ? u[1] : 0, u ? u[2] : 0,
                 p ? *(float *)(p + EV_HEALTH) : 0);
    }
}

static int __cdecl tick_cmd(BYTE *cmd)
{
    double ft = *(const double *)HOST_FRAMETIME * 1000.0;
    WORD *buttons = (WORD *)(cmd + CMD_BUTTONS);
    float *move = (float *)(cmd + CMD_MOVE), *ang = (float *)(cmd + CMD_ANGLES);
    int mode = decide(), msec, a;

    if (!fix_on) {
        msec = ft < 255 ? (int)ft : 255;
    } else {

        tick_pressed |= *buttons & ~tick_held & ~kb_bits;
        tick_held = *buttons;
        if (mode == FRAME_SKIP) {
            if (cmd[CMD_IMPULSE])
                tick_impulse = cmd[CMD_IMPULSE];
            if (cmd[CMD_WEAPONSELECT])
                tick_weapon = cmd[CMD_WEAPONSELECT];
            return -1;
        }
        if (mode == FRAME_TICK) {
            msec = TICK;
            tick_regular = TRUE;

            if (tick_frac < 1 && !bench.exact_view && !memcmp(ang, (float *)CL_VIEWANGLES, 12))
                for (a = 0; a < 3; a++) {
                    float from = adj_frame == frame_no ? adj_after[a] : view_start[a];
                    float d = ang[a] - from;

                    d -= 360.0f * floorf((d + 180.0f) / 360.0f);
                    ang[a] = from + d * (float)tick_frac;
                }
        } else if (*(int *)CLS_STATE != 5) {

            msec = ft < 255 ? (int)ft : 255;
            if ((fast || level_host_rate_fast()) && ft < TICK && *(int *)SVS_MAXCLIENTS == 1 &&
                level_frame != frame_no) {
                msec = TICK;
                cmd_ahead += TICK - ft;
            }
            tick_regular = FALSE;
        } else {
            msec = slow_msec;
            tick_regular = FALSE;
        }
        *buttons |= tick_pressed;
        if (!cmd[CMD_IMPULSE])
            cmd[CMD_IMPULSE] = tick_impulse;
        if (!cmd[CMD_WEAPONSELECT])
            cmd[CMD_WEAPONSELECT] = tick_weapon;
        tick_pressed = tick_impulse = tick_weapon = 0;
        memset(kb_marks, 0, sizeof(kb_marks));
    }
    if (bench.state != BENCH_OFF && bench.state != BENCH_WATCH)
        bench_cmd(cmd, &frame_idx);
    if (trace_on == 3)
        vkgl_log("cmd: frame %u ft %.6f mode %d move %.1f %.1f %.1f btn %x ang %.4f %.4f", frame_no, ft, mode, move[0],
                 move[1], move[2], *buttons, ang[0], ang[1]);
    tick_count++;
    trace("cmd msec", msec);
    return msec;
}

static void client_module(DWORD id[3])
{
    BYTE *base = (BYTE *)GetModuleHandleA("client.dll");
    IMAGE_NT_HEADERS *nt;

    memset(id, 0, 3 * sizeof(DWORD));
    if (!base)
        return;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    id[0] = (DWORD)(ULONG_PTR)base;
    id[1] = nt->FileHeader.TimeDateStamp;
    id[2] = nt->OptionalHeader.SizeOfImage;
}

#define ADJ_LEN 56
static const BYTE adj_pat[ADJ_LEN] = {
    0xf6, 0x05, 0, 0, 0, 0, 0x01, 0x74, 0x12, 0xa1, 0, 0, 0, 0, 0xd9, 0x44, 0x24, 0x04, 0xd8, 0x48, 0x0c, 0xd9,
    0x5c, 0x24, 0x04, 0xeb, 0x08, 0x8b, 0x4c, 0x24, 0x04, 0x89, 0x4c, 0x24, 0x04, 0xa0, 0, 0, 0, 0, 0x56, 0x8b,
    0x74, 0x24, 0x0c, 0xa8, 0x01, 0x75, 0x4d, 0x68, 0, 0, 0, 0, 0xe8, 0 };
static const char adj_any[] = "..xxxx....xxxx......................xxxx..........xxxx.x";
static void(__cdecl *adj_orig)(float, float *);
static BYTE *adj_fn;

static void __cdecl adjust_hook(float ft, float *va)
{
    int mode;

    if (!fix_on || *(int *)CLS_STATE != 5) {
        adj_orig(ft, va);
        return;
    }
    mode = decide();
    if (mode == FRAME_SKIP)
        return;
    if (mode == FRAME_TICK) {
        adj_orig((float)tick_sec, va);
        memcpy(adj_after, va, sizeof(adj_after));
        adj_frame = frame_no;
        return;
    }
    adj_orig(slow_turn >= 0 ? slow_turn : ft, va);
}

static void find_adjust(BYTE *const *lo, BYTE *const *hi, const DWORD *flags, int nsec)
{
    static BYTE *thunk;
    BYTE *f = NULL, *q, jmp[7];
    int k, i, n = 0;

    adj_frame = ~0u;
    for (k = 0; k < nsec; k++) {
        if (!(flags[k] & IMAGE_SCN_MEM_EXECUTE))
            continue;
        for (q = lo[k]; q + ADJ_LEN <= hi[k]; q++) {
            for (i = 0; i < ADJ_LEN && (adj_any[i] == 'x' || q[i] == adj_pat[i]); i++)
                ;
            if (i == ADJ_LEN && n++ == 0)
                f = q;
        }
    }
    if (n != 1) {
        vkgl_log("engine: CL_AdjustAngles not found in client.dll (%d matches), turning keys turn every frame", n);
        return;
    }
    if (f == adj_fn || f[0] == 0xe9)
        return;
    if (!thunk && !(thunk = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        return;

    memcpy(thunk, f, 7);
    thunk[7] = 0xe9;
    *(DWORD *)(thunk + 8) = (DWORD)(ULONG_PTR)(f + 7) - (DWORD)(ULONG_PTR)(thunk + 12);
    FlushInstructionCache(GetCurrentProcess(), thunk, 12);
    adj_orig = (void *)thunk;
    jmp[0] = 0xe9;
    *(DWORD *)(jmp + 1) = (DWORD)(ULONG_PTR)adjust_hook - (DWORD)(ULONG_PTR)(f + 5);
    jmp[5] = jmp[6] = 0x90;
    write_code((DWORD)(ULONG_PTR)f, jmp, 7);
    adj_fn = f;
    vkgl_log("engine: CL_AdjustAngles in client.dll at %p: turning keys turn once a step", (void *)f);
}

#define INMOVE_LEN 18
static const BYTE inmove_pat[INMOVE_LEN] = { 0x56, 0x55, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x08, 0x8a, 0x15, 0, 0, 0, 0,
                                             0x6a, 0x01 };
static const char inmove_any[] = "...xxxx.....xxxx..";
static void(__cdecl *inmove_orig)(float, BYTE *);
static BYTE *inmove_site;
static float mouse_move[3];

static void __cdecl inmove_hook(float ft, BYTE *cmd)
{
    float *m = (float *)(cmd + CMD_MOVE), before[3];
    int i, mode;

    memcpy(before, m, sizeof(before));
    inmove_orig(ft, cmd);
    if (!fix_on || *(int *)CLS_STATE != 5) {
        memset(mouse_move, 0, sizeof(mouse_move));
        return;
    }
    mode = decide();
    for (i = 0; i < 3; i++) {
        float d = m[i] - before[i], carry = mouse_move[i];

        if (mode == FRAME_SKIP) {
            mouse_move[i] += d;
        } else if (mode == FRAME_TICK && tick_frac < 1 && d != 0) {
            m[i] = before[i] + carry + d * (float)tick_frac;
            mouse_move[i] = d - d * (float)tick_frac;
        } else {
            if (carry != 0)
                m[i] += carry;
            mouse_move[i] = 0;
        }
    }
}

static void find_inmove(BYTE *const *lo, BYTE *const *hi, const DWORD *flags, int nsec)
{
    BYTE *f = NULL, *q, b[5];
    int k, i, n = 0;

    for (k = 0; k < nsec; k++) {
        if (!(flags[k] & IMAGE_SCN_MEM_EXECUTE))
            continue;
        for (q = lo[k]; q + INMOVE_LEN <= hi[k]; q++) {
            for (i = 0; i < INMOVE_LEN && (inmove_any[i] == 'x' || q[i] == inmove_pat[i]); i++)
                ;
            if (i == INMOVE_LEN && n++ == 0)
                f = q + 2;
        }
    }
    if (n != 1) {
        vkgl_log("engine: CL_CreateMove's IN_Move call not found in client.dll (%d matches), the mouse's movement "
                 "between steps is lost", n);
        return;
    }
    if (f + 5 + *(int *)(f + 1) == (BYTE *)inmove_hook)
        return;
    inmove_orig = (void *)(f + 5 + *(int *)(f + 1));
    b[0] = 0xe8;
    *(DWORD *)(b + 1) = (DWORD)(ULONG_PTR)inmove_hook - (DWORD)(ULONG_PTR)(f + 5);
    write_code((DWORD)(ULONG_PTR)f, b, 5);
    inmove_site = f;
    memset(mouse_move, 0, sizeof(mouse_move));
    vkgl_log("engine: IN_Move call in client.dll at %p: the mouse's movement between steps goes into the step",
             (void *)f);
}

static void find_keys(void)
{
    static const char *const names[] = { "+forward", "+back", "+moveleft", "+moveright", "+moveup", "+movedown",
                                         "+jump", "+duck", "+use", "+attack", "+attack2", "+reload",
                                         "+left", "+right", "+lookup", "+lookdown" };
    static const WORD bits[] = { 0x8, 0x10, 0x200, 0x400, 0, 0, 0x2, 0x4, 0x20, 0x1, 0x800, 0x2000,
                                 0x80, 0x100, 0, 0 };
    BYTE *base, *lo[8], *hi[8];
    DWORD flags[8], keydown = 0;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int nsec = 0, n, k;

    kb_count = 0;
    kb_bits = 0;
    client_module(kb_module);
    if (!(base = (BYTE *)(ULONG_PTR)kb_module[0])) {
        vkgl_log("engine: client.dll not loaded, key marks not kept");
        return;
    }
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (k = 0; k < nt->FileHeader.NumberOfSections && nsec < 8; k++)
        if (readable((DWORD)(ULONG_PTR)(base + sec[k].VirtualAddress), sec[k].Misc.VirtualSize)) {
            lo[nsec] = base + sec[k].VirtualAddress;
            hi[nsec] = base + sec[k].VirtualAddress + sec[k].Misc.VirtualSize;
            flags[nsec++] = sec[k].Characteristics;
        }
    for (n = 0; n < (int)(sizeof(names) / sizeof(*names)); n++) {
        size_t len = strlen(names[n]) + 1;
        DWORD handler = 0, target;
        BYTE *q, *h = NULL;
        int *kb = NULL;

        for (k = 0; k < nsec && !handler; k++)
            for (q = lo[k]; q + len <= hi[k] && !handler; q++) {
                DWORD str = (DWORD)(ULONG_PTR)q;
                int j;
                BYTE *r;

                if (*q != '+' || (q > lo[k] && q[-1]) || memcmp(q, names[n], len))
                    continue;
                for (j = 0; j < nsec && !handler; j++)
                    for (r = lo[j] + 5; r + 7 <= hi[j] && !handler; r++)
                        if (r[0] == 0x68 && *(DWORD *)(r + 1) == str && r[-5] == 0x68 && r[5] == 0xff &&
                            r[6] == 0x15)
                            handler = *(DWORD *)(r - 4);
            }
        for (k = 0; k < nsec && handler; k++)
            if ((flags[k] & IMAGE_SCN_MEM_EXECUTE) && (BYTE *)(ULONG_PTR)handler >= lo[k] &&
                (BYTE *)(ULONG_PTR)handler + 10 <= hi[k])
                h = (BYTE *)(ULONG_PTR)handler;
        if (!h || h[0] != 0x68 || h[5] != 0xe8)
            continue;
        target = (DWORD)(ULONG_PTR)h + 10 + *(DWORD *)(h + 6);
        if (keydown && target != keydown)
            continue;
        for (k = 0; k < nsec; k++)
            if ((flags[k] & IMAGE_SCN_MEM_WRITE) && (BYTE *)(ULONG_PTR)*(DWORD *)(h + 1) >= lo[k] &&
                (BYTE *)(ULONG_PTR)*(DWORD *)(h + 1) + 12 <= hi[k])
                kb = (int *)(ULONG_PTR)*(DWORD *)(h + 1);
        if (!kb || (kb[2] & ~7))
            continue;
        keydown = target;
        kb_state[kb_count++] = kb + 2;
        kb_bits |= bits[n];
    }
    vkgl_log("engine: %d of %d key states found in client.dll", kb_count, (int)(sizeof(names) / sizeof(*names)));
    find_adjust(lo, hi, flags, nsec);
    find_inmove(lo, hi, flags, nsec);
}

static void keep_key_marks(BOOL entered)
{
    int i;

    if (entered && kb_count >= 0) {
        DWORD id[3];
        client_module(id);
        if (memcmp(id, kb_module, sizeof(id)))
            kb_count = -1;
    }
    if (kb_count < 0)
        find_keys();
    for (i = 0; i < kb_count; i++) {
        kb_marks[i] |= *kb_state[i] & 6;
        *kb_state[i] |= kb_marks[i];
    }
}

static void __cdecl cldata_hook(void)
{
    void(__cdecl *orig)(void) = (void *)CL_UPDATECLIENTDATA;
    int i;

    orig();
    for (i = 0; fix_on && i < kb_count; i++)
        if (!(*kb_state[i] & 7))
            kb_marks[i] = 0;
}

static void(__cdecl *const eng_get_view_angles)(float *) = (void *)ENG_GETVIEWANGLES;

static void __cdecl view_angles_hook(float *va)
{
    eng_get_view_angles(va);
    if (autofunc_armed && !autofunc_shown) {
        autofunc_real = *(double *)HOST_FRAMETIME;
        *(double *)HOST_FRAMETIME = tick_sec;
        autofunc_shown = TRUE;
    }
}

static void autofunc_restore(void)
{
    if (autofunc_shown) {
        *(double *)HOST_FRAMETIME = autofunc_real;
        autofunc_shown = autofunc_armed = FALSE;
    }
}

static void __cdecl cbuf_body(void)
{
    autofunc_restore();
}

static BOOL console_held(void)
{
    double ft, a, avg;
    BOOL f = fast;

    if (!fix_on || *(int *)CLS_STATE != 5 || frame_no == decided_frame || level_reconnecting())
        return FALSE;
    if (bench.state == BENCH_WATCH && !bench.watch_started)
        return FALSE;
    ft = *(double *)HOST_FRAMETIME * 1000.0;
    if (bench.state == BENCH_WATCH && bench.watch_started && bench.watch_nominal && bench.pre_ft &&
        fabs(ft - bench.watch_ft) < 1e-4)
        ft = bench.watch_nominal;
    if (fps_max_ok && *(float *)FPS_MAX <= 100 && (bench_play || *(float *)HOST_FRAMERATE == 0))
        return FALSE;
    a = ft < 2 * TICK ? ft : 2 * TICK;
    if (!(low_restart && ft >= TICK)) {
        avg = ft_restart || low_restart ? a : ft_avg + (a - ft_avg) * 0.1;
        if (f ? avg > FAST_LEAVE : avg < FAST_ENTER)
            f = !f;
    }
    return f && tick_acc + ft < TICK - 1e-9;
}

static BOOL tas_frames(void)
{
    return !bench_play && (*(float *)HOST_FRAMERATE != 0 || bench.state == BENCH_WATCH);
}

static void cbuf_run(void(__cdecl *orig)(void))
{
    autofunc_armed = autofunc_ft && fast && *(int *)CLS_STATE == 5 && (bench_play || *(float *)HOST_FRAMERATE == 0);
    in_cbuf = TRUE;
    orig();
    in_cbuf = FALSE;
    if (autofunc_shown) {
        autofunc_restore();
        autofunc_ft = FALSE;
        vkgl_log("engine: Cbuf_Execute's body was not reached after BXT's autofuncs, they keep the frame's time");
    }
    autofunc_armed = FALSE;
}

#define FPS_MODEM 0x01eca210
#define DEVELOPER 0x01eca310
#define CL_MAXCLIENTS 0x02f599c4
#define CAP_JMP 0x01d57092

static void vclock_frame(void)
{
    float fps;
    double h;

    if (*(int *)SV_ACTIVE) {
        fps = *(float *)FPS_MAX;
    } else {
        fps = *(float *)FPS_MODEM > 0 ? *(float *)FPS_MODEM : *(float *)FPS_MAX;
        if (*(int *)CLS_STATE != 1 && *(int *)CLS_STATE != 5)
            fps = 30;
    }
    if (fps < 0.5f)
        fps = 0.5f;
    if (*(float *)DEVELOPER == 0 && *(BYTE *)CAP_JMP != 0xeb && fps > 100)
        fps = 100;
    if (*(int *)CL_MAXCLIENTS > 1 && fps < 20)
        fps = 20;
    h = frame_time(fps);
    if (vclock_jitter > 0)
        h = up_float(h + vclock_jitter * bench_frand());
    if (h > 0.25)
        h = 0.25;
    if (h < 0.001)
        h = 0.001f;
    *(float *)HOST_FRAMERATE = (float)h;
}

typedef struct {
    double t;
    BOOL mid;
    char cmd[244];
} bench_event_t;
static bench_event_t *events;
static int nevents, next_event;
static BOOL ev_active, ev_waiting = TRUE;
static double ev_sum;
static unsigned ev_cmd0, ev_level = ~0u;
static unsigned ev_steps;
static BOOL ev_frozen, ev_loaded;
static double ev_t0;
static double ev_mouse[2], ev_mouse_acc[2];
static unsigned ev_steps_seen;
static double ev_sum_prev;
static int ev_mid_wait = -1;
static double ev_mid_done = -1;
static unsigned ev_mid_rand = 0x2545f491;

static void events_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[300];
    int cap = 0;

    if (!f) {
        vkgl_log("bench: cannot read %s", path);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *end;
        double t;
        size_t n;

        if (!strncmp(p, "@next", 5)) {
            t = -1;
            end = p;
        } else {
            t = strtod(p, &end);
            if (end == p || *p == '#')
                continue;
        }
        while (*end == ' ' || *end == '\t')
            end++;
        if (nevents == cap) {
            cap = cap ? cap * 2 : 64;
            events = realloc(events, cap * sizeof(*events));
        }
        events[nevents].mid = !strncmp(end, "@mid ", 5);
        if (events[nevents].mid)
            end += 5;
        n = strcspn(end, "\r\n");
        if (!n)
            continue;
        events[nevents].t = t < 0 ? (nevents ? events[nevents - 1].t : 0) : t;
        lstrcpynA(events[nevents].cmd, end, (int)(n < sizeof(events[0].cmd) ? n + 1 : sizeof(events[0].cmd)));
        nevents++;
    }
    fclose(f);
    vkgl_log("bench: %d scheduled commands", nevents);
}

static void events_start(void)
{
    if (!nevents || !ev_waiting)
        return;
    ev_waiting = FALSE;
    ev_active = TRUE;
    ev_sum = *(double *)HOST_FRAMETIME;
    ev_cmd0 = ev_steps_seen = ev_steps;
    ev_mid_wait = -1;
    ev_level = level_frame;
    vkgl_log("bench: schedule starts at sv.time %.6f", *(double *)SV_TIME);
}

static BOOL event_due(const bench_event_t *e, BOOL stepped)
{
    double h = *(double *)HOST_FRAMETIME, end;
    int n;

    if (ev_sum >= e->t)
        return TRUE;
    if (!e->mid || ev_frozen)
        return FALSE;
    if (e->t == ev_mid_done)
        return TRUE;
    if (ev_mid_wait < 0) {

        if (!stepped || ev_sum_prev < e->t - 0.01 - 1e-9)
            return FALSE;
        for (n = 0, end = ev_sum; end < e->t && n < 1000; end += h)
            n++;
        if (!n)
            return FALSE;
        ev_mid_rand ^= ev_mid_rand << 13;
        ev_mid_rand ^= ev_mid_rand >> 17;
        ev_mid_rand ^= ev_mid_rand << 5;
        ev_mid_wait = (int)(ev_mid_rand % (unsigned)n);
    }
    if (ev_mid_wait-- > 0)
        return FALSE;
    ev_mid_wait = -1;
    ev_mid_done = e->t;
    return TRUE;
}

static void events_frame(void)
{
    BOOL stepped;

    if (!ev_active)
        return;
    stepped = ev_steps != ev_steps_seen;
    ev_steps_seen = ev_steps;
    ev_mid_done = -1;

    if (level_frame != ev_level && level_frame < frame_no) {
        ev_level = level_frame;
        ev_sum = (ev_steps - ev_cmd0) * 0.01;
        ev_frozen = TRUE;
        ev_loaded = FALSE;
        ev_t0 = *(double *)SV_TIME;
        vkgl_log("bench: level started on frame %u, schedule clock %.2f s (%u steps)", level_frame, ev_sum,
                 ev_steps - ev_cmd0);
    }
    if (ev_frozen) {
        if (*(int *)CLS_STATE != 5)
            ev_loaded = TRUE;

        else if (ev_loaded)
            ev_frozen = FALSE;
        else if (*(double *)SV_TIME != ev_t0) {
            BYTE *p = player();

            if (p && (*(int *)(p + EV_FLAGS) & FL_CLIENT))
                ev_frozen = FALSE;
        }
    }
    ev_sum_prev = ev_sum;
    if (!ev_frozen) {
        ev_sum += *(double *)HOST_FRAMETIME - drop_carry / 1000.0;
        drop_carry = 0;
        if (ev_mouse[0] != 0 || ev_mouse[1] != 0) {
            LONG c[2];
            int k;

            for (k = 0; k < 2; k++) {
                ev_mouse_acc[k] += ev_mouse[k] * *(double *)HOST_FRAMETIME * 1000.0;
                c[k] = (LONG)floor(ev_mouse_acc[k] + 1e-9);
                ev_mouse_acc[k] -= c[k];
            }
            vkgl_mouse_feed(c[0], c[1]);
        }
    }
    while (ev_active && next_event < nevents && event_due(&events[next_event], stepped)) {
        const bench_event_t *e = &events[next_event++];

        if (!strncmp(e->cmd, "@mouse", 6)) {

            ev_mouse[0] = ev_mouse[1] = ev_mouse_acc[0] = ev_mouse_acc[1] = 0;
            sscanf(e->cmd + 6, "%lf %lf", &ev_mouse[0], &ev_mouse[1]);
            vkgl_log("bench: at %.4f s (sv.time %.4f, frame %u): the mouse moves %g %g a ms", e->t,
                     *(double *)SV_TIME, frame_no, ev_mouse[0], ev_mouse[1]);
            continue;
        }
        if (!strncmp(e->cmd, "@next", 5)) {
            ev_active = FALSE;
            ev_mouse[0] = ev_mouse[1] = 0;
            ev_waiting = TRUE;
            if (nsplits)
                split_end = bench.watch_starts;
            vkgl_log("bench: scenario ends at sv.time %.4f", *(double *)SV_TIME);
            break;
        }
        if (e->mid && ev_sum < e->t)
            vkgl_log("bench: at %.4f s, between steps at %.4f (sv.time %.4f, frame %u): %s", e->t, ev_sum,
                     *(double *)SV_TIME, frame_no, e->cmd);
        else
            vkgl_log("bench: at %.4f s (sv.time %.4f, frame %u): %s", e->t, *(double *)SV_TIME, frame_no, e->cmd);
        if (!strncmp(e->cmd, "quit", 4)) {

            rng_report();
            if (bench.log)
                fflush(bench.log);
            log_through = TRUE;
        }
        cbuf_addtext(e->cmd);
        cbuf_addtext("\n");
    }
}

static int __cdecl filter_hook(float time)
{
    int(__cdecl *orig)(float) = (void *)HOST_FILTERTIME;
    int r;

    if (vclock)
        vclock_frame();
    r = orig(time);

    if (r)
        rng_between(FALSE);
    return r;
}

static void __cdecl cbuf_hook(void)
{
    void(__cdecl *orig)(void) = (void *)CBUF_EXECUTE;
    static int last_cls;
    int cls0 = *(int *)CLS_STATE;
    double ft0, svt0, ft_in;

    frame_no++;
    frame_idx = -1;
    rng_between(FALSE);
    vkgl_menu_test_frame();
    if (bench_mouse)
        memcpy(view_start, (float *)CL_VIEWANGLES, sizeof(view_start));
    bench_frame_begin();
    events_frame();
    ft0 = *(double *)HOST_FRAMETIME;
    svt0 = *(double *)SV_TIME;
    ft_in = ft0;
    if (console_held()) {
        if (tas_frames()) {
            int n = *(int *)CMD_TEXT_CURSIZE, max = *(int *)CMD_TEXT_MAXSIZE, left;
            char *text = *(char **)CMD_TEXT_DATA;

            if (n > 0 && n <= (int)sizeof(held_text) && n <= max) {
                memcpy(held_text, text, n);
                *(int *)CMD_TEXT_CURSIZE = 0;
                cbuf_run(orig);

                text = *(char **)CMD_TEXT_DATA;
                left = *(int *)CMD_TEXT_CURSIZE > 0 ? *(int *)CMD_TEXT_CURSIZE : 0;
                if (n > max - left)
                    n = max - left;
                memcpy(text + left, held_text, n);
                *(int *)CMD_TEXT_CURSIZE = left + n;
            } else
                cbuf_run(orig);
        } else
            console_ft += ft0;
    } else {
        if (console_ft > 0) {
            ft_in = ft0 + console_ft;
            *(double *)HOST_FRAMETIME = ft_in;
            console_ft = 0;
        }
        cbuf_run(orig);
        if (ft_in != ft0 && *(double *)HOST_FRAMETIME == ft_in)
            *(double *)HOST_FRAMETIME = ft0;
    }

    if ((*(double *)HOST_FRAMETIME != ft0 && *(double *)HOST_FRAMETIME != ft_in) || *(double *)SV_TIME != svt0) {
        level_frame = frame_no;
        level_reconnect = fix_on && (last_cls == 5 || cls0 == 5) && *(int *)SVS_MAXCLIENTS == 1;
        level_reconnect_steps = 0;
        level_host_loading = level_reconnect && !bench_play;
    }
    if (*(int *)CLS_STATE != 5)
        console_ft = 0;

    if (!bench_mouse)
        memcpy(view_start, (float *)CL_VIEWANGLES, sizeof(view_start));
    if (fix_on && *(int *)CLS_STATE == 5) {
        if (last_cls != 5) {
            if (level_host_rate_fast())
                ft_restart = TRUE;
            level_host_loading = FALSE;
            first_step_draw = TRUE;
        }
        keep_key_marks(last_cls != 5);

        if (last_cls != 5) {
            static BOOL told;
            BOOL list = fps_max_ok && engine_cvar("fps_max") == (float *)FPS_MAX;

            pitch_up = list ? engine_cvar("cl_pitchup") : NULL;
            pitch_down = list ? engine_cvar("cl_pitchdown") : NULL;
            if ((!pitch_up || !pitch_down) && !told)
                vkgl_log("engine: cl_pitchup/cl_pitchdown not found, a pitch they set turns gradually");
            told = TRUE;
        }
        if (!bench_mouse && pitch_up && pitch_down) {
            if (view_start[0] > *pitch_down)
                view_start[0] = *pitch_down;
            if (view_start[0] < -*pitch_up)
                view_start[0] = -*pitch_up;
        }
    }
    last_cls = *(int *)CLS_STATE;
    if (fix_on && decide() == FRAME_SKIP)
        rng_between(TRUE);

    if (bench.state == BENCH_WATCH && !bench.watch_started && bench.pre_ft && *(float *)HOST_FRAMERATE == 0)
        *(float *)HOST_FRAMERATE = bench.pre_ft;
}

static long long bench_clock_us;

static double __cdecl bench_floattime(void)
{
    return 1000.0 + bench_clock_us / 1e6;
}

static BYTE floor_color[FLOOR_MAX][16];
static unsigned sv_ran = ~0u;

static struct {
    float prev[6], cur[6];
    int model;
} ent_step[FLOOR_MAX];
static float ent_drawn[FLOOR_MAX][6];
static short ent_moved[FLOOR_MAX];
static unsigned ent_seen = ~0u;

static int ents_between_steps(BYTE *ents, int n)
{
    BOOL step = ent_seen != tick_count, next = ent_seen + 1 == tick_count;
    BOOL on = interp_on && tick_regular && *(int *)SV_ACTIVE;
    float a = (float)(tick_acc / TICK), *o, d, d2;
    int i, k, moved = 0;

    ent_seen = tick_count;
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    for (i = *(int *)SVS_MAXCLIENTS + 1; i < n; i++) {
        BYTE *e = ents + i * CL_ENTITY_SIZE;
        int model = *(int *)(e + CL_STATE + ES_MODELINDEX);

        o = (float *)(e + CL_ORIGIN);
        if (step) {
            memcpy(ent_step[i].prev, next && ent_step[i].model == model ? ent_step[i].cur : o, 24);
            memcpy(ent_step[i].cur, o, 24);
            ent_step[i].model = model;
        }
        if (!on || !*(void **)(e + CL_MODEL) || memcmp(o, ent_step[i].cur, 24) ||
            *(int *)(e + CL_STATE + ES_MOVETYPE) == MOVETYPE_STEP ||
            *(int *)(e + CL_STATE + ES_MOVETYPE) == MOVETYPE_FOLLOW ||
            (*(int *)(e + CL_STATE + ES_EFFECTS) & EF_NOINTERP) || !memcmp(ent_step[i].prev, o, 24))
            continue;
        for (k = 0, d2 = 0; k < 3; k++)
            d2 += (o[k] - ent_step[i].prev[k]) * (o[k] - ent_step[i].prev[k]);
        for (k = 3; k < 6; k++) {
            d = o[k] - ent_step[i].prev[k];
            if (fabsf(d - 360.0f * floorf((d + 180.0f) / 360.0f)) > 45)
                break;
        }
        if (d2 > 64 * 64 || k < 6)
            continue;
        memcpy(ent_drawn[i], o, 24);
        for (k = 0; k < 6; k++) {
            d = o[k] - ent_step[i].prev[k];
            if (k >= 3)
                d -= 360.0f * floorf((d + 180.0f) / 360.0f);
            o[k] = ent_step[i].prev[k] + d * a;
        }
        ent_moved[moved++] = (short)i;
    }
    return moved;
}

static void __cdecl screen_hook(void)
{
    void(__cdecl *orig)(void) = (void *)HOST_UPDATESCREEN;
    BYTE *ents = *(BYTE **)CL_ENTITIES, *e;
    int n = 0, i, moved = 0;

    if (fix_on && first_step_draw && *(int *)CLS_STATE == 5) {
        if (sv_ran != frame_no && frame_mode == FRAME_SKIP)
            return;
        first_step_draw = FALSE;
    }

    if (bench_norender && sv_ran != frame_no && *(int *)CLS_STATE == 5)
        return;
    if (*(int *)CLS_STATE == 5 && ents) {
        n = *(int *)SV_NUM_EDICTS < FLOOR_MAX ? *(int *)SV_NUM_EDICTS : FLOOR_MAX;
        if (fix_on)
            moved = ents_between_steps(ents, n);
        if (sv_ran == frame_no)
            n = 0;
        for (i = 0, e = ents + CL_FLOORCOLOR; i < n; i++, e += CL_ENTITY_SIZE)
            memcpy(floor_color[i], e, 16);
    }
    orig();
    if (*(BYTE **)CL_ENTITIES != ents || *(int *)CLS_STATE != 5)
        return;
    for (i = 0; i < moved; i++)
        memcpy(ents + ent_moved[i] * CL_ENTITY_SIZE + CL_ORIGIN, ent_drawn[ent_moved[i]], 24);
    for (i = 0, e = ents + CL_FLOORCOLOR; i < n; i++, e += CL_ENTITY_SIZE)
        memcpy(e, floor_color[i], 16);
}

static int __cdecl illum_hook(BYTE *e)
{
    int i = edict_index(e), v, *c;

    if (!e)
        return -1;
    if (i <= *(int *)SVS_MAXCLIENTS)
        return *(int *)(e + EV + 0x124);
    if (*(int *)CLS_STATE < 3 || *(int *)CLS_STATE > 5)
        return 128;
    c = (int *)(*(BYTE **)CL_ENTITIES + i * CL_ENTITY_SIZE + CL_FLOORCOLOR);
    v = (c[0] + c[1] + c[2]) / 3;
    vkgl_log("illum: t %.4f frame %u ent %d %d (%d %d %d)", *(double *)SV_TIME, frame_no, i, v, c[0], c[1], c[2]);
    return v;
}

#define RANDOMFLOAT 0x01d79d70
#define RANDOMLONG 0x01d79da0
static const BYTE randomfloat_orig[] = "\x51\x8d\x44\x24\x0c";

static void rng_record(int kind, DWORD a, DWORD b)
{
    BYTE r[16];

    *(DWORD *)r = frame_no;
    r[4] = (BYTE)kind;
    r[5] = (BYTE)(in_sv_frame | in_cbuf << 1 | rng_aside << 2);
    r[6] = (BYTE)frame_mode;
    r[7] = 0;
    *(DWORD *)(r + 8) = a;
    *(DWORD *)(r + 12) = b;
    fwrite(r, 16, 1, rng_log);
}

static void rng_mark_server(void)
{
    static const char *const mods[] = { "hl.dll", "client.dll", "bunnymodxt.dll" };
    static BOOL told;
    rng_t r;

    if (!told) {
        for (int i = 0; i < 3; i++)
            rng_record(17, (DWORD)(ULONG_PTR)GetModuleHandleA(mods[i]), i);
        told = TRUE;
    }

    rng_get(&r);
    rng_record(16, (DWORD)fnv(1469598103934665603ull, (BYTE *)&r, sizeof(r)),
               (DWORD)llround(*(double *)SV_TIME * 1e6));
}

typedef struct {
    BYTE kind, mod, pad[2];
    DWORD ra, lo, hi, val;
    DWORD pad2;
    double t;
} rng_rec_t;
static rng_rec_t *rng_play;
static int rng_nplay, rng_iplay;
static BOOL rng_play_on;
static int rng_loads, rng_misses;
static DWORD hl_base, hl_size;
static float(__cdecl *rf_next)(float, float);
static int(__cdecl *rl_next)(int, int);

static void find_game_dll(void)
{
    HMODULE m = GetModuleHandleA("hl.dll");
    IMAGE_NT_HEADERS *nt;

    hl_base = hl_size = 0;
    if (!m)
        return;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    hl_base = (DWORD)(ULONG_PTR)m;
    hl_size = nt->OptionalHeader.SizeOfImage;
}

static BOOL server_draw(DWORD ra, BYTE *mod, DWORD *rel)
{
    if (!hl_base)
        find_game_dll();
    if (hl_base && ra - hl_base < hl_size) {
        *mod = 1;
        *rel = ra - hl_base;
        return TRUE;
    }
    *mod = 0;
    *rel = ra;
    return in_sv_frame;
}

static void rng_mismatch(const char *why, int kind, BYTE mod, DWORD ra, DWORD lo, DWORD hi)
{
    const rng_rec_t *r = rng_iplay < rng_nplay ? &rng_play[rng_iplay] : NULL;

    vkgl_log("bench: random number %d at sv.time %.4f is not the recorded one (%s): %s %s+%lx (%08lx %08lx), "
             "recorded %s %s+%lx (%08lx %08lx) at %.4f; the generator from here on", rng_iplay,
             *(double *)SV_TIME, why, kind == 1 ? "RandomFloat" : "RandomLong", mod ? "hl" : "hw",
             (unsigned long)ra, (unsigned long)lo, (unsigned long)hi,
             r ? (r->kind == 1 ? "RandomFloat" : "RandomLong") : "-", r && r->mod ? "hl" : "hw",
             r ? (unsigned long)r->ra : 0ul, r ? (unsigned long)r->lo : 0ul, r ? (unsigned long)r->hi : 0ul,
             r ? r->t : 0.0);
    rng_play_on = FALSE;
    rng_misses++;
}

static void rng_mark_load(const char *map)
{
    DWORD h = (DWORD)fnv(1469598103934665603ull, (const BYTE *)map, strlen(map));
    int i;

    if (rng_rec_file) {
        rng_rec_t r;

        memset(&r, 0, sizeof(r));
        r.lo = (DWORD)rng_loads;
        r.hi = h;
        r.t = *(double *)SV_TIME;
        fwrite(&r, sizeof(r), 1, rng_rec_file);
    }
    if (rng_play) {
        for (i = 0; i < rng_nplay; i++)
            if (!rng_play[i].kind && rng_play[i].lo == (DWORD)rng_loads && rng_play[i].hi == h)
                break;
        if (i < rng_nplay) {
            if (rng_play_on && rng_iplay != i) {
                vkgl_log("bench: load %d (%s): the run drew fewer random numbers than recorded before it",
                         rng_loads, map);
                rng_misses++;
            } else if (!rng_play_on)
                vkgl_log("bench: random numbers from the record again from load %d (%s)", rng_loads, map);
            rng_iplay = i + 1;
            rng_play_on = TRUE;
        } else if (rng_play_on) {
            vkgl_log("bench: load %d (%s) is not in the record, the generator from here on", rng_loads, map);
            rng_play_on = FALSE;
            rng_misses++;
        }
    }
    rng_loads++;
}

static BOOL rng_replay(int kind, BYTE mod, DWORD ra, DWORD lo, DWORD hi, DWORD *val)
{
    const rng_rec_t *r;

    if (!rng_play_on)
        return FALSE;
    if (rng_iplay >= rng_nplay) {
        rng_mismatch("past the end", kind, mod, ra, lo, hi);
        return FALSE;
    }
    r = &rng_play[rng_iplay];
    if (!r->kind) {
        rng_mismatch("one more than recorded before the next load", kind, mod, ra, lo, hi);
        return FALSE;
    }
    if (r->kind != kind || r->mod != mod || r->ra != ra || r->lo != lo || r->hi != hi) {
        rng_mismatch("another draw", kind, mod, ra, lo, hi);
        return FALSE;
    }
    rng_iplay++;
    *val = r->val;
    return TRUE;
}

static void rng_keep(int kind, BYTE mod, DWORD ra, DWORD lo, DWORD hi, DWORD val)
{
    rng_rec_t r;

    memset(&r, 0, sizeof(r));
    r.kind = (BYTE)kind;
    r.mod = mod;
    r.ra = ra;
    r.lo = lo;
    r.hi = hi;
    r.val = val;
    r.t = *(double *)SV_TIME;
    fwrite(&r, sizeof(r), 1, rng_rec_file);
}

static float __cdecl randomfloat_hook(float lo, float hi)
{
    DWORD ra = (DWORD)(ULONG_PTR)__builtin_return_address(0), rel = 0, l, h, v;
    BYTE mod = 0;
    float f;
    BOOL sv = (rng_rec_file || rng_play_on) && server_draw(ra, &mod, &rel);

    memcpy(&l, &lo, 4);
    memcpy(&h, &hi, 4);
    if (rng_log)
        rng_record(1, ra, l);
    if (sv && rng_replay(1, mod, rel, l, h, &v)) {
        memcpy(&f, &v, 4);
        return f;
    }
    f = rf_next(lo, hi);
    if (sv && rng_rec_file) {
        memcpy(&v, &f, 4);
        rng_keep(1, mod, rel, l, h, v);
    }
    return f;
}

static int __cdecl randomlong_hook(int lo, int hi)
{
    DWORD ra = (DWORD)(ULONG_PTR)__builtin_return_address(0), rel = 0, v;
    BYTE mod = 0;
    int n;
    BOOL sv = (rng_rec_file || rng_play_on) && server_draw(ra, &mod, &rel);

    if (rng_log)
        rng_record(2, ra, (DWORD)lo);
    if (sv && rng_replay(2, mod, rel, (DWORD)lo, (DWORD)hi, &v))
        return (int)v;
    n = rl_next(lo, hi);
    if (sv && rng_rec_file)
        rng_keep(2, mod, rel, (DWORD)lo, (DWORD)hi, (DWORD)n);
    return n;
}

static BOOL rng_load_play(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (!f)
        return FALSE;
    fseek(f, 0, SEEK_END);
    n = ftell(f) / (long)sizeof(rng_rec_t);
    fseek(f, 0, SEEK_SET);
    rng_play = n > 0 ? malloc(n * sizeof(rng_rec_t)) : NULL;
    rng_nplay = rng_play ? (int)fread(rng_play, sizeof(rng_rec_t), n, f) : 0;
    fclose(f);
    return TRUE;
}

static void rng_report(void)
{
    if (rng_play) {
        int n = 0, i;

        for (i = 0; i < rng_nplay; i++)
            n += rng_play[i].kind != 0;
        vkgl_log("bench: random numbers: %d recorded over %d loads, %d not as recorded%s", n, rng_loads, rng_misses,
                 rng_iplay < rng_nplay && rng_play_on ? ", the record goes on past the end" : "");
    }
    if (rng_rec_file)
        fflush(rng_rec_file);
}

static void rng_split(const split_t *sp)
{
    if (rng_play)
        rng_report();
    if (rng_rec_file) {
        fclose(rng_rec_file);
        rng_rec_file = NULL;
    }
    free(rng_play);
    rng_play = NULL;
    rng_nplay = rng_iplay = rng_misses = rng_loads = 0;
    rng_play_on = FALSE;
    if (sp->rec[0] && strcmp(sp->rec, "-") && (rng_rec_file = fopen(sp->rec, "wb")))
        setvbuf(rng_rec_file, NULL, _IOFBF, 1 << 16);
    if (sp->play[0] && strcmp(sp->play, "-")) {
        if (rng_load_play(sp->play))
            rng_play_on = TRUE;
        else
            vkgl_log("bench: cannot read %s", sp->play);
    }
}

void vkgl_physics_exit(void)
{
    if (rng_play || rng_rec_file)
        rng_report();
    if (bench.log)
        fflush(bench.log);
}

static void rng_hooks_setup(BYTE *page)
{
    char path[MAX_PATH];
    BYTE b[5], *p = page;

    if (GetEnvironmentVariableA("VKGL_BENCH_RNGLOG", path, sizeof(path)) && (rng_log = fopen(path, "wb")))
        setvbuf(rng_log, NULL, _IOFBF, 1 << 20);
    if (GetEnvironmentVariableA("VKGL_BENCH_RNGREC", path, sizeof(path)) && (rng_rec_file = fopen(path, "wb")))
        setvbuf(rng_rec_file, NULL, _IOFBF, 1 << 16);
    if (GetEnvironmentVariableA("VKGL_BENCH_RNGPLAY", path, sizeof(path))) {
        if (rng_load_play(path))
            rng_play_on = TRUE;
        else
            vkgl_log("bench: cannot read %s", path);
    }
    if (!rng_log && !rng_rec_file && !rng_play_on && !nsplits)
        return;
    if (!readable(RANDOMFLOAT, 5) || memcmp((void *)RANDOMFLOAT, randomfloat_orig, 5) || *(BYTE *)RANDOMLONG != 0xe9) {
        vkgl_log("bench: RandomFloat/RandomLong are not the expected code, no random number log or replay");
        rng_play_on = FALSE;
        return;
    }

    rf_next = (void *)p;
    p = put(p, (const char *)randomfloat_orig, 5);
    p = put(p, "\xe9", 1);
    p = rel32(p, RANDOMFLOAT + 5);
    FlushInstructionCache(GetCurrentProcess(), page, (DWORD)(p - page));
    rl_next = (void *)(ULONG_PTR)(RANDOMLONG + 5 + *(int *)(RANDOMLONG + 1));
    put32(put(b, "\xe9", 1), (DWORD)(ULONG_PTR)randomfloat_hook - (RANDOMFLOAT + 5));
    write_code(RANDOMFLOAT, b, 5);
    put32(put(b, "\xe9", 1), (DWORD)(ULONG_PTR)randomlong_hook - (RANDOMLONG + 5));
    write_code(RANDOMLONG, b, 5);
    vkgl_log("bench: random numbers%s%s%s", rng_log ? " logged" : "", rng_rec_file ? ", recorded" : "",
             rng_play_on ? ", handed out from the record" : "");
    if (rng_play_on)
        vkgl_log("bench: %d recorded random numbers", rng_nplay);
}

static int __cdecl spawn_hook(int demo, char *server, char *startspot)
{
    int(__cdecl *orig)(int, char *, char *) = (void *)SV_SPAWNSERVER;

    memset(str_hi, 0, sizeof(str_hi));
    if (server && (rng_rec_file || rng_play))
        rng_mark_load(server);
    if (server)
        bench_seed(bench.seg + (int)(fnv(1469598103934665603ull, (BYTE *)server, strlen(server)) % 1000003));
    return orig(demo, server, startspot);
}

static void __cdecl sv_frame_hook(void)
{
    void(__cdecl *orig)(void) = (void *)SV_FRAME;
    int mode = decide();
    double saved = *(double *)HOST_FRAMETIME, t0;
    trace("sv_frame idx", frame_idx);
    if (mode == FRAME_SKIP)
        return;

    if (bench.state == BENCH_WATCH && !bench.watch_started && bench.watch_loaded && bench.pre_ft &&
        *(int *)CLS_STATE == 5 && fabs(saved * 1000.0 - bench.pre_ft * 1000.0) > 1e-4)
        return;
    if (bench.state != BENCH_OFF)
        bench_before_server(frame_idx);

    if (mode == FRAME_TICK ||
        (fix_on && (fast || level_host_rate_fast()) && *(int *)CLS_STATE != 5 &&
         *(int *)SVS_MAXCLIENTS == 1 && saved * 1000.0 < TICK &&
         level_frame != frame_no))
        *(double *)HOST_FRAMETIME = tick_sec;
    else if (mode == FRAME_SLOW && world_slow > 0)
        *(double *)HOST_FRAMETIME = world_slow;
    t0 = *(double *)SV_TIME;
    if (rng_log)
        rng_mark_server();
    if (*(int *)CLS_STATE == 5)
        ev_steps++;
    in_sv_frame = TRUE;
    orig();
    in_sv_frame = FALSE;
    sv_ran = frame_no;

    if (cmd_ahead > 0) {
        BYTE *cl = *(int *)SVS_MAXCLIENTS == 1 ? *(BYTE **)SVS_CLIENTS : NULL;
        if (cl && *(int *)cl)
            *(double *)(cl + CL_CMDTIME) -= cmd_ahead / 1000.0;
        cmd_ahead = 0;
    }
    if (*(double *)SV_TIME > t0)
        bench_clock_us += llround((*(double *)SV_TIME - t0) * 1e6);
    *(double *)HOST_FRAMETIME = saved;
    trace("sv_frame done", 0);
    if (trace_left)
        trace_left--;
    if (bench.log)
        log_frame(frame_idx);
}

static void view_log_frame(BYTE *params, float a)
{
    BYTE *pl = player(), *ents = *(BYTE **)CL_ENTITIES, *g;
    float *v = (float *)params, *o;
    int gi = -1;

    if (!view_log)
        return;
    if (pl && (g = *(BYTE **)(pl + EV_GROUNDENTITY)))
        gi = edict_index(g);
    fprintf(view_log, "%u %u %.4f %.4f %.4f %.4f", frame_no, tick_count, a, v[0], v[1], v[2]);
    if (gi > 0 && gi < *(int *)SV_NUM_EDICTS && ents) {
        o = (float *)(ents + gi * CL_ENTITY_SIZE + CL_ORIGIN);
        fprintf(view_log, " %d %.4f %.4f %.4f %.4f %.4f %.4f", gi, o[0], o[1], o[2], o[3], o[4], o[5]);
    }
    fputc('\n', view_log);
}

static void __cdecl calcrefdef(BYTE *params)
{
    static float prev[3], cur[3];
    static unsigned seen;
    void(__cdecl *client)(BYTE *) = *(void(__cdecl **)(BYTE *))CALCREFDEF_PTR;
    float *simorg = (float *)(params + REF_SIMORG), saved[3], d2 = 0, a;
    int i;

    if (!client)
        return;
    if (seen != tick_count) {
        memcpy(prev, seen + 1 == tick_count ? cur : simorg, sizeof(prev));
        seen = tick_count;
    }
    memcpy(cur, simorg, sizeof(cur));
    for (i = 0; i < 3; i++)
        d2 += (cur[i] - prev[i]) * (cur[i] - prev[i]);

    if (!interp_on || !tick_regular || d2 > 64 * 64 || *(int *)(params + REF_DEMOPLAYBACK)) {
        client(params);
        view_log_frame(params, -1);
        return;
    }

    a = (float)(tick_acc / TICK);
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    memcpy(saved, simorg, sizeof(saved));
    for (i = 0; i < 3; i++)
        simorg[i] = prev[i] + (cur[i] - prev[i]) * a;
    client(params);
    view_log_frame(params, a);
    memcpy(simorg, saved, sizeof(saved));
}

static BOOL autofunc_setup(BYTE *thunk)
{
    BYTE b[6], *p = thunk;
    DWORD hook = (DWORD)(ULONG_PTR)view_angles_hook, *slot = (DWORD *)(CL_ENGINEFUNCS + 34 * 4);

    if (!readable(CBUF_EXECUTE, 12) || !readable(CL_ENGINEFUNCS, 36 * 4) ||
        (*(BYTE *)CBUF_EXECUTE != 0xe9 && memcmp((void *)CBUF_EXECUTE, cbuf_entry_orig, 6)) ||
        memcmp((void *)CBUF_BODY, cbuf_body_orig, 6) || slot[0] != ENG_GETVIEWANGLES || slot[1] != ENG_SETVIEWANGLES)
        return FALSE;
    p = put(p, "\x60\x9c\xb8", 3);
    p = put32(p, (DWORD)(ULONG_PTR)cbuf_body);
    p = put(p, "\xff\xd0\x9d\x61", 4);
    p = put(p, (const char *)cbuf_body_orig, 6);
    p = put(p, "\xe9", 1);
    p = rel32(p, CBUF_BODY + 6);
    FlushInstructionCache(GetCurrentProcess(), thunk, (DWORD)(p - thunk));
    put32(put(b, "\xe9", 1), (DWORD)(ULONG_PTR)thunk - (CBUF_BODY + 5));
    b[5] = 0x90;
    write_code(CBUF_BODY, b, 6);
    write_code((DWORD)(ULONG_PTR)slot, &hook, 4);
    return TRUE;
}

static BOOL redirect_call(DWORD site, DWORD target, void *hook)
{
    BYTE b[5];

    if (!readable(site, 5) || *(BYTE *)site != 0xe8 || (DWORD)(*(int *)(site + 1) + site + 5) != target)
        return FALSE;
    b[0] = 0xe8;
    put32(b + 1, (DWORD)(ULONG_PTR)hook - (site + 5));
    write_code(site, b, 5);
    return TRUE;
}

void vkgl_physics_setup(BOOL fix, BOOL interp)
{
    tick_sec = nextafterf(0.01f, 1);
    BYTE *page, *p, site[8];
    HMODULE self;
    bench_setup();
    if (!fix && bench.state == BENCH_OFF)
        return;
    if (!readable(MSEC_SITE, MSEC_ORIG_LEN) || memcmp((void *)MSEC_SITE, msec_orig, MSEC_ORIG_LEN) ||
        !readable(CALL_CBUF, 5) || !readable(CALL_SV_FRAME, 5) || !readable(RNG_IV - 0x10, 0x800) ||
        memcmp((void *)0x01d79c50, ran1_orig, sizeof(ran1_orig) - 1) ||
        memcmp(*(char **)(HOST_FRAMERATE - 12), hfr_name, sizeof(hfr_name))) {
        vkgl_log("engine: not the expected hw.dll build, physics fix skipped");
        bench.state = BENCH_OFF;
        return;
    }

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)tick_cmd, &self) ||
        !(page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        return;
    if (!redirect_call(CALL_CBUF, CBUF_EXECUTE, cbuf_hook) || !redirect_call(CALL_SV_FRAME, SV_FRAME, sv_frame_hook)) {
        vkgl_log("engine: _Host_Frame is not the expected code, physics fix skipped");
        bench.state = BENCH_OFF;
        return;
    }

    if (fix || bench.state != BENCH_WATCH) {
        p = page;
        p = put(p, "\x56\xb8", 2);
        p = put32(p, (DWORD)(ULONG_PTR)tick_cmd);
        p = put(p, "\xff\xd0\x83\xc4\x04", 5);
        p = put(p, "\x85\xc0\x0f\x8c", 4);
        p = rel32(p, CL_MOVE_EXIT);
        p = put(p, "\xe9", 1);
        p = rel32(p, MSEC_RESUME);
        FlushInstructionCache(GetCurrentProcess(), page, (DWORD)(p - page));

        put32(put(site, "\x83\xc4\x08\xe9", 4), (DWORD)(ULONG_PTR)page - (MSEC_SITE + 8));
        write_code(MSEC_SITE, site, 8);
    }

    if (bench.state != BENCH_OFF) {
        rng_hooks_setup(page + 512);
        if (readable(PM_FLOATTIME_SITE, sizeof(pm_floattime_orig) - 1) &&
            !memcmp((void *)PM_FLOATTIME_SITE, pm_floattime_orig, sizeof(pm_floattime_orig) - 1)) {
            DWORD f = (DWORD)(ULONG_PTR)bench_floattime;
            write_code(PM_FLOATTIME_SITE + 6, &f, 4);
        } else
            vkgl_log("bench: PM_Init is not the expected code, pmove keeps the wall clock");
    }

    fix_on = fix;
    if ((fix || vclock) && !redirect_call(CALL_FILTERTIME, HOST_FILTERTIME, filter_hook))
        vkgl_log("engine: Host_FilterTime call is not the expected code, frames between steps start "
                 "drawing from their own random numbers after the console%s", vclock ? ", no bench clock" : "");
    if (fix) {
        rng_get(&rng_draw);
        tick_stats = GetEnvironmentVariableA("VKGL_STATS", NULL, 0) > 0;
        bench_play = bench.state != BENCH_OFF && (GetEnvironmentVariableA("VKGL_BENCH_PLAY", NULL, 0) > 0 || vclock);
        fps_max_ok = readable(FPS_MAX - 12, 16) && readable(*(DWORD *)(FPS_MAX - 12), sizeof(fps_max_name)) &&
                     !memcmp(*(char **)(FPS_MAX - 12), fps_max_name, sizeof(fps_max_name));
        if (!fps_max_ok)
            vkgl_log("engine: fps_max not found, a script that lowers it waits for the average frame time");
        autofunc_ft = autofunc_setup(page + 256);
        if (!autofunc_ft)
            vkgl_log("engine: Cbuf_Execute or cl_enginefuncs is not the expected code, Bunnymod XT's autofuncs "
                     "(jumpbug) predict with the frame's time");
        interp_on = interp;
        if (interp) {
            if (readable(CALCREFDEF_SITE, sizeof(calcrefdef_orig) - 1) &&
                !memcmp((void *)CALCREFDEF_SITE, calcrefdef_orig, sizeof(calcrefdef_orig) - 1)) {
                put32(put(site, "\xe9", 1), (DWORD)(ULONG_PTR)calcrefdef - (CALCREFDEF_SITE + 5));
                write_code(CALCREFDEF_SITE, site, 5);
            } else {
                interp_on = FALSE;
                vkgl_log("engine: ClientDLL_CalcRefdef is not the expected code, no view interpolation");
            }
        }
        if (!redirect_call(CALL_CLDATA, CL_UPDATECLIENTDATA, cldata_hook))
            vkgl_log("engine: ClientDLL_UpdateClientData call is not the expected code, a click that picks a "
                     "weapon from the HUD menu between steps also attacks");
        if (!redirect_call(CALL_SCREEN, HOST_UPDATESCREEN, screen_hook))
            vkgl_log("engine: Host_UpdateScreen call is not the expected code, the server sees the light of the "
                     "last frame drawn");
        vkgl_log("engine: physics fix on (10 ms simulation steps%s)", interp_on ? ", view interpolated" : "");
    }

    if (bench.state == BENCH_WATCH) {
        int i, n = 0;

        bench_seed(bench.seg);
        if (GetEnvironmentVariableA("VKGL_BENCH_ILLUM", NULL, 0) > 0 &&
            readable(GET_ENTITY_ILLUM, sizeof(illum_orig) - 1) &&
            !memcmp((void *)GET_ENTITY_ILLUM, illum_orig, sizeof(illum_orig) - 1)) {
            put32(put(site, "\xe9", 1), (DWORD)(ULONG_PTR)illum_hook - (GET_ENTITY_ILLUM + 5));
            write_code(GET_ENTITY_ILLUM, site, 5);
        }
        for (i = 0; i < (int)(sizeof(spawn_calls) / sizeof(*spawn_calls)); i++)
            n += redirect_call(spawn_calls[i], SV_SPAWNSERVER, spawn_hook);
        if (n != (int)(sizeof(spawn_calls) / sizeof(*spawn_calls)))
            vkgl_log("bench: %d of %d SV_SpawnServer calls found, levels may spawn differently", n,
                     (int)(sizeof(spawn_calls) / sizeof(*spawn_calls)));
    }
}
