#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys

def main():
    exports_txt, nm, implib, outdir, *sources = sys.argv[1:]
    out = pathlib.Path(outdir)
    out.mkdir(parents=True, exist_ok=True)

    names = [l.strip() for l in open(exports_txt)
             if l.strip() and not l.startswith("#")]
    syms = subprocess.run([nm, implib], capture_output=True, text=True,
                          check=True).stdout
    argbytes = {n: int(b) for n, b in re.findall(r" T _(\w+)@(\d+)", syms)}
    missing = [n for n in names if n not in argbytes]
    if missing:
        sys.exit(f"no stdcall size in {implib} for: {missing}")

    implemented = set()
    for src in sources:
        implemented |= set(re.findall(r"^GLFUNC\(\s*[^,]+,\s*(\w+)\s*,",
                                      open(src).read(), re.M))
    unknown = implemented - set(names)
    if unknown:
        sys.exit(f"GLFUNC for non-exported functions: {sorted(unknown)}")

    ids = "\n".join(f"    ID_{n} = {i}," for i, n in enumerate(names))
    (out / "vkgl_ids.h").write_text(f"""#pragma once
enum {{
{ids}
    VKGL_NUM_FUNCS = {len(names)}
}};
""")

    name_list = "\n".join(f'    "{n}",' for n in names)
    (out / "vkgl_names.c").write_text(f"""#include "vkgl_ids.h"

const char *const vkgl_names[VKGL_NUM_FUNCS] = {{
{name_list}
}};
""")

    asm = ["    .text"]
    for i, n in enumerate(names):
        if n in implemented:
            continue
        asm += [
            f"    .globl _stub_{n}",
            f"_stub_{n}:",
            f"    pushl ${i}",
            "    call _vkgl_stub_hit",
            "    addl $4, %esp",
            "    xorl %eax, %eax",
            f"    ret ${argbytes[n]}" if argbytes[n] else "    ret",
        ]
    for i, n in enumerate(names):
        asm += [f"    .globl _exp_{n}", f"_exp_{n}:", f"    jmp *_vkgl_dispatch+{4 * i}"]

    for i, n in enumerate(names):
        asm += [f"_trace_{n}:", f"    pushl ${i}", "    call _vkgl_trace_hit",
                "    addl $4, %esp", f"    jmp *_vkgl_traced+{4 * i}"]
    asm += ["    .section .rdata", "    .globl _vkgl_trace_thunks", "    .p2align 2",
            "_vkgl_trace_thunks:"]
    asm += [f"    .long _trace_{n}" for n in names]
    asm += ["    .data", "    .globl _vkgl_dispatch", "    .p2align 2", "_vkgl_dispatch:"]
    asm += [f"    .long _{'impl' if n in implemented else 'stub'}_{n}" for n in names]
    (out / "vkgl_stubs.S").write_text("\n".join(asm) + "\n")

    exports = "\n".join(
        f"    {n}=exp_{n}" for n in names)
    (out / "vkgl.def").write_text(f"EXPORTS\n{exports}\n")
    print(f"vkgl: {len(implemented)} implemented, "
          f"{len(names) - len(implemented)} stubbed")

if __name__ == "__main__":
    main()
