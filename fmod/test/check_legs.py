#!/usr/bin/env python3
"""
check_legs — checks each built ampaac library against its platform contract (README.md, Build Legs).

    check_legs.py [--build-root DIR] [--build-type release] [--llvm-bin DIR] [--allow-dirty]

For every <leg>-<build-type>/artifact.txt under the build root: architecture and OS floor, dynamic
dependencies, exported symbols, no C++ runtime, stack protection, no builder path in the bytes, and a
revision stamp equal to the fork's HEAD. One line per check; exits 1 if any check fails or nothing is built.
Apple legs use Xcode's tools (xcrun); ELF and PE legs use LLVM's (llvm-mingw ships them).
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

EXPORTS = {"AMPAAC_GetCodecDescription", "FMODGetCodecDescription"}
IOS_EXPORTS = {"AMPAAC_GetCodecDescription"}

OSX_MINOS = {"x86_64": "10.15", "arm64": "11.0"}  # arm64 macOS starts at 11.0
IOS_MINOS = "12.4"
ANDROID_API = 27
ANDROID_PAGE = 0x4000
ANDROID_NEEDED = {"libc.so", "libm.so", "libdl.so"}
LINUX_NEEDED = {"libc.so.6", "libm.so.6"}
LEGS = ("OSX", "iOS", "Android", "Linux", "Windows")
# Absolute-path prefixes that never belong in a shipped binary, besides the builder's own paths.
BUILDER_PREFIXES = ("/Users/", "/home/", "/private/", "/tmp/", "/var/folders/")
LINUX_GLIBC_MAX = (2, 28)
WINDOWS_IMPORT = re.compile(r"^(KERNEL32\.dll|api-ms-win-crt-[a-z]+-l1-1-0\.dll)$", re.IGNORECASE)

# Imports that would mean a C++ runtime dependency (Mach-O names are compared without their leading '_').
CXX_RUNTIME = re.compile(r"^(_Zn[wa]|_Zd[la]|__cxa_(throw|rethrow|begin_catch|end_catch|allocate_exception|"
                         r"guard_|pure_virtual|bad_)|__gxx_personality|_Unwind_Resume)")

IDENT = re.compile(rb"ampaac fdk-aac ([0-9a-f]{7,40}(?:-dirty)?)")


class Report:
    def __init__(self):
        self.checks = 0
        self.failed = 0

    def check(self, leg, name, ok, detail):
        self.checks += 1
        self.failed += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  {leg:<18} {name:<16} {detail}")

    def info(self, leg, name, detail):
        print(f"      {leg:<18} {name:<16} {detail}")


def run(*cmd):
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise RuntimeError(f"{Path(cmd[0]).name} {' '.join(cmd[1:-1])}: {done.stderr.strip() or done.returncode}")
    return done.stdout


def names(text):
    """Symbol names from nm output ('<addr> T name', 'U name@VER', or a bare name; 'member.o:' headers skipped)."""
    out = set()
    for line in text.splitlines():
        parts = line.split()
        if parts and not line.endswith(":"):
            out.add(parts[-1].split("@")[0])
    return out


def unprefixed(symbols):
    return {s[1:] if s.startswith("_") else s for s in symbols}


def common_checks(report, leg, artifact, undefined, args):
    data = artifact.read_bytes()
    stamps = {m.decode() for m in IDENT.findall(data)}
    # The build stamps "-dirty" when the compiled trees differ from HEAD; that stamp passes only when allowed.
    ok = stamps == {args.head} or (args.allow_dirty and stamps == {args.head + "-dirty"})
    report.check(leg, "revision", ok, f"stamp {', '.join(sorted(stamps)) or 'missing'}; fork HEAD {args.head}")

    leaks = [p for p in args.builder_paths + list(BUILDER_PREFIXES) if p.encode() in data]
    report.check(leg, "builder paths", not leaks, "none embedded" if not leaks else f"embedded: {', '.join(leaks)}")

    if undefined is not None:
        cxx = sorted(s for s in undefined if CXX_RUNTIME.match(s))
        report.check(leg, "no C++ runtime", not cxx, "no C++ runtime imports" if not cxx else ", ".join(cxx))
        report.check(leg, "stack protector", "__stack_chk_fail" in undefined, "imports __stack_chk_fail"
                     if "__stack_chk_fail" in undefined else "__stack_chk_fail not imported")
    report.info(leg, "size", f"{artifact.stat().st_size:,} bytes")


def build_versions(otool_l):
    """(platform, minos) per LC_BUILD_VERSION in `otool -l` output."""
    out = []
    for block in otool_l.split("Load command")[1:]:
        if "cmd LC_BUILD_VERSION" in block:
            platform = re.search(r"platform (\d+)", block)
            minos = re.search(r"minos (\S+)", block)
            out.append((int(platform.group(1)), minos.group(1)))
    return out


def check_osx(report, leg, artifact, args):
    archs = run("xcrun", "lipo", "-archs", str(artifact)).split()
    report.check(leg, "archs", set(archs) == set(OSX_MINOS), " ".join(archs))
    undefined = set()
    for arch in archs:
        versions = build_versions(run("xcrun", "otool", "-arch", arch, "-l", str(artifact)))
        want = OSX_MINOS.get(arch)
        report.check(leg, f"minos {arch}", versions == [(1, want)], f"{versions} (want macOS {want})")
        deps = [line.split(" (")[0].strip() for line in run("xcrun", "otool", "-arch", arch, "-L", str(artifact))
                .splitlines()[1:] if line.startswith("\t")]
        deps = [d for d in deps if not d.endswith("/libampaac.dylib")]
        report.check(leg, f"deps {arch}", deps == ["/usr/lib/libSystem.B.dylib"], " ".join(deps))
        exports = unprefixed(names(run("xcrun", "nm", "-gU", "-arch", arch, str(artifact))))
        report.check(leg, f"exports {arch}", exports == EXPORTS, " ".join(sorted(exports)))
        undefined |= unprefixed(names(run("xcrun", "nm", "-u", "-arch", arch, str(artifact))))
    common_checks(report, leg, artifact, undefined, args)


def check_ios(report, leg, artifact, args):
    archs = run("xcrun", "lipo", "-archs", str(artifact)).split()
    report.check(leg, "archs", archs == ["arm64"], " ".join(archs))
    members = [m for m in run("ar", "t", str(artifact)).splitlines() if not m.startswith("__.SYMDEF")]
    report.check(leg, "members", members == ["ampaac_prelinked.o"], " ".join(members))
    versions = build_versions(run("xcrun", "otool", "-l", str(artifact)))
    report.check(leg, "minos", versions == [(2, IOS_MINOS)], f"{versions} (want iOS {IOS_MINOS})")
    exports = unprefixed(names(run("xcrun", "nm", "-gU", str(artifact))))
    report.check(leg, "globals", exports == IOS_EXPORTS, " ".join(sorted(exports)))
    undefined = unprefixed(names(run("xcrun", "nm", "-u", str(artifact))))
    common_checks(report, leg, artifact, undefined, args)


def elf_basics(report, leg, artifact, args, machine):
    header = run(args.tool("llvm-readelf"), "-h", str(artifact))
    got = re.search(r"Machine:\s+(.+)", header).group(1).strip()
    report.check(leg, "machine", "ELF64" in header and got == machine, got)
    dynamic = run(args.tool("llvm-readelf"), "-d", str(artifact))
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", dynamic)
    exports = names(run(args.tool("llvm-nm"), "-D", "--defined-only", str(artifact)))
    report.check(leg, "exports", exports == EXPORTS, " ".join(sorted(exports)))
    undefined = names(run(args.tool("llvm-nm"), "-D", "--undefined-only", str(artifact)))
    return needed, undefined


def check_android(report, leg, artifact, args):
    needed, undefined = elf_basics(report, leg, artifact, args, "AArch64")
    report.check(leg, "needed", "libc.so" in needed and set(needed) <= ANDROID_NEEDED, " ".join(needed) or "none parsed")
    aligns = [int(line.split()[-1], 16) for line in run(args.tool("llvm-readelf"), "-lW", str(artifact))
              .splitlines() if line.strip().startswith("LOAD")]
    report.check(leg, "LOAD align", aligns and all(a == ANDROID_PAGE for a in aligns),
                 " ".join(hex(a) for a in aligns) + f" (want {hex(ANDROID_PAGE)})")
    notes = run(args.tool("llvm-readelf"), "-n", str(artifact))
    ident = re.search(r"NT_ANDROID_TYPE_IDENT\s+description data: ((?:[0-9a-f]{2} ){4})", notes)
    api = int.from_bytes(bytes.fromhex(ident.group(1).replace(" ", "")), "little") if ident else None
    report.check(leg, "API level", api == ANDROID_API, f"{api} (want {ANDROID_API})")
    common_checks(report, leg, artifact, undefined, args)


def check_linux(report, leg, artifact, args):
    needed, undefined = elf_basics(report, leg, artifact, args, "Advanced Micro Devices X86-64")
    report.check(leg, "needed", "libc.so.6" in needed and set(needed) <= LINUX_NEEDED, " ".join(needed) or "none parsed")
    versions = {tuple(int(p) for p in v.split(".")) for v in
                re.findall(r"Name: GLIBC_([0-9.]+)", run(args.tool("llvm-readelf"), "-V", str(artifact)))}
    top = max(versions) if versions else None
    report.check(leg, "glibc floor", top is not None and top <= LINUX_GLIBC_MAX,
                 f"GLIBC_{'.'.join(map(str, top)) if top else '?'} (want <= {'.'.join(map(str, LINUX_GLIBC_MAX))})")
    common_checks(report, leg, artifact, undefined, args)


def check_windows(report, leg, artifact, args):
    header = run(args.tool("llvm-readobj"), "--file-headers", str(artifact))
    machine = re.search(r"Machine: (\S+)", header).group(1)
    report.check(leg, "machine", machine == "IMAGE_FILE_MACHINE_AMD64", machine)
    private = run(args.tool("llvm-objdump"), "-p", str(artifact))
    imports = re.findall(r"DLL Name: (\S+)", private)
    stray = [i for i in imports if not WINDOWS_IMPORT.match(i)]
    report.check(leg, "imports", imports and not stray, " ".join(imports) if not stray else "stray: " + " ".join(stray))
    exports = set(re.findall(r"^\s+\d+\s+0x[0-9a-f]+\s+(\S+)$", private.split("Export Table:")[-1], re.MULTILINE)) \
        if "Export Table:" in private else set()
    report.check(leg, "exports", exports == EXPORTS, " ".join(sorted(exports)))
    sections = re.findall(r"^\s*\d+\s+(\S+)", run(args.tool("llvm-objdump"), "-h", str(artifact)), re.MULTILINE)
    dwarf = [s for s in sections if s.startswith(".debug")]
    report.check(leg, "no DWARF", ".text" in sections and not dwarf, " ".join(dwarf or sections) or "no sections parsed")
    debug = run(args.tool("llvm-readobj"), "--coff-debug-directory", str(artifact))
    pdb_name = re.search(r"PDBFileName: (\S+)", debug)
    pdb_name = pdb_name.group(1) if pdb_name else None
    report.check(leg, "PDB reference", pdb_name == "ampaac.pdb", f"{pdb_name} (a bare file name)")
    # The DLL links its runtime statically, so its imports cannot show a C++ runtime or stack protection:
    # the PDB's public symbols can.
    pdb = artifact.with_suffix(".pdb")
    publics = set(re.findall(r"`([^`]+)`", run(args.tool("llvm-pdbutil"), "dump", "-publics", str(pdb)))) \
        if pdb.exists() else set()
    cxx = sorted(s for s in publics if CXX_RUNTIME.match(s))
    report.check(leg, "no C++ runtime", publics and not cxx, ("PDB lists no C++ runtime symbol" if not cxx
                 else ", ".join(cxx)) if publics else f"{pdb.name} missing")
    report.check(leg, "stack protector", "__stack_chk_fail" in publics,
                 "PDB lists __stack_chk_fail" if publics else f"{pdb.name} missing")
    common_checks(report, leg, artifact, None, args)


CHECKERS = {
    "OSX": check_osx,
    "iOS": check_ios,
    "Android": check_android,
    "Linux": check_linux,
    "Windows": check_windows,
}


def main():
    fmod_dir = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--build-root", type=Path, default=fmod_dir / "build")
    parser.add_argument("--build-type", default="release")
    parser.add_argument("--llvm-bin", type=Path, help="directory holding llvm-readelf, llvm-nm, llvm-objdump, "
                        "llvm-readobj, llvm-pdbutil (ELF and PE legs)")
    parser.add_argument("--allow-dirty", action="store_true", help="accept a -dirty revision stamp")
    parser.add_argument("--require-all", action="store_true", help=f"fail unless every leg is built ({', '.join(LEGS)})")
    args = parser.parse_args()

    fork = fmod_dir.parent
    args.head = run("git", "-C", str(fork), "rev-parse", "--short", "HEAD").strip()
    args.builder_paths = sorted({str(Path.home()), str(fork), str(fork.resolve())})
    args.tool = lambda name: str(args.llvm_bin / name) if args.llvm_bin else name

    report = Report()
    manifests = sorted(args.build_root.glob(f"*-{args.build_type}/artifact.txt"))
    for manifest in manifests:
        subdir, artifact = manifest.read_text().splitlines()[:2]
        leg, artifact = subdir, Path(artifact)
        checker = CHECKERS.get(subdir.split("/")[0])
        if checker is None:
            report.check(leg, "platform", False, f"no checker for {subdir}")
            continue
        if not artifact.exists():
            report.check(leg, "artifact", False, f"{artifact} missing")
            continue
        try:
            checker(report, leg, artifact, args)
        except (RuntimeError, AttributeError, FileNotFoundError) as err:
            report.check(leg, "tooling", False, str(err))

    if args.require_all:
        built = {manifest.read_text().splitlines()[0].split("/")[0] for manifest in manifests}
        for leg in LEGS:
            if leg not in built:
                report.check(leg, "built", False, f"no {args.build_type} build under {args.build_root}")
    print(f"check_legs: {len(manifests)} legs, {report.checks} checks, {report.failed} failed")
    if not manifests or report.failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
