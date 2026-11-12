#!/usr/bin/env python3
"""Terapkan patch fw003 pada salinan upstream open-smc-pad (commit
21cc306a10fe1c5c53e6828100d84b32396cae44).

Usage:
    python patches/fw003.py <root_repo>

Patch ditulis dalam Python (tidak butuh `patch`/`git`) agar jalan di Windows
tanpa git/GNU tools. Setiap patch mengeluarkan PASS/FAIL dan skrip berhenti
pada gagal pertama.
"""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()

FAILS = []


def sub_file(path, old, new, count=1, name=""):
    p = ROOT / path
    s = p.read_text(encoding="utf-8", errors="replace")
    n = s.count(old)
    if n == 0:
        FAILS.append(f"{name or path}: pattern tidak ditemukan")
        return False
    if count == 1 and n > 1:
        FAILS.append(f"{name or path}: pola ada {n} occurrences, ambigu")
        return False
    p.write_text(s.replace(old, new, count if count > 0 else n), encoding="utf-8")
    print(f"  patch {name or path}: OK ({n} occurrence{'s' if n != 1 else ''})")
    return True


def sub_re(path, pat, repl, count=1, flags=0, name=""):
    p = ROOT / path
    s = p.read_text(encoding="utf-8", errors="replace")
    s2, n = re.subn(pat, repl, s, count=count, flags=flags)
    if n == 0:
        FAILS.append(f"{name or path}: regex tidak cocok")
        return False
    if count == 1 and n > 1:
        FAILS.append(f"{name or path}: regex ada {n} matches, ambigu")
        return False
    p.write_text(s2, encoding="utf-8")
    print(f"  patch {name or path}: OK ({n} match{'es' if n != 1 else ''})")
    return True


# ----------------------------------------------------------------------------
# 1. fw/boot/boot.h  — konstanta layout fw003
# ----------------------------------------------------------------------------
ok = True
ok &= sub_file("fw/boot/boot.h",
               "#define APP_AREA_END        0x71000u                        /* = the VM start both directories declare */",
               "#define APP_AREA_END        0x67000u                        /* = the VM start both directories declare (fw003) */",
               name="boot.h APP_AREA_END")

ok &= sub_re("fw/boot/boot.h",
             r"#define STAGE_END\s+0x4D000u",
             "#define STAGE_END           0x75000u            /* fw003: VM 0x67000..0x75000 */",
             name="boot.h STAGE_END")

ok &= sub_re("fw/boot/boot.h",
             r"#define DEST_MAX\s+\(APP_DIR - DEST\)",
             "#define DEST_MAX            0x66000u            /* fw003: 0x67000 - DEST */",
             name="boot.h DEST_MAX")

# APP_AREA_MAX derived automatically from APP_AREA_END - APP_DIR in boot.h,
# no separate #define to patch there.

# ----------------------------------------------------------------------------
# 2. fw/boot/install.c  — batasi erase step 3 + diag_flash_ok
# ----------------------------------------------------------------------------
# Step 3: erase INSTALL_DIR..STAGE_END -> erase hanya hingga akhir payload,
# maksimum STAGE_END.
ok &= sub_file("fw/boot/install.c",
               """    diag(3, 0);
    if (set_boot_dir(DEST)) {
        diag(3, 1);
        reset();
    }
    for (u32 a = INSTALL_DIR; a < STAGE_END; a += SECTOR_SIZE) {
        flash_erase_sector(a);
        wdt_kick();
    }
    diag(3, 0xF);""",
               """    diag(3, 0);
    if (set_boot_dir(DEST)) {
        diag(3, 1);
        reset();
    }
    /* fw003: hapus hanya installer + staging yang baru saja dipakai, hingga
     * akhir payload (rata ke batas sektor), maksimum STAGE_END. Jangan sentuh
     * VM (0x67000..) dan region atas yang mungkin masih berisi data. */
    {
        u32 end = STAGE + h.payload_off + h.payload_len;
        end = (end + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
        if (end > STAGE_END) {
            end = STAGE_END;
        }
        for (u32 a = INSTALL_DIR; a < end; a += SECTOR_SIZE) {
            flash_erase_sector(a);
            wdt_kick();
        }
    }
    diag(3, 0xF);""",
               name="install.c step 3 erase")

# diag_save: jangan menulis flash bila DIAG_PAGE di dalam staging payload.
ok &= sub_file("fw/boot/install.c",
               """static void diag_save(void)
{
    flash_erase_sector(DIAG_PAGE);
    flash_program(DIAG_PAGE, (const u8 *)diag_words, sizeof(diag_words));
}""",
               """static int diag_flash_ok(void)
{
    /* DIAG_PAGE = 0x4C000 berada di dalam rentang payload staging
     * (STAGE + payload_off .. +payload_len, 0x44000..0x69000). Menulis flash
     * di sana pada saat install ulang/restore akan merusak staging itu sendiri.
     * Default aman: jangan menulis flash. */
    (void)diag_words;
    return 0;
}

static void diag_save(void)
{
    if (!diag_flash_ok()) {
        return;                               /* RAM record only */
    }
    flash_erase_sector(DIAG_PAGE);
    flash_program(DIAG_PAGE, (const u8 *)diag_words, sizeof(diag_words));
}""",
               name="install.c diag_save")

# ----------------------------------------------------------------------------
# 3. fw/boot/dfu.c  — terima erase_end stok (0x26000) di DFU alt 2 manifest
# ----------------------------------------------------------------------------
ok &= sub_file("fw/boot/dfu.c",
               """    if (stage_check(&h) || h.erase_end != DEST_ERASE_UPDATE) {
        fail(0x313, DFU_STATUS_ERR_VERIFY);         /* no good bootloader in it, or a stock install */
        return;
    }""",
               """    if (stage_check(&h) ||
        (h.erase_end != DEST_ERASE_UPDATE && h.erase_end != DEST_ERASE_STOCK)) {
        fail(0x313, DFU_STATUS_ERR_VERIFY);         /* no good bootloader, or a stock install */
        return;
    }""",
               name="dfu.c manifest erase_end")

# ----------------------------------------------------------------------------
# 4. host/bootpkg.py  — VM & STAGE_END fw003
# ----------------------------------------------------------------------------
ok &= sub_file("host/bootpkg.py",
               "STAGE_END = 0x4D000",
               "STAGE_END = 0x75000",
               name="bootpkg.py STAGE_END")
ok &= sub_file("host/bootpkg.py",
               "VM = (0x71000, 0x4000)          # as in host/package_fw.sh",
               "VM = (0x67000, 0xE000)          # fw003: VM di 0x67000, ukuran 0xE000",
               name="bootpkg.py VM")

# ----------------------------------------------------------------------------
# 5. host/jlimage.py  — DEFAULT_DIR VM / PRCT fw003
# ----------------------------------------------------------------------------
ok &= sub_file("host/jlimage.py",
               "(0x00026000, 0x04f000, 0x12, 0x80, 0, b'VM\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\x00\\x10\\x00\\x00'),",
               "(0x00067000, 0x00e000, 0x12, 0x80, 0, b'VM\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\x00\\x10\\x00\\x00'),",
               name="jlimage.py DEFAULT_DIR VM")
ok &= sub_file("host/jlimage.py",
               "(0x00000000, 0x026000, 0x92, 0x82, 0, b'PRCT\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xffCODE'),",
               "(0x00067000, 0x067000, 0x92, 0x82, 0, b'PRCT\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xffCODE'),",
               name="jlimage.py DEFAULT_DIR PRCT")

# ----------------------------------------------------------------------------
# 6. arduino/release/make_template.py  — vm / prct_size fw003
# ----------------------------------------------------------------------------
ok &= sub_file("arduino/release/make_template.py",
               "area = J.build_app_area(b\"\\0\" * 0x200, cfg, t, entry_vma=0x1E08120, vm=(0x71000, 0x4000), prct_size=0x71000)",
               "area = J.build_app_area(b\"\\0\" * 0x200, cfg, t, entry_vma=0x1E08120, vm=(0x67000, 0xE000), prct_size=0x67000)",
               name="make_template.py vm/prct")

# ----------------------------------------------------------------------------
# 7. arduino/smcpad/tools/smcpad_pack.c  — AREA_MAX fw003
# ----------------------------------------------------------------------------
ok &= sub_file("arduino/smcpad/tools/smcpad_pack.c",
               "#define AREA_MAX    (0x71000u - 0x9000u)",
               "#define AREA_MAX    (0x67000u - 0x9000u)            /* fw003 */",
               name="smcpad_pack.c AREA_MAX")

# ----------------------------------------------------------------------------
# Hasil
# ----------------------------------------------------------------------------
print()
if FAILS:
    print("GAGAL: beberapa patch tidak dapat diterapkan:")
    for f in FAILS:
        print("  -", f)
    sys.exit(1)
print("Semua patch fw003 diterapkan dengan sukses.")
