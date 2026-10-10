#!/usr/bin/env python3
"""Embed a verified ARMv7 driver in SC-Orbit.sh and refresh release hashes."""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
from pathlib import Path
import re
import struct
import tempfile


ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "SC-Orbit.sh"
TRACKED_BINARY = ROOT / "SC-Orbit-driver.armv7l"
MANIFEST = ROOT / "SHA256SUMS.txt"
MANIFEST_FILES = (
    "CREDITS.txt",
    "LICENSE",
    "README.md",
    "README.txt",
    "COMPATIBILITY.md",
    "Dockerfile.armv7",
    "Makefile",
    "SC-Orbit-driver.c",
    "SC-Orbit-driver.armv7l",
    "SC-Orbit.sh",
    "tools/package.py",
)
HEADER = re.compile(rb"(?m)^BIN_SIZE=\d+\nBIN_SHA=\"[0-9a-f]{64}\"$")
PAYLOAD = re.compile(
    rb"(?ms)^#__SC_ORBIT_BINARY_BEGIN__\n.*?^#__SC_ORBIT_BINARY_END__$"
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def atomic_write(path: Path, data: bytes) -> None:
    mode = path.stat().st_mode & 0o777 if path.exists() else 0o644
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    except BaseException:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass
        raise


def verify_arm_elf(binary: bytes, path: Path) -> None:
    if len(binary) < 52 or binary[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError(f"{path} is not a little-endian ELF32 ARM candidate")
    e_type, e_machine = struct.unpack_from("<HH", binary, 16)
    e_flags = struct.unpack_from("<I", binary, 36)[0]
    if e_type != 2 or e_machine != 40:
        raise ValueError(f"{path} must be an ARM executable")
    if (e_flags & 0xFF000000) != 0x05000000 or (e_flags & 0x600) != 0x200:
        raise ValueError(f"{path} must use ARM EABI5 soft-float ABI")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        default=ROOT / "obj/SC-Orbit-driver.armv7l",
        help="candidate ARMv7 executable (default: obj/SC-Orbit-driver.armv7l)",
    )
    args = parser.parse_args()
    candidate_path = args.binary.resolve()
    binary = candidate_path.read_bytes()
    verify_arm_elf(binary, candidate_path)

    installer = INSTALLER.read_bytes()
    header_matches = list(HEADER.finditer(installer))
    payload_matches = list(PAYLOAD.finditer(installer))
    if len(header_matches) != 1 or len(payload_matches) != 1:
        raise ValueError("installer must contain exactly one binary header and payload block")

    header = f'BIN_SIZE={len(binary)}\nBIN_SHA="{sha256(binary)}"'.encode()
    encoded = base64.b64encode(binary)
    payload_lines = b"\n".join(
        b"#" + encoded[i : i + 76] for i in range(0, len(encoded), 76)
    )
    payload = b"#__SC_ORBIT_BINARY_BEGIN__\n" + payload_lines + b"\n#__SC_ORBIT_BINARY_END__"

    # Validate every output before making any file changes.
    updated_installer = HEADER.sub(header, installer, count=1)
    updated_installer = PAYLOAD.sub(payload, updated_installer, count=1)
    if base64.b64decode(
        b"".join(line[1:] for line in payload_lines.splitlines()), validate=True
    ) != binary:
        raise ValueError("generated installer payload failed round-trip verification")
    for name in MANIFEST_FILES:
        if not (ROOT / name).is_file():
            raise FileNotFoundError(f"required package file is missing: {name}")
    generated = {
        TRACKED_BINARY: binary,
        INSTALLER: updated_installer,
    }
    manifest_lines = []
    for name in MANIFEST_FILES:
        path = ROOT / name
        data = generated.get(path, path.read_bytes())
        manifest_lines.append(f"{sha256(data)}  {name}\n")
    manifest = "".join(manifest_lines).encode()

    atomic_write(TRACKED_BINARY, binary)
    atomic_write(INSTALLER, updated_installer)
    atomic_write(MANIFEST, manifest)
    print(f"embedded {len(binary)} bytes from {candidate_path}")
    print(f"binary sha256 {sha256(binary)}")
    print(f"installer sha256 {sha256(updated_installer)}")
    print(f"wrote {len(MANIFEST_FILES)} checksums to {MANIFEST.name}")


if __name__ == "__main__":
    main()
