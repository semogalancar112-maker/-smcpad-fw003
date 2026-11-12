# Kit pembangunan SMC-PAD fw003 + SmcPadFx

Kit ini berisi berkas yang diperlukan untuk membangun firmware open-source
`fedepaj/open-smc-pad` (commit `21cc306a10fe1c5c53e6828100d84b32396cae44`)
dengan patch layout fw003, serta sketch Arduino `SmcPadFx` untuk efek LED
custom / running text. Build dilakukan melalui **GitHub Actions** (lokal tidak
mungkin karena Smart App Control memblokir compiler).

## Isi kit

| Path | Keterangan |
|------|------------|
| `.github/workflows/build.yml` | Workflow CI: checkout upstream @ SHA, submodules, unduh toolchain JieLi, terapkan patch, build firmware + bootloader + sketch, paketkan artefak ke zip. |
| `patches/fw003.py` | Skrip Python (tanpa butuh `patch`/`git`) yang menerapkan semua konstanta/layout fw003 pada upstream. |
| `sketch/SmcPadFx/` | Sketch Arduino: `SmcPadFx.ino`, `Font5x7.h`, `Midi.h`. |
| `tools/build_fw003_artifacts.py` | Bangun zip artefak pemasangan (install.bin, boot_area.bin, app_area.bin, boot_update.bin, restore_stock.bin, manifest.json). |
| `README.md` | Berkas ini. |

## Alur CI (ringkas)

1. Checkout upstream `open-smc-pad` @ `21cc306a...` + submodules (SDK, TinyUSB, ArduinoCore-API).
2. Salin kit ke dalam upstream (patch, sketch, tools).
3. `python3 patches/fw003.py .` → terapkan konstanta fw003.
4. Unduh toolchain + post-build JieLi dari aliyuncs (verifikasi SHA-256).
5. `SMCPAD_ARDUINO=1 SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh` → precompiled core.
6. `fw/boot/build.sh` → bootloader + installer.
7. `tools/build_fw003_artifacts.py` → zip artefak.
8. Unggah zip sebagai GitHub artifact; release jika tag `v*`.

## Konstanta fw003 (patch)

| Konstanta | Nilai | File |
|-----------|-------|------|
| `APP_AREA_END` | `0x67000` | `fw/boot/boot.h` |
| `STAGE_END` | `0x75000` | `fw/boot/boot.h`, `host/bootpkg.py` |
| `DEST_MAX` | `0x66000` | `fw/boot/boot.h` |
| `VM` | `(0x67000, 0xE000)` | `host/bootpkg.py`, `arduino/release/make_template.py` |
| `PRCT` | `0x67000` | `host/jlimage.py`, `make_template.py` |
| `AREA_MAX` (sketch) | `0x67000 - 0x9000 = 0x5E000` | `arduino/smcpad/tools/smcpad_pack.c` |

## Sketch SmcPadFx

Efek yang tersedia: `static`, `rainbow`, `chase`, `scanner`, `pulse`, `VU`,
`sparkle`, `marquee` (running text via font 5x7).

Kontrol:
- **SHIFT + pad** → ganti efek berikutnya
- **PLAY** → tap tempo
- **Knob 1** → kecepatan efek
- **Knob 2** → kecerahan
- **Note On MIDI** → VU flash pada pad (note % 16)
- **CC 1** → kecerahan, **CC 7** → kecepatan
- **MIDI Clock 0xF8** → auto-tempo dari MIDI clock
- **SysEx** `F0 00 12 03 01 <teks> F7` → upload teks running text

Settings disimpan ke EEPROM (4096 B, key 40-47).

## Artefak output (zip)

- `install.bin` — installer area (SFC-encrypted untuk flash 0x41000)
- `boot_area.bin` — bootloader area plaintext untuk staging 0x43000
- `app_area.bin` — sketch SmcPadFx sebagai app area di 0x9000
- `boot_update.bin` — paket DFU alt 2 (update bootloader, erase_end 0x9000)
- `restore_stock.bin` — paket DFU alt 2 (restore stok, erase_end 0x26000)
- `manifest.json` — SHA-256 + CRC setiap berkas + konstanta layout

## Langkah berikutnya (user)

1. Buat repo GitHub (publik) dan unggah isi folder `kit/` via web UI (tanpa git/token), atau buat akun + personal access token untuk unggah via API.
2. Aktifkan GitHub Actions di repo, push workflow `build.yml`.
3. Tunggu artefak zip siap, unduh, lalu verifikasi manifest.
4. Pasang `tools/install_fw003.py` (berada di root `tools/`) dan jalankan `check` (read-only).
5. Berikan persetujuan ulang sebelum `probe --yes` / `install --yes` / `dfu`.

Detail pemasangan: lihat `work/RENCANA_INSTALL_FW003.md`.

## Peringatan

- Jangan menulis/erase apa pun ke device sebelum artefak siap, diverifikasi, dan
  persetujuan ulang user.
- Firmware baru harus tetap mengirim MIDI (pad/knob/transport) agar Midi Suite
  tetap bisa dipakai.
- Pemasangan adalah reversibel: flash dump lengkap ada sebagai acuan pemulihan,
  dan restore stok via DFU alt 2 tersedia.
