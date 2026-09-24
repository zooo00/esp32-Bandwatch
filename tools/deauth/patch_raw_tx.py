#!/usr/bin/env python3
"""Post-link patch: neutralise libnet80211's raw-TX subtype gate (docs/DEVELOPER.md §11).

WHAT THIS IS FOR
    Bandwatch is a Wi-Fi security research tool and its deauth feature is documented as such. Use this
    only against networks you own or are authorised to test. A broadcast deauth disconnects *every*
    station on the target BSS, not just yours, and is illegal in many jurisdictions against third
    parties. The firmware's dead-man's switch (kDeauthMaxMs) still applies.

WHY A BINARY PATCH
    esp_wifi_80211_tx() calls ieee80211_raw_frame_sanity_check() as its first act and bails if it
    returns nonzero. That check rejects the deauth/disassoc subtypes, which is why the raw path returns
    ESP_ERR_INVALID_ARG (measured: 304/304 rejections, nothing on air).

    The usual escape, -Wl,--wrap=ieee80211_raw_frame_sanity_check, DOES NOT WORK here: in core 3.3.11
    the check and its only caller are both inside ieee80211_output.o, so the call is bound within the
    object file and is never an undefined reference for ld to redirect. Verified on esp32, esp32s3,
    esp32c3, esp32c5 and esp32c6 - all the same layout. The blob is closed source and identical in
    ESP-IDF, so rewriting the firmware or switching framework does not change this.

    That leaves patching the linked image. We overwrite the function's prologue with "return 0":
        c.li a0, 0   (0x4501)
        c.jr ra      (0x8082)

FRAGILITY (read this before relying on it)
    The address is resolved from the ELF on every run because it MOVES whenever the sketch changes -
    it shifted from 0x420ff3b0 to 0x420ff436 just from adding a diagnostic command. Never hardcode it.
    This is CLAUDE.md rule 8 fragility squared: it is pinned to one exact core build, and a core bump
    changes behaviour silently rather than failing loudly. Re-verify with tools/witness/verify.py after
    any toolchain change.

    Disabling the check wholesale also lets malformed frames through to the driver, not just deauths.
    If the firmware starts behaving oddly after this, suspect the patch first.

Usage:
    ./build.sh                                  # produce build/bandwatch.ino.bin
    python3 tools/deauth/patch_raw_tx.py        # writes build/bandwatch.ino.patched.bin
    python3 tools/deauth/patch_raw_tx.py --dry-run          # locate the function, write nothing
    python3 tools/deauth/patch_raw_tx.py --flash --port /dev/cu.usbmodem1101

There is no unpatch: the patch is applied to a copy, so reverting means flashing the stock image
(./build.sh --upload --no-patch).
"""
import argparse, os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(REPO, "build")
ELF = os.path.join(BUILD, "bandwatch.ino.elf")
BIN = os.path.join(BUILD, "bandwatch.ino.bin")
OUT = os.path.join(BUILD, "bandwatch.ino.patched.bin")

SYMBOL = "ieee80211_raw_frame_sanity_check"
RET0 = bytes([0x01, 0x45, 0x82, 0x80])   # c.li a0,0 ; c.jr ra   (little-endian)
SIGLEN = 24                              # bytes of prologue used to locate the function in the .bin



def reseal_image(data):
    """Recompute the ESP32 image checksum and SHA-256 after patching bytes in place.

    An app image ends with a 1-byte XOR checksum over all segment data and (when the header's
    hash_appended flag is set) a 32-byte SHA-256 over everything before it. Patching a byte without
    resealing leaves both stale, and the second-stage bootloader refuses the image with
    "Checksum failed ... No bootable app partitions" - an unbootable board, recoverable only by
    holding BOOT to force ROM download mode. Learned the hard way.
    """
    import hashlib, struct
    d = bytearray(data)
    if d[0] != 0xE9:
        sys.exit("not an ESP32 app image (bad magic)")
    seg_count = d[1]
    hash_appended = d[23] == 1

    off = 24
    checksum = 0xEF
    for _ in range(seg_count):
        _addr, length = struct.unpack_from("<II", d, off)
        off += 8
        for b in d[off:off + length]:
            checksum ^= b
        off += length

    cksum_pos = off + (15 - (off % 16))      # pad so the checksum lands on the 16-byte boundary
    d[cksum_pos] = checksum

    if hash_appended:
        digest = hashlib.sha256(bytes(d[:cksum_pos + 1])).digest()
        d[cksum_pos + 1:cksum_pos + 33] = digest
    return bytes(d), cksum_pos, hash_appended


def read_vaddr(elf_path, vaddr, n):
    """Read n bytes at a virtual address by walking the ELF32 little-endian program headers."""
    import struct
    d = open(elf_path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        return None
    e_phoff, = struct.unpack_from("<I", d, 0x1c)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 0x2a)
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", d, off)
        if p_type == 1 and p_vaddr <= vaddr < p_vaddr + p_filesz:      # PT_LOAD
            start = p_offset + (vaddr - p_vaddr)
            return d[start:start + n]
    return None


def sync_flashed_reference(written):
    """Tell the Arduino core's fast-reflash cache what we just put on the chip.

    Core 3.3.11 uploads through tools/flasher.py, an esptool wrapper that passes --diff-with a
    reference copy of the last image it flashed (build/bandwatch.ino_flashed.bin) and only writes the
    sectors that differ. Flashing with esptool directly, as --flash does, leaves that reference
    describing an image that is no longer on the chip, so the next `./build.sh --upload` diffs against
    the wrong baseline and can skip sectors that really do differ - an incoherent flash that looks
    like a successful one. build.sh itself is unaffected: it swaps the patched image into
    bandwatch.ino.bin before the upload, so the wrapper records the right bytes.

    Copying the image we actually wrote into that reference is exactly what the wrapper would have
    done. A failure here is not fatal - it only costs a full flash next time - but say so.
    """
    import shutil
    ref = os.path.join(BUILD, "bandwatch.ino_flashed.bin")
    try:
        shutil.copy2(written, ref)
        print(f"updated {os.path.basename(ref)} (the core's --diff-with reference)")
    except OSError as e:
        print(f"warning: could not update {ref} ({e}).\n"
              f"         Delete it by hand before the next ./build.sh --upload, or that upload will "
              f"diff against a stale baseline.")


def tool(name):
    base = os.path.expanduser("~/Library/Arduino15/packages/esp32/tools/esp-rv32")
    for root, _, files in os.walk(base):
        if name in files:
            return os.path.join(root, name)
    sys.exit(f"{name} not found under {base}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", action="store_true", help="flash the patched image after writing it")
    ap.add_argument("--port", default="/dev/cu.usbmodem1101")
    # Was called --revert, which it never was: it only ever reported and stopped, so anyone reaching
    # for it to undo a patched flash got a success message and a still-patched board. Renamed rather
    # than aliased, so the old spelling fails loudly instead of doing nothing under a promising name.
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve and locate the function, then stop without writing anything")
    a = ap.parse_args()

    for f in (ELF, BIN):
        if not os.path.exists(f):
            sys.exit(f"missing {f} - run ./build.sh first")

    nm = tool("riscv32-esp-elf-nm")   # nm only: the ELF is parsed directly below, objdump is not used

    addr = None
    for line in subprocess.run([nm, ELF], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == SYMBOL and p[1] in "Tt":
            addr = int(p[0], 16)
    if addr is None:
        sys.exit(f"symbol {SYMBOL} not found in {ELF}")
    print(f"{SYMBOL} @ 0x{addr:08x}  (resolved from this build's ELF)")

    # Pull the real prologue bytes out of the ELF so we can find the same code inside the .bin.
    # Parsed straight from the program headers - objdump's text output is not worth scraping.
    sig = read_vaddr(ELF, addr, SIGLEN)
    if sig is None or len(sig) < SIGLEN:
        sys.exit(f"could not read {SIGLEN} bytes at 0x{addr:08x} from {ELF}")
    print(f"prologue signature: {sig[:8].hex()}...")

    image = open(BIN, "rb").read()
    hits = []
    start = image.find(sig)
    while start != -1:
        hits.append(start)
        start = image.find(sig, start + 1)
    if len(hits) != 1:
        sys.exit(f"expected exactly 1 match in the image, found {len(hits)} - refusing to patch blindly")
    off = hits[0]
    print(f"located in {os.path.basename(BIN)} at file offset 0x{off:x}")

    if a.dry_run:
        print("--dry-run given: reporting only, nothing written")
        return

    patched = bytearray(image)
    patched[off:off + len(RET0)] = RET0
    sealed, cksum_pos, hashed = reseal_image(patched)
    open(OUT, "wb").write(sealed)
    print(f"resealed: checksum at 0x{cksum_pos:x}" + (" + SHA-256" if hashed else " (no hash)"))
    print(f"wrote {OUT}")
    print(f"  {sig[:4].hex()}  ->  {RET0.hex()}   (c.li a0,0 ; c.jr ra)")
    print("  the original build/bandwatch.ino.bin is untouched")

    if a.flash:
        base = os.path.expanduser("~/Library/Arduino15/packages/esp32/tools/esptool_py")
        esptool = next((os.path.join(r, "esptool")
                        for r, _, f in os.walk(base) if "esptool" in f), "esptool")
        cmd = [esptool, "--chip", "esp32c5", "--port", a.port, "--baud", "460800",
               "write-flash", "0x10000", OUT]
        print("\n$ " + " ".join(cmd))
        subprocess.run(cmd, check=True)
        sync_flashed_reference(OUT)
        print("\nflashed. Verify on air with:  python3 tools/witness/verify.py")


if __name__ == "__main__":
    main()
