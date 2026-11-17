#!/usr/bin/env python3
"""Build the independently force-loaded Fuxi ADSP owner-glue archive.

The archive carries the board's audio runtime: the ADSP owner glue (package/FuxiAdspProbe.cpp), the shared
page's ABI (package/QcomAudioAbi.c) and the audio service the iOS audio server's plug-in opens
(package/FuxiAudioService.cpp).

The service's classes are registered late: their static initializer leaves __DATA,__mod_init_func and is run
by the runtime itself between OSMetaClass preModLoad and postModLoad, as UniXNU-sheng does for
package/ShengAudioService.cpp (scripts/build-gop-legacy-display.py).  The objcopy below removes the section and
globalises the symbol; the instructions are unchanged.
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/fuxi-audio-runtime"
OBJ = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837"
KIT = ROOT / "kits/fuxi"
CXX_FLAGS = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
             "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
             "-Wno-unnecessary-virtual-specifier"]
C_FLAGS = ["-Wall", "-Wextra", "-Werror", "-mgeneral-regs-only"]


def record(path):
    if not path.is_file():
        sys.exit(f"{path} missing: the XNU compile-record pass has not completed")
    return json.loads(path.read_text().strip().rstrip(","))


def command(rec, source, obj, extra):
    args, result, index = rec["arguments"], [], 0
    while index < len(args):
        arg = args[index]
        if arg == rec["file"]:
            result.append(str(source))
        elif arg in ("-o", "-MF", "-MT", "-MJ"):
            if arg == "-o":
                result += ["-o", str(obj)]
            index += 1
        elif arg == "-Xclang" and index + 1 < len(args) and args[index + 1].startswith("-W"):
            index += 1
        elif not arg.startswith("-W"):
            result.append(arg)
        index += 1
    return result + [str(value) for value in extra]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    generated = OUT / "FuxiAudioBindingData.inc"
    # kits/fuxi is a symlink to the device repository's package/. Keep binding.toml
    # as the single fact source instead of copying it into the assembled tree.
    binding = KIT.resolve().parent / "facts/audio/binding.toml"
    subprocess.run([sys.executable, ROOT / "scripts/gen-fuxi-audio-binding.py",
                    "--binding", binding, "--output", generated], check=True)
    cxx = record(OBJ / "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json")
    c = record(OBJ / "osfmk/DEVELOPMENT/startup.o.json")
    bsd = record(OBJ / "bsd/DEVELOPMENT/imageboot.o.json")
    includes = ["-I", KIT, "-I", OUT, "-I", ROOT / ".cache/xnu-work/osfmk"]
    jobs = [(cxx, KIT / "FuxiAdspProbe.cpp", CXX_FLAGS + includes),
            (cxx, KIT / "FuxiAudioService.cpp", CXX_FLAGS + includes),
            (c, KIT / "FuxiAudioBinding.c", C_FLAGS + includes),
            (c, KIT / "QcomAudioAbi.c", C_FLAGS + includes),
            (c, KIT / "FuxiGraphReply.c", C_FLAGS + includes),
            (c, KIT / "FatVolume.c", C_FLAGS + includes),
            (bsd, KIT / "FuxiAdspFirmware.c", C_FLAGS + includes),
            (c, KIT / "QcomAdsp.c", C_FLAGS + includes),
            (c, KIT / "QcomAudioGraph.c", C_FLAGS + includes),
            (c, KIT / "QcomGlink.c", C_FLAGS + includes),
            (c, KIT / "QcomMdt.c", C_FLAGS + includes),
            (c, KIT / "QcomQmp.c", C_FLAGS + includes),
            (c, KIT / "QcomRpmh.c", C_FLAGS + includes),
            (c, KIT / "QcomSmem.c", C_FLAGS + includes),
            (c, KIT / "QcomSmp2p.c", C_FLAGS + includes),
            (bsd, KIT / "ShengGpr.c", C_FLAGS + includes),
            (c, KIT / "GeniI2c.c", C_FLAGS + includes),
            (c, KIT / "Cs35Run.c", C_FLAGS + includes),
            (c, KIT / "CirrusCs35l41.c", C_FLAGS + includes),
            (c, KIT / "FuxiCs35l41.c", C_FLAGS + includes),
            (c, KIT / "FuxiAmpBringup.c", C_FLAGS + includes)]
    objects = []
    for rec, source, flags in jobs:
        if not source.is_file():
            sys.exit(f"{source} missing from the assembled fuxi kit")
        obj = OUT / (source.stem + ".o")
        subprocess.run(command(rec, source, obj, flags), cwd=rec["directory"], check=True, timeout=180)
        objects.append(obj)
        print(f"compiled {source.relative_to(ROOT)}")
    # The audio service's static initializer is taken out of __mod_init_func and globalised, so the runtime
    # runs it between preModLoad and postModLoad (the kernel's own init table would run it too early, before
    # the classes are registered).
    service_obj = OUT / "FuxiAudioService.o"
    service_late = OUT / "FuxiAudioService.late.o"
    objcopy = shutil.which("llvm-objcopy") or "/opt/homebrew/opt/llvm/bin/llvm-objcopy"
    subprocess.run([objcopy, "--remove-section=__DATA,__mod_init_func",
                    "--globalize-symbol=__GLOBAL__sub_I_FuxiAudioService.cpp", str(service_obj), str(service_late)],
                   check=True, timeout=120)
    # Only the late-init copy goes into the archive: the original carries the same
    # class symbols, so keeping both would give the linker duplicate symbols
    # (UniXNU-sheng archives only the .late.o of its audio service as well).
    objects.remove(service_obj)
    objects.append(service_late)
    print(f"staged {service_late.relative_to(ROOT)}")
    archive = OUT / "libFuxiAudioRuntime.a"
    archive.unlink(missing_ok=True)
    ar = ["xcrun", "ar"] if shutil.which("xcrun") else [shutil.which("llvm-ar") or "ar"]
    subprocess.run([*ar, "rcs", archive, *objects], check=True, timeout=60)
    print(f"audio runtime archive: {archive.relative_to(ROOT)} ({len(objects)} objects)")


if __name__ == "__main__":
    main()
