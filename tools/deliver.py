#!/usr/bin/env python3
"""Build checksummed native delivery artifacts, or import a developer-tools bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(directory, names):
    (directory / "SHA256SUMS").write_text(
        "".join(f"{sha(directory / name)}  {name}\n" for name in names)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    native = commands.add_parser("native")
    native.add_argument("--ndk", type=Path, default=Path(os.environ.get(
        "ANDROID_NDK_HOME", "/tmp/teleop_tools/android/ndk/27.2.12479018")))
    native.add_argument("--openxr-aar", type=Path)
    package = commands.add_parser("devtools")
    package.add_argument("archive", type=Path)
    args = parser.parse_args()
    if args.command == "devtools":
        if args.archive.stat().st_size >= 95 * 1024 * 1024:
            parser.error("Bundle too large for this Git delivery workflow; use release assets.")
        destination = ROOT / "packages/devtools"
        shutil.copyfile(args.archive, destination / "tools.tar.gz")
        manifest(destination, ["tools.tar.gz", "install.sh"])
        print("Packaged developer tools. Commit packages/devtools and push when ready.")
        return

    output = ROOT / "build/telemetry.so"
    command = [sys.executable, str(ROOT / "tools/compile_native.py"),
               "--ndk", str(args.ndk), "--output", str(output)]
    if args.openxr_aar:
        command += ["--openxr-aar", str(args.openxr_aar)]
    subprocess.run(command, check=True)
    readelf = args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(output)], text=True)
    for symbol in ("quest_native_app_v1", "ANativeActivity_onCreate"):
        if not any(line.split()[-1:] == [symbol] and " UND " not in line
                   for line in symbols.splitlines()):
            raise RuntimeError(f"Missing native-host export: {symbol}")
    header = output.read_bytes()[:20]
    if header[:6] != b"\x7fELF\x02\x01" or header[16:20] != b"\x03\x00\xb7\x00":
        raise RuntimeError("Expected ARM64 ELF shared library")
    destination = ROOT / "packages/telemetry"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(output, destination / "libapp.so")
    sources = [*sorted((ROOT / "native").glob("*")), ROOT / "tools/compile_native.py",
               ROOT / "tools/deliver.py"]
    info = {
        "name": "Quest telemetry", "host_abi": 1, "architecture": "android-arm64",
        "minimum_api": 28, "openxr_loader": "1.1.53",
        "library_sha256": sha(output),
        "sources": {str(p.relative_to(ROOT)): sha(p) for p in sources if p.is_file()},
        "ndk": (args.ndk / "source.properties").read_text(),
    }
    (destination / "build.json").write_text(json.dumps(info, indent=2) + "\n")
    manifest(destination, ["libapp.so", "build.json"])
    print("Built and checked packages/telemetry. Commit source + package together.")


if __name__ == "__main__":
    main()
