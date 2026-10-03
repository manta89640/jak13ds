#!/usr/bin/env python3
# (AI-assisted) Refuse an ELF that makes a system call its CIA's exheader doesn't allow.
# On a real 3DS a svc missing from the RSF's SystemCallAccess list raises an exception instead of
# returning an error; Azahar ignores the list. 2026-10-03: a svcFlushProcessDataCache killed the
# render thread at its first cache flush (black top screen, sound still playing, the game hung).
# Every `svc` in the disassembly (objdump keeps literal pools out of it: mapping symbols) must be
# a number listed under SystemCallAccess.
#   check_svc.py <gk.elf> <gk.rsf> [--objdump PATH]
import os, re, shutil, subprocess, sys


def find_objdump(arg):
    if arg:
        return arg
    for d in (os.environ.get("DEVKITARM"), "/opt/devkitpro/devkitARM"):
        if d and os.path.isfile(os.path.join(d, "bin", "arm-none-eabi-objdump")):
            return os.path.join(d, "bin", "arm-none-eabi-objdump")
    return shutil.which("arm-none-eabi-objdump")


def allowed_svcs(rsf):
    """The numbers under SystemCallAccess (Name: number lines, more indented than the key)."""
    allowed, indent = {}, None
    for line in open(rsf, encoding="utf-8", errors="replace"):
        text = line.split("#")[0].rstrip()
        if not text.strip():
            continue
        depth = len(text) - len(text.lstrip())
        if indent is None:
            if text.strip() == "SystemCallAccess:":
                indent = depth
            continue
        if depth <= indent:
            break
        m = re.match(r"\s*([A-Za-z0-9_]+)\s*:\s*(\S+)\s*$", text)
        if m:
            allowed[int(m.group(2), 0)] = m.group(1)
    return allowed if indent is not None else None


def used_svcs(objdump, elf):
    """svc number -> functions that contain it (ARM `svc 0x..` and Thumb `svc N`)."""
    used, func = {}, "?"
    label = re.compile(r"^[0-9a-f]+ <(.+)>:$")
    insn = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4,8}\s+)+svc(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?\s+(?:#)?(0x[0-9a-f]+|\d+)")
    proc = subprocess.Popen([objdump, "-d", elf], stdout=subprocess.PIPE, text=True,
                            errors="replace")
    for line in proc.stdout:
        m = label.match(line)
        if m:
            func = m.group(1)
            continue
        if "svc" not in line:
            continue
        m = insn.match(line)
        if m:
            used.setdefault(int(m.group(1), 0), set()).add(func)
    if proc.wait() != 0:
        return None
    return used


def main():
    args = [a for a in sys.argv[1:]]
    objdump_arg = None
    if "--objdump" in args:
        i = args.index("--objdump")
        objdump_arg = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        print("usage: check_svc.py <elf> <rsf> [--objdump PATH]", file=sys.stderr)
        return 2
    elf, rsf = args
    objdump = find_objdump(objdump_arg)
    if not objdump:
        print("check_svc: arm-none-eabi-objdump not found (set DEVKITARM)", file=sys.stderr)
        return 2
    allowed = allowed_svcs(rsf)
    if allowed is None:
        print(f"check_svc: no SystemCallAccess list in {rsf}", file=sys.stderr)
        return 2
    used = used_svcs(objdump, elf)
    if used is None:
        print(f"check_svc: {objdump} -d {elf} failed", file=sys.stderr)
        return 2
    bad = sorted(n for n in used if n not in allowed)
    for n in bad:
        funcs = ", ".join(sorted(used[n])[:8])
        print(f"check_svc: svc 0x{n:02x} ({n}) is not in {os.path.basename(rsf)}'s SystemCallAccess "
              f"(used in {funcs}): it raises an exception on a real 3DS", file=sys.stderr)
    if bad:
        return 1
    print(f"check_svc: {len(used)} system calls in {os.path.basename(elf)}, all in "
          f"{os.path.basename(rsf)}'s SystemCallAccess: "
          + " ".join(f"0x{n:02x}" for n in sorted(used)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
