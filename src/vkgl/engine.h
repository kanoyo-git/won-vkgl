#pragma once
#include <windows.h>

#define MSEC_SITE 0x01d1c4d0
#define MSEC_RESUME 0x01d1c52c
#define CL_MOVE_EXIT 0x01d1c8c1
#define MSEC_ORIG_LEN 92
extern const BYTE msec_orig[MSEC_ORIG_LEN + 1];

BOOL readable(DWORD addr, DWORD len);
void write_code(DWORD addr, const void *bytes, DWORD len);
BYTE *put(BYTE *p, const char *bytes, int len);
BYTE *put32(BYTE *p, DWORD v);
BYTE *rel32(BYTE *p, DWORD target);

float *engine_cvar(const char *name);

void vkgl_physics_setup(BOOL fix, BOOL interp);
void vkgl_physics_exit(void);
