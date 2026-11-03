#!/usr/bin/env python3
"""Verify the merged ESP32-C3 firmware layout produced by idf.py merge-bin."""

from __future__ import annotations

import hashlib
import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


EXPECTED_IMAGES = (
    (0x0000, "bootloader/bootloader.bin"),
    (0x8000, "partition_table/partition-table.bin"),
    (0x10000, "passport-os.bin"),
)

FLASH_SIZE = 8 * 1024 * 1024
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
APP_MAX_SIZE = 0x400000
CARDID_OFFSET = 0x410000
CARDID_SIZE = 0x4000
RECOVERY_OFFSET = 0x700000
RECOVERY_SIZE = 0x100000
ENTRY = struct.Struct("<HBBII16sI")
# ESP32-C3 has no PSRAM: IRAM code, .data and .bss all come out of the same SRAM
# block that becomes the first heap region (heap_init "At <heap_start> ... RAM").
# Option C (-Os + IRAM to flash) left ~78 KB there; the old -Og build had ~37 KB
# and failed AAC/MP3/TLS allocations. Keep ~4 KB of slack for normal growth.
HEAP_REGION_END = 0x3FCC0000
MIN_STATIC_HEAP = 72 * 1024
ELF_HEADER = struct.Struct("<16sHHIIIIIHHHHHH")
ELF_SECTION = struct.Struct("<IIIIIIIIII")


@dataclass(frozen=True)
class Partition:
    kind: int
    subtype: int
    offset: int
    size: int
    label: str

    @property
    def end(self) -> int:
        return self.offset + self.size


def parse_partition_table(raw: bytes) -> tuple[list[Partition], bool]:
    """Parse an ESP-IDF table and verify its optional MD5 marker."""
    if len(raw) < PARTITION_TABLE_SIZE:
        raise ValueError("partition table is truncated")

    partitions: list[Partition] = []
    found_md5 = False
    for cursor in range(0, PARTITION_TABLE_SIZE, ENTRY.size):
        magic = int.from_bytes(raw[cursor : cursor + 2], "little")
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            expected = hashlib.md5(raw[:cursor]).digest()
            actual = raw[cursor + 16 : cursor + 32]
            if actual != expected:
                raise ValueError("partition table MD5 marker does not match")
            found_md5 = True
            break
        if magic != 0x50AA:
            raise ValueError(f"invalid partition entry at table offset 0x{cursor:x}")

        _, kind, subtype, offset, size, label_raw, _ = ENTRY.unpack_from(raw, cursor)
        label = label_raw.split(b"\0", 1)[0].decode("ascii", "strict")
        if not label or not size or offset < 0x9000 or offset + size > FLASH_SIZE:
            raise ValueError(f"invalid partition bounds for {label!r}")
        partitions.append(Partition(kind, subtype, offset, size, label))

    if not partitions:
        raise ValueError("partition table is empty")
    return partitions, found_md5


def verify_protected_layout(merged: bytes, build_dir: Path) -> None:
    """Enforce the protected partition and merged-artifact layout."""
    table = merged[
        PARTITION_TABLE_OFFSET : PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE
    ]
    partitions, found_md5 = parse_partition_table(table)
    if not found_md5:
        raise ValueError("partition table has no MD5 marker")

    by_label = {item.label: item for item in partitions}
    # Match the committed Passport OS layout, not the predecessor project.
    # Recovery is a reserved physical region even though it has no CSV entry.
    expected = {
        "nvs": Partition(1, 2, 0x9000, 0x6000, "nvs"),
        "phy_init": Partition(1, 1, 0xF000, 0x1000, "phy_init"),
        "storage": Partition(1, 0x82, 0x420000, 0x200000, "storage"),
        "factory": Partition(0, 0, 0x10000, APP_MAX_SIZE, "factory"),
        "cardid": Partition(1, 2, CARDID_OFFSET, CARDID_SIZE, "cardid"),
    }
    if "recovery" in by_label:
        expected["recovery"] = Partition(0, 0x20, RECOVERY_OFFSET, RECOVERY_SIZE, "recovery")
    for label, wanted in expected.items():
        if by_label.get(label) != wanted:
            raise ValueError(f"partition {label!r} must remain {wanted}, got {by_label.get(label)}")

    ordered = sorted(partitions, key=lambda item: item.offset)
    for left, right in zip(ordered, ordered[1:]):
        if left.end > right.offset:
            raise ValueError(f"partitions {left.label!r} and {right.label!r} overlap")
    for item in partitions:
        if item.label != "cardid" and item.offset < CARDID_OFFSET + CARDID_SIZE and CARDID_OFFSET < item.end:
            raise ValueError(f"partition {item.label!r} overlaps protected cardid")
        if item.label != "recovery" and item.offset < RECOVERY_OFFSET + RECOVERY_SIZE and RECOVERY_OFFSET < item.end:
            raise ValueError(f"partition {item.label!r} overlaps permanent Recovery")

    app_path = build_dir / "passport-os.bin"
    app_size = app_path.stat().st_size
    if app_size > APP_MAX_SIZE:
        raise ValueError(f"application is {app_size} bytes; limit is {APP_MAX_SIZE}")
    if len(merged) <= 0x10000 or merged[0x10000] != 0xE9:
        raise ValueError("merged artifact has no ESP application image at 0x10000")

    # A derivative may add resource partitions after cardid. The merged file is
    # still acceptable only if protected regions contain padding, never a real
    # device identity or a replacement Recovery payload.
    for label, offset, size in (
        ("cardid", CARDID_OFFSET, CARDID_SIZE),
        ("recovery", RECOVERY_OFFSET, RECOVERY_SIZE),
    ):
        payload = merged[offset : min(len(merged), offset + size)]
        if any(byte != 0xFF for byte in payload):
            raise ValueError(f"merged artifact contains forbidden {label} payload bytes")

    print(f"Protected firmware layout: PASS (app {app_size} / {APP_MAX_SIZE} bytes)")


def elf_sections(raw: bytes) -> dict[str, tuple[int, int]]:
    """Return {name: (address, size)} from a little-endian ELF32 image."""
    if raw[:4] != b"\x7fELF" or raw[4] != 1 or raw[5] != 1:
        raise ValueError("application ELF is not little-endian ELF32")
    header = ELF_HEADER.unpack_from(raw, 0)
    shoff, shentsize, shnum, shstrndx = header[6], header[11], header[12], header[13]
    entries = [ELF_SECTION.unpack_from(raw, shoff + index * shentsize) for index in range(shnum)]
    names = entries[shstrndx]
    strings = raw[names[4] : names[4] + names[5]]
    return {
        strings[entry[0] : strings.index(b"\0", entry[0])].decode("ascii"): (entry[3], entry[5])
        for entry in entries
    }


def verify_sram_budget(build_dir: Path) -> None:
    """Fail when static SRAM use leaves too little room for the first heap region."""
    sections = elf_sections((build_dir / "passport-os.elf").read_bytes())
    if ".dram0.heap_start" not in sections:
        raise ValueError("application ELF has no .dram0.heap_start section")
    heap = HEAP_REGION_END - sections[".dram0.heap_start"][0]
    usage = ", ".join(
        f"{name} {sections.get(name, (0, 0))[1]}"
        for name in (".iram0.text", ".dram0.data", ".dram0.bss")
    )
    if heap < MIN_STATIC_HEAP:
        raise ValueError(
            f"first heap region is {heap} bytes; budget is {MIN_STATIC_HEAP} ({usage}). "
            "Check -Os, IRAM placement and new static buffers before raising the budget"
        )
    print(f"SRAM budget: PASS (first heap region {heap} / min {MIN_STATIC_HEAP} bytes; {usage})")


def verify_public_defaults(build_dir: Path) -> None:
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    if "PASSPORT_PUBLIC_RELEASE:BOOL=ON" not in cache.splitlines():
        raise ValueError("public build must enable PASSPORT_PUBLIC_RELEASE")
    defaults = (build_dir / "esp-idf/passport_muse/muse_proxy_defaults.h").read_text(encoding="utf-8")
    lines = [line.strip() for line in defaults.splitlines()]
    if '#define MUSE_DEFAULT_PROXY_HOST ""' not in lines or '#define MUSE_DEFAULT_PROXY_PORT 0' not in lines:
        raise ValueError("public build must have empty proxy defaults")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", nargs="?", default="build")
    parser.add_argument("--public", action="store_true")
    args = parser.parse_args()
    build_dir = Path(args.build_dir).resolve()
    merged_path = build_dir / "FoloToy-AI-Passport-full.bin"
    flash_args_path = build_dir / "flash_args"

    if not merged_path.is_file() or not flash_args_path.is_file():
        print("ERROR: merged firmware or flash_args is missing", file=sys.stderr)
        return 1

    flash_args = flash_args_path.read_text(encoding="utf-8")
    if "--flash_size 8MB" not in flash_args:
        print("ERROR: flash_args does not select the required 8 MB flash size", file=sys.stderr)
        return 1

    merged = merged_path.read_bytes()
    for offset, relative_name in EXPECTED_IMAGES:
        image_path = build_dir / relative_name
        if not image_path.is_file():
            print(f"ERROR: missing image {image_path}", file=sys.stderr)
            return 1
        image = image_path.read_bytes()
        if merged[offset : offset + len(image)] != image:
            print(f"ERROR: {relative_name} differs at merged offset 0x{offset:x}", file=sys.stderr)
            return 1
        print(f"Verified {relative_name}: {len(image)} bytes at 0x{offset:x}")

    if len(merged) > FLASH_SIZE:
        print("ERROR: merged firmware exceeds 8 MB", file=sys.stderr)
        return 1

    try:
        verify_protected_layout(merged, build_dir)
        verify_sram_budget(build_dir)
        if args.public:
            verify_public_defaults(build_dir)
            print("Public build defaults: PASS")
    except (OSError, UnicodeDecodeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"Merged firmware: PASS ({len(merged)} bytes, flash at 0x0)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
