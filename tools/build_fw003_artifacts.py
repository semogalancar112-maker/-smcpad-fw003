#!/usr/bin/env python3
"""Bangun artefak pemasangan fw003 ke dalam zip, siap untuk dfu-util +
tools/install_fw003.py.

Dijalankan di GitHub Actions setelah fw/build.sh, fw/boot/build.sh, dan
arduino-cli compile (sketch). Membaca komponen yang sudah dibangun, memaketkan
semua ke dalam zip dengan manifest.json (sha256 + CRC16/CRC32 setiap berkas).

Output zip berisi:
  install.bin        installer area (SFC-encrypted untuk flash 0x41000)
  boot_area.bin      bootloader area plaintext untuk staging di 0x43000
  app_area.bin       sketch SmcPadFx sebagai app area di 0x9000
  boot_update.bin    paket DFU alt 2: update bootloader (erase_end = 0x9000)
  restore_stock.bin  paket DFU alt 2: restore stok (erase_end = 0x26000)
                     (hanya jika --stock-app diberikan)
  manifest.json      sha256 + CRC setiap berkas + konstanta layout

Usage:
    python tools/build_fw003_artifacts.py --out dist/smc-pad-fw003-artifacts.zip
        [--boot-bin fw/boot/out/boot.bin]
        [--install-bin fw/boot/out/install.bin]
        [--sketch-bin work/flash/sketch-build/SmcPadFx.bin]
        [--stock-app <app area stok plaintext untuk restore_stock.bin>]
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

import bootpkg
import jlimage as J

# --------------------------------------------------------------------------- konstanta fw003
INSTALL_DIR = 0x41000
STAGE = 0x43000
STAGE_END = 0x75000
DEST = 0x1000
DEST_ERASE_STOCK = 0x26000
DEST_ERASE_UPDATE = 0x9000
APP_DIR = 0x9000
VM = (0x67000, 0xE000)
PRCT_SIZE = 0x67000
AREA_MAX = 0x67000 - 0x9000  # 0x5E000

SECTOR = 0x1000


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def describe(name, data):
    return {
        "name": name,
        "size": len(data),
        "sha256": sha256_hex(data),
        "crc16": f"{crc16(data):04x}",
        "crc32": f"{zlib.crc32(data) & 0xFFFFFFFF:08x}",
    }


def build_boot_area(boot_bin, cfg, template):
    """Pack bootloader sebagai app area di flash 0x1000."""
    return J.build_app_area(
        boot_bin, cfg, template,
        entry_vma=J.AREA_VMA + 0x120,
        vm=VM, prct_size=PRCT_SIZE,
    )


def build_install_area(template, cfg):
    """Installer sebagai app area di INSTALL_DIR (via bootpkg)."""
    return bootpkg.installer_area(template, cfg)


def build_stage(payload_bin, erase_end):
    """Staging area: header + payload (bootloader app area plaintext)."""
    return bootpkg.stage(erase_end, payload=payload_bin)


def build_update_package(template, cfg):
    """DFU alt 2: bootloader update (erase_end = 0x9000)."""
    return bootpkg.update_package(template, cfg)


def build_restore_package(template, cfg, stock_app_bin):
    """DFU alt 2: restore stok (erase_end = 0x26000).

    Payload = app area stok plaintext. Installer menulis balik ke 0x1000
    lalu set_boot_dir(0x1000).
    """
    from bootpkg import STAGE as _STG, INSTALL_DIR as _ID
    from bootpkg import STAGE_MAGIC, PAYLOAD_OFF, DEST, ERASE_STOCK

    payload = stock_app_bin
    head = J.read_dir(payload)[0]
    hdr = struct.pack("<7I", STAGE_MAGIC, 1, PAYLOAD_OFF, len(payload),
                      zlib.crc32(payload), DEST, ERASE_STOCK)
    hdr += struct.pack("<I", zlib.crc32(hdr))
    blob = hdr + b"\xff" * (PAYLOAD_OFF - len(hdr)) + payload

    inst = build_install_area(template, cfg)
    img = inst + b"\xff" * (_STG - _ID - len(inst)) + blob
    img = img + b"\xff" * (-len(img) % 0x1000)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, help="path zip output")
    ap.add_argument("--boot-bin", default="fw/boot/out/boot.bin")
    ap.add_argument("--install-bin", default="fw/boot/out/install.bin")
    ap.add_argument("--sketch-bin", default="work/flash/sketch-build/SmcPadFx.bin")
    ap.add_argument("--stock-app", default=None,
                    help="app area stok plaintext untuk restore_stock.bin")
    a = ap.parse_args()

    out_path = Path(a.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # Template + cfg (dari default, sama dengan stock)
    template, cfg = J.template_and_cfg()

    files = {}

    # 1. boot_area.bin (bootloader di 0x1000)
    boot_bin = Path(a.boot_bin).read_bytes()
    boot_area = build_boot_area(boot_bin, cfg, template)
    files["boot_area.bin"] = boot_area
    print(f"  boot_area.bin: {len(boot_area):#x} bytes")

    # 2. install.bin (installer area di 0x41000)
    install_area = build_install_area(template, cfg)
    files["install.bin"] = install_area
    print(f"  install.bin: {len(install_area):#x} bytes")

    # 3. app_area.bin (sketch di 0x9000) — smcpad_pack sudah menghasilkan app area
    sketch_bin = Path(a.sketch_bin)
    if not sketch_bin.exists():
        print(f"  ERROR: sketch bin tidak ada: {sketch_bin}", file=sys.stderr)
        sys.exit(1)
    app_area = sketch_bin.read_bytes()
    if len(app_area) > AREA_MAX:
        print(f"  ERROR: app area terlalu besar: {len(app_area):#x} > {AREA_MAX:#x}",
              file=sys.stderr)
        sys.exit(1)
    files["app_area.bin"] = app_area
    print(f"  app_area.bin: {len(app_area):#x} bytes")

    # 4. boot_update.bin (DFU alt 2, bootloader update)
    boot_update = build_update_package(template, cfg)
    files["boot_update.bin"] = boot_update
    print(f"  boot_update.bin: {len(boot_update):#x} bytes")

    # 5. restore_stock.bin (DFU alt 2, erase_end = 0x26000) — optional
    if a.stock_app:
        stock_app = Path(a.stock_app).read_bytes()
        restore_stock = build_restore_package(template, cfg, stock_app)
        files["restore_stock.bin"] = restore_stock
        print(f"  restore_stock.bin: {len(restore_stock):#x} bytes")
    else:
        print("  peringatan: --stock-app tidak diberikan, restore_stock.bin dilewati")

    # 6. manifest.json
    manifest = {
        "version": "0.1.0-fw003",
        "upstream_sha": "21cc306a10fe1c5c53e6828100d84b32396cae44",
        "layout": {
            "INSTALL_DIR": INSTALL_DIR,
            "STAGE": STAGE,
            "STAGE_END": STAGE_END,
            "DEST": DEST,
            "DEST_ERASE_STOCK": DEST_ERASE_STOCK,
            "DEST_ERASE_UPDATE": DEST_ERASE_UPDATE,
            "APP_DIR": APP_DIR,
            "VM": list(VM),
            "PRCT_SIZE": PRCT_SIZE,
            "AREA_MAX": AREA_MAX,
        },
        "files": [describe(n, d) for n, d in files.items()],
    }
    files["manifest.json"] = json.dumps(manifest, indent=2).encode("utf-8")

    # Tulis zip
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)

    print(f"\nArtefak ditulis ke {out_path}")
    print("Berkas dalam zip:")
    for name in files:
        print(f"  {name} ({len(files[name]):#x} bytes)")
    print("\nSelanjutnya: verifikasi manifest, lalu gunakan tools/install_fw003.py")


if __name__ == "__main__":
    main()
