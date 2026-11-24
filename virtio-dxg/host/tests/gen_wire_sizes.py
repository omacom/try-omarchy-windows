#!/usr/bin/env python3
"""Writes a C program printing the size and member offsets of every message
struct in dxgvmbus.h and d3dkmthk.h. Compiled by the guest's GCC and the
host's MSVC/MinGW, the two outputs must be identical."""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parents[2]
headers = [root / "guest/dxgkrnl/dxgvmbus.h",
           root / "guest/include/uapi/misc/d3dkmthk.h"]
out = []
for h in headers:
    text = h.read_text()
    # Drop kernel-only branches: the host sees the #else (userspace) side.
    text = re.sub(r"#ifdef __KERNEL__.*?#else(.*?)#endif", r"\1", text, flags=re.S)
    for m in re.finditer(r"^struct (\w+) \{\n(.*?)^\}[^;\n]*;", text, re.S | re.M):
        name, body = m.group(1), m.group(2)
        fields, depth = [], 0
        for line in body.splitlines():
            depth += line.count("{") - line.count("}")
            f = re.match(r"\s+[\w\s\*]+?[\s\*](\w+)(\[[^\]]*\])?\s*(:\s*\d+)?;", line)
            if f and depth == 0 and not f.group(3) and line.count("{") == 0:
                fields.append(f.group(1))
        out.append((name, fields))

src = ['#include <stdio.h>', '#include <stddef.h>', '#include "../src/wire.h"',
       "int main(void)", "{"]
for name, fields in out:
    src.append(f'\tprintf("{name} %u\\n", (unsigned)sizeof(struct {name}));')
    for f in fields:
        src.append(f'\tprintf("{name}.{f} %u\\n", '
                   f'(unsigned)offsetof(struct {name}, {f}));')
src += ["\treturn 0;", "}"]
pathlib.Path(sys.argv[1]).write_text("\n".join(src) + "\n")
print(f"{len(out)} structs")
