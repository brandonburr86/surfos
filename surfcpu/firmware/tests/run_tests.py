#!/usr/bin/env python3
"""SurfCPU firmware tests: SurfBoot against card images
--------------------
File: run_tests.py Date: 10/15/26 (SurfCPU)
--------------------
Builds a card image for each scenario (tools/mkcard.py and tools/surfimg.py as libraries,
then exact damage where a scenario needs it), runs SurfBoot's portable code on it through
build/fw/host/fwtest (native, with AddressSanitizer and UBSan) and checks the result: the
error name, words of the sentence, lines of the boot log, the boot flags and command line
handed to the kernel. Covers the brief's storage list (absent card, failed initialization,
bad MBR/BPB/header, exFAT and FAT16 refused, missing file, truncated package, bad checksums,
out-of-range load or entry, fragmented chains, cycles and out-of-range FAT entries,
multi-cluster directories, the backup path, full volume, removal) and more, then fuzzes
the metadata with several seeds.

    python3 firmware/tests/run_tests.py [--fwtest build/fw/host/fwtest] [--jobs 4] [-k NAME]
                                        [--keep] [--fuzz-seeds 1,2,3] [--fuzz-iterations 300]
    python3 firmware/tests/run_tests.py --io-model --fwtest build/fw/host/fwtest-io
                                        (the same scenarios against the io track's SD card model)

Images go to build/fw/cards/ (sparse files) and are deleted when their scenario passes
(--keep keeps them). Exit status 0 when every scenario passes.
"""
import argparse
import concurrent.futures
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]                  # surfcpu/
sys.path.insert(0, str(ROOT / "tools"))
import mkcard                                                # noqa: E402
import surfimg                                               # noqa: E402

CARDS = ROOT / "build" / "fw" / "cards"
DDR = 0x80000000
SCENARIOS = []


def scenario(name, args=(), expect="OK", msg=None, logs=(), nologs=(), flags=None, cmdline=None, commands=None,
             mode=None):
    """Registers a scenario: its builder makes the image (or returns None for no card)."""
    def deco(fn):
        SCENARIOS.append(dict(name=name, build=fn, args=list(args), expect=expect, msg=msg, logs=list(logs),
                              nologs=list(nologs), flags=flags, cmdline=cmdline, commands=commands,
                              mode=mode or ("monitor" if commands else "boot")))
        return fn
    return deco


def payload(n=50000, seed=1):
    x = seed * 2654435761 & 0xFFFFFFFF
    out = bytearray(n)
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = x >> 24
    return bytes(out)


def pkg(n=50000, load=DDR, entry=None, mem=None, seed=1, **kw):
    kw.setdefault("name", "TestOS")
    kw.setdefault("image_version", 0x000100)
    kw.setdefault("build_id", 0x1234ABCD)
    return surfimg.make_package(payload(n, seed), load, entry, mem if mem is not None else n + 4096, **kw)


def card(path, files=None, dirs=mkcard.STANDARD_DIRS, size=64 << 20, **kw):
    c = mkcard.Card(size=size, **kw)
    for d in dirs:
        c.add_dir(d)
    for p, data in (files or {}).items():
        if data is None:
            c.add_dir(p)
        else:
            c.add_file(p, data)
    c.build(str(path))
    return path


def std(path, **kw):
    return card(path, {"/BOOT/SURFOS.BIN": pkg()}, **kw)


def patch(path, offset, data):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)


# ---------------------------------------------------------------- happy paths

@scenario("ok_sdhc", flags=0, cmdline="", logs=["no /BOOT/CMDLINE.TXT", "payload CRC", "matches (card and DDR)"])
def _(p):
    return std(p)


@scenario("ok_sdsc_v2", args=["--type", "sdsc2"], logs=["sd: SDSC (SD 2.0)"])
def _(p):
    return std(p)


@scenario("ok_sdsc_v1", args=["--type", "sdsc1"], logs=["sd: SDSC (SD 1.x)"])
def _(p):
    return std(p)


@scenario("ok_cmdline", cmdline="console=uart loglevel=3", flags=0)
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/CMDLINE.TXT": b"console=uart loglevel=3\r\nignored\r\n"})


@scenario("ok_cmdline_bom_tab", cmdline="a b c")
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/CMDLINE.TXT": b"\xef\xbb\xbfa\tb c\n"})


@scenario("ok_cmdline_127", cmdline="x" * 127, flags=0)
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/CMDLINE.TXT": b"x" * 127 + b"\n"})


@scenario("ok_cmdline_truncated", cmdline="y" * 127, flags=2, logs=["truncated to 127"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/CMDLINE.TXT": b"y" * 900})


@scenario("ok_cmdline_empty_file", cmdline="", flags=0)
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/CMDLINE.TXT": b""})


@scenario("ok_cmdline_damaged_is_ignored", cmdline="", logs=["CMDLINE.TXT ignored"])
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/CMDLINE.TXT": b"hello\n"})
    with mkcard.Image(str(p)) as im:
        im.set_first_cluster("/BOOT/CMDLINE.TXT", im.clusters + 50)
    return p


@scenario("ok_long_names_like_macos")
def _(p):
    return std(p, lfn=True)


@scenario("ok_4k_clusters", logs=["4096-byte clusters"])
def _(p):
    return std(p, size=300 << 20, cluster=4096)


@scenario("ok_32k_clusters_2g", logs=["32768-byte clusters"])
def _(p):
    return std(p, size=(2 << 30) + (64 << 20), cluster=32768)


@scenario("ok_partition_at_63", logs=["at sector 63"])
def _(p):
    return std(p, part_start=63)


@scenario("ok_one_fat")
def _(p):
    return std(p, nfats=1)


@scenario("ok_type_0b", logs=["type 0x0b"])
def _(p):
    return std(p, part_type=0x0B)


@scenario("ok_active_fat_2")
def _(p):
    std(p)
    with mkcard.Image(str(p)) as im:
        bs = bytearray(im.bs)
        struct.pack_into("<H", bs, 40, 0x81)            # mirroring off, FAT 1 active
        im.write(im.part_lba, bytes(bs))
        e = im.find("/BOOT/SURFOS.BIN")
        im.fat_set(e["cluster"], 0, fats=1)             # FAT 0 now broken; FAT 1 still good
    return p


@scenario("ok_second_partition", logs=["partition 2"])
def _(p):
    std(p, part_start=40960)
    with mkcard.Image(str(p)) as im:
        mbr = bytearray(im.mbr)
        mbr[462:478] = mbr[446:462]                     # FAT32 moves to entry 2
        mbr[446:462] = mkcard.mbr_entry(0, 0x83, 2048, 30000)
        im.write(0, bytes(mbr))
    return p


@scenario("ok_hybrid_mbr_with_gpt_entry", logs=["partition 2"])
def _(p):
    std(p, part_start=40960)
    with mkcard.Image(str(p)) as im:
        mbr = bytearray(im.mbr)
        mbr[462:478] = mbr[446:462]
        mbr[446:462] = mkcard.mbr_entry(0, 0xEE, 1, 2047)
        im.write(0, bytes(mbr))
    return p


@scenario("ok_large_4m_payload")
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(4 << 20, mem=6 << 20)})


@scenario("ok_load_just_below_ddr_top")
def _(p):
    n = 20000
    return card(p, {"/BOOT/SURFOS.BIN": pkg(n, load=0x8F000000 - 32768, mem=32768)})


@scenario("ok_entry_last_word")
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(4096, entry=DDR + 4092)})


@scenario("ok_reserved_flags_ignored", logs=["reserved and ignored"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(flags=1)})


@scenario("ok_slow_card_timing", args=["--ncr", "8", "--nac", "300"])
def _(p):
    return std(p)


@scenario("ok_acmd41_slow", args=["--acmd41-busy", "400"])
def _(p):
    return std(p)


@scenario("ok_crc_errors_retried", args=["--crc-errors", "3"])
def _(p):
    return std(p)


@scenario("ok_card_too_slow_for_fast_clock", args=["--min-div", "4"])
def _(p):
    return std(p)


@scenario("ok_no_cmd59", args=["--no-cmd59"], logs=["data CRC not checked"])
def _(p):
    return std(p)


@scenario("ok_socket_without_card_detect", args=["--no-cd"])
def _(p):
    return std(p)


@scenario("ok_token_error_retried", args=["--token-errors", "2"])
def _(p):
    return std(p)


@scenario("ok_card_busy_from_before_a_reset", args=["--busy-bytes", "4000"])
def _(p):
    return std(p)


@scenario("ok_read_timeout_retried", args=["--read-timeouts", "1"])
def _(p):
    return std(p)


@scenario("ok_full_volume")
def _(p):
    c = mkcard.Card(size=40 << 20, cluster=512)
    c.add_dir("/BOOT")
    c.add_file("/BOOT/SURFOS.BIN", pkg())
    c.fill = True
    c.build(str(p))
    return p


@scenario("ok_ddr_slow_calibration", args=["--ddr-calib-ms", "1700"], logs=["calibrated after 17"])
def _(p):
    return std(p)


@scenario("ok_path_option", args=["--path", "/BOOT/OTHER.BIN"], logs=["boot: /BOOT/OTHER.BIN"])
def _(p):
    return card(p, {"/BOOT/OTHER.BIN": pkg(seed=5)})


@scenario("ok_mkfs_fat_and_mcopy", logs=["fat32: partition 1"], cmdline="from mtools")
def _(p):
    """An image formatted by dosfstools and filled by mtools: SurfBoot reads what other tools write."""
    size = 96 << 20
    with open(p, "wb") as f:
        f.truncate(size)
    mbr = bytearray(512)
    mbr[446:462] = mkcard.mbr_entry(0x80, 0x0C, 2048, size // 512 - 2048)
    mbr[510:512] = b"\x55\xaa"
    patch(p, 0, bytes(mbr))
    run(["mkfs.fat", "-F", "32", "-n", "SURFOS", "--offset", "2048", "-S", "512", str(p),
         str((size // 512 - 2048) // 2)])
    src = CARDS / f"{p.stem}.bin"
    src.write_bytes(pkg(70000))
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    img = f"{p}@@{2048 * 512}"
    for d in ("BOOT", "SYSTEM", "USERS", "USERS/DOCUMENTS"):
        run(["mmd", "-i", img, "::/" + d], env=env)
    run(["mcopy", "-i", img, str(src), "::/BOOT/SURFOS.BIN"], env=env)
    cmd = CARDS / f"{p.stem}.txt"
    cmd.write_text("from mtools\n")
    run(["mcopy", "-i", img, str(cmd), "::/BOOT/cmdline.txt"], env=env)
    src.unlink()
    cmd.unlink()
    return p


# ---------------------------------------------------------------- the card and its initialization

@scenario("no_card", args=["--absent"], expect="NO_CARD", msg="card-detect switch is open")
def _(p):
    return None


@scenario("empty_socket_without_card_detect", args=["--absent", "--no-cd"], expect="SD_NORESP", msg="CMD0")
def _(p):
    return None


@scenario("dead_card", args=["--dead"], expect="SD_NORESP", msg="no answer to CMD0")
def _(p):
    return std(p)


@scenario("miso_stuck_low", args=["--miso-low"], expect="SD_NORESP", msg="MISO stuck low")
def _(p):
    return std(p)


@scenario("acmd41_never_ready", args=["--acmd41-busy", "-1"], expect="SD_INIT", msg="after 2 s")
def _(p):
    return std(p)


@scenario("cmd8_bad_echo", args=["--cmd8-bad-echo"], expect="SD_UNSUPPORTED", msg="CMD8 echo")
def _(p):
    return std(p)


@scenario("no_3v3", args=["--no-33v"], expect="SD_UNSUPPORTED", msg="3.3 V")
def _(p):
    return std(p)


@scenario("mmc_card", args=["--type", "mmc"], expect="SD_UNSUPPORTED", msg="MMC")
def _(p):
    return std(p)


@scenario("reads_always_error_token", args=["--token-errors", "1000"], expect="SD_READ", msg="error token")
def _(p):
    return std(p)


@scenario("reads_never_send_data", args=["--read-timeouts", "1000"], expect="SD_READ", msg="250 ms")
def _(p):
    return std(p)


@scenario("every_read_bad_crc", args=["--crc-every", "1"], expect="SD_READ", msg="CRC wrong")
def _(p):
    return std(p)


@scenario("card_removed_while_loading", args=["--remove-after", "60"], expect="NO_CARD", msg="removed")
def _(p):
    return std(p)


# ---------------------------------------------------------------- partition table and boot sector

@scenario("blank_card", expect="MBR", msg="no partition table signature")
def _(p):
    with open(p, "wb") as f:
        f.truncate(64 << 20)
    return p


@scenario("superfloppy", expect="SUPERFLOPPY", msg="no MBR partition table")
def _(p):
    with open(p, "wb") as f:
        f.truncate(64 << 20)
    run(["mkfs.fat", "-F", "32", "-n", "FLOPPY", str(p)])
    return p


@scenario("gpt_only", expect="GPT", msg="GPT")
def _(p):
    std(p)
    with mkcard.Image(str(p)) as im:
        mbr = bytearray(im.mbr)
        mbr[446:462] = mkcard.mbr_entry(0, 0xEE, 1, (64 << 20) // 512 - 1)
        im.write(0, bytes(mbr))
    return p


def set_part_type(p, t):
    with mkcard.Image(str(p)) as im:
        mbr = bytearray(im.mbr)
        mbr[446 + 4] = t
        im.write(0, bytes(mbr))


def exfat_boot_sector():
    b = bytearray(512)
    b[0:3] = b"\xeb\x76\x90"
    b[3:11] = b"EXFAT   "
    b[510:512] = b"\x55\xaa"
    return bytes(b)


@scenario("exfat_partition", expect="EXFAT", msg="exFAT")
def _(p):
    std(p)
    set_part_type(p, 0x07)
    patch(p, 2048 * 512, exfat_boot_sector())
    return p


@scenario("exfat_in_fat32_typed_partition", expect="EXFAT", msg="holds exFAT")
def _(p):
    std(p)
    patch(p, 2048 * 512, exfat_boot_sector())
    return p


@scenario("exfat_without_partition_table", expect="EXFAT", msg="without a partition table")
def _(p):
    with open(p, "wb") as f:
        f.truncate(64 << 20)
    patch(p, 0, exfat_boot_sector())
    return p


@scenario("ntfs_partition", expect="NTFS", msg="NTFS")
def _(p):
    std(p)
    set_part_type(p, 0x07)
    b = bytearray(512)
    b[0:3] = b"\xeb\x52\x90"
    b[3:11] = b"NTFS    "
    b[510:512] = b"\x55\xaa"
    patch(p, 2048 * 512, bytes(b))
    return p


@scenario("fat16_partition_type", expect="FAT1216", msg="FAT16")
def _(p):
    std(p)
    set_part_type(p, 0x06)
    return p


def mkfs_partition(p, fat, size_mb=64, ptype=0x0C):
    size = size_mb << 20
    with open(p, "wb") as f:
        f.truncate(size)
    mbr = bytearray(512)
    mbr[446:462] = mkcard.mbr_entry(0x00, ptype, 2048, size // 512 - 2048)
    mbr[510:512] = b"\x55\xaa"
    patch(p, 0, bytes(mbr))
    run(["mkfs.fat", "-F", str(fat), "--offset", "2048", str(p), str((size // 512 - 2048) // 2)])
    return p


@scenario("fat16_volume_in_fat32_partition", expect="FAT1216", msg="FAT16")
def _(p):
    return mkfs_partition(p, 16)


@scenario("fat12_volume", expect="FAT1216", msg="FAT12")
def _(p):
    return mkfs_partition(p, 12, size_mb=4)


@scenario("only_linux_partition", expect="NO_FAT32_PART", msg="no FAT32 partition")
def _(p):
    std(p)
    set_part_type(p, 0x83)
    return p


@scenario("empty_partition_table", expect="NO_FAT32_PART", msg="empty")
def _(p):
    std(p)
    patch(p, 446, bytes(64))
    return p


@scenario("bad_partition_status", expect="MBR", msg="status bytes")
def _(p):
    std(p)
    patch(p, 446, b"\x7f")
    return p


@scenario("partition_beyond_card", expect="PART_RANGE", msg="beyond the card")
def _(p):
    std(p)
    patch(p, 446 + 12, struct.pack("<I", (64 << 20) // 512))
    return p


@scenario("partition_at_sector_0", expect="PART_RANGE", msg="starts at sector 0")
def _(p):
    std(p)
    patch(p, 446 + 8, struct.pack("<I", 0))
    return p


def bpb_patch(p, offset, data):
    patch(p, 2048 * 512 + offset, data)


@scenario("bpb_4096_byte_sectors", expect="BPB", msg="4096-byte sectors")
def _(p):
    std(p)
    bpb_patch(p, 11, struct.pack("<H", 4096))
    return p


@scenario("bpb_3_sectors_per_cluster", expect="BPB", msg="not a power of two")
def _(p):
    std(p)
    bpb_patch(p, 13, b"\x03")
    return p


@scenario("bpb_no_fats", expect="BPB", msg="0 FATs")
def _(p):
    std(p)
    bpb_patch(p, 16, b"\x00")
    return p


@scenario("bpb_no_reserved_sectors", expect="BPB", msg="no reserved sectors")
def _(p):
    std(p)
    bpb_patch(p, 14, struct.pack("<H", 0))
    return p


@scenario("bpb_no_signature", expect="BPB", msg="no 55 aa signature")
def _(p):
    std(p)
    bpb_patch(p, 510, b"\x00\x00")
    return p


@scenario("bpb_no_jump", expect="BPB", msg="jump instruction")
def _(p):
    std(p)
    bpb_patch(p, 0, b"\x00")
    return p


@scenario("bpb_volume_bigger_than_partition", expect="BPB", msg="larger than its partition")
def _(p):
    std(p)
    with mkcard.Image(str(p)) as im:
        bpb_patch(p, 32, struct.pack("<I", im.part_sectors + 4096))
    return p


@scenario("bpb_fat_too_small", expect="BPB", msg="too small")
def _(p):
    std(p)
    with mkcard.Image(str(p)) as im:
        # shrink the FAT size field while keeping the data area where it was
        bpb_patch(p, 36, struct.pack("<I", im.fatsz // 4))
        bpb_patch(p, 14, struct.pack("<H", im.reserved + 2 * (im.fatsz - im.fatsz // 4)))
    return p


@scenario("bpb_root_cluster_out_of_range", expect="BPB", msg="root directory starts at cluster")
def _(p):
    std(p)
    bpb_patch(p, 44, struct.pack("<I", 0x0FFFFFF0))
    return p


@scenario("bpb_unknown_fat32_version", expect="BPB", msg="version")
def _(p):
    std(p)
    bpb_patch(p, 42, struct.pack("<H", 0x0100))
    return p


@scenario("bpb_active_fat_out_of_range", expect="BPB", msg="active")
def _(p):
    std(p)
    bpb_patch(p, 40, struct.pack("<H", 0x83))
    return p


@scenario("bpb_fat16_fields_set", expect="BPB", msg="FAT16 fields")
def _(p):
    std(p)
    bpb_patch(p, 17, struct.pack("<H", 512))
    return p


# ---------------------------------------------------------------- directories and files

@scenario("no_boot_directory", expect="NOT_FOUND", msg="/BOOT does not exist",
          logs=["trying /BOOT/BACKUP.BIN"])
def _(p):
    return card(p, {}, dirs=("/SYSTEM",))


@scenario("only_backup_present", flags=1, logs=["/BOOT/SURFOS.BIN does not exist", "trying /BOOT/BACKUP.BIN"])
def _(p):
    return card(p, {"/BOOT/BACKUP.BIN": pkg(seed=2)})


@scenario("nothing_to_boot", expect="NOT_FOUND", msg="/BOOT/SURFOS.BIN does not exist")
def _(p):
    return card(p, {})


@scenario("surfos_is_a_directory", expect="NOT_FILE", msg="directory")
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": None})


@scenario("boot_is_a_file", expect="NOT_DIR", msg="/BOOT is a file")
def _(p):
    return card(p, {"/BOOT": b"not a directory"}, dirs=())


@scenario("multi_cluster_boot_directory", logs=["boot: TestOS"])
def _(p):
    files = {f"/BOOT/F{i:04d}.DAT": b"x" * 10 for i in range(120)}
    files["/BOOT/SURFOS.BIN"] = pkg()                     # added last: in the last cluster
    return card(p, files, cluster=512)


@scenario("multi_cluster_boot_directory_fragmented", logs=["boot: TestOS"])
def _(p):
    files = {f"/BOOT/F{i:04d}.DAT": b"y" * 700 for i in range(80)}
    files["/BOOT/SURFOS.BIN"] = pkg()
    return card(p, files, cluster=512, fragment="interleave")


@scenario("multi_cluster_root_directory")
def _(p):
    files = {f"/R{i:05d}.TXT": b"r" for i in range(200)}
    files["/BOOT/SURFOS.BIN"] = pkg()
    return card(p, files, dirs=(), cluster=512)


@scenario("deleted_entry_before_the_file")
def _(p):
    card(p, {"/BOOT/OLD.BIN": pkg(seed=9), "/BOOT/SURFOS.BIN": pkg()})
    with mkcard.Image(str(p)) as im:
        e = im.find("/BOOT/OLD.BIN")
        im.poke(e["offset"], b"\xe5URFOS  BIN")          # a deleted SURFOS.BIN entry first
    return p


@scenario("fragmented_interleaved", logs=["fragments"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(80000), "/BOOT/BACKUP.BIN": pkg(80000, seed=3)}, cluster=512,
                fragment="interleave")


@scenario("fragmented_reverse_chain")
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(30000)}, cluster=512, fragment="reverse")


for _seed in (1, 2, 3, 4, 5):
    @scenario(f"fragmented_random_seed{_seed}")
    def _(p, seed=_seed):
        return card(p, {"/BOOT/SURFOS.BIN": pkg(40000, seed=seed), "/BOOT/CMDLINE.TXT": b"seed\n",
                        "/SYSTEM/A.CFG": b"a" * 3000}, cluster=512, fragment="random", seed=seed)


def chain_of(p, path):
    with mkcard.Image(str(p)) as im:
        e = im.find(path)
        return im.chain(e["cluster"]), im.clusters


def fat_set(p, cluster, value):
    with mkcard.Image(str(p)) as im:
        im.fat_set(cluster, value)


@scenario("file_chain_loops", expect=("FAT_CYCLE", "FAT_CHAIN"), msg="loop")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000)}, cluster=512)
    ch, _ = chain_of(p, "/BOOT/SURFOS.BIN")
    fat_set(p, ch[10], ch[3])
    return p


@scenario("file_chain_points_to_itself", expect="FAT_CYCLE", msg="loops")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000)}, cluster=512)
    ch, _ = chain_of(p, "/BOOT/SURFOS.BIN")
    fat_set(p, ch[5], ch[5])
    return p


@scenario("directory_chain_loops", expect="FAT_CYCLE", msg="loops")
def _(p):
    # . and .. plus 46 entries fill three 512-byte clusters exactly: no end marker stops the scan
    files = {f"/BOOT/D{i:03d}.DAT": b"d" for i in range(46)}
    card(p, files, cluster=512)
    ch, _ = chain_of(p, "/BOOT")
    assert len(ch) == 3
    fat_set(p, ch[-1], ch[0])                            # SURFOS.BIN is not there: the walk goes round
    return p


@scenario("chain_points_to_free_cluster", expect="FAT_CHAIN", msg="free")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000)}, cluster=512)
    ch, _ = chain_of(p, "/BOOT/SURFOS.BIN")
    fat_set(p, ch[4], 0)
    return p


@scenario("chain_points_beyond_last_cluster", expect="FAT_CHAIN", msg="beyond the last cluster")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000)}, cluster=512)
    ch, n = chain_of(p, "/BOOT/SURFOS.BIN")
    fat_set(p, ch[4], n + 7)
    return p


@scenario("chain_reaches_bad_cluster_mark", expect="FAT_CHAIN", msg="bad")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000)}, cluster=512)
    ch, _ = chain_of(p, "/BOOT/SURFOS.BIN")
    fat_set(p, ch[4], 0x0FFFFFF7)
    return p


@scenario("chain_points_to_reserved_cluster", expect="FAT_CHAIN", msg="reserved")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000)}, cluster=512)
    ch, _ = chain_of(p, "/BOOT/SURFOS.BIN")
    fat_set(p, ch[4], 1)
    return p


@scenario("chain_shorter_than_size", expect="FAT_CHAIN", msg="ends after")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000)}, cluster=512)
    ch, _ = chain_of(p, "/BOOT/SURFOS.BIN")
    fat_set(p, ch[6], 0x0FFFFFFF)
    return p


@scenario("chain_longer_than_size", expect="FAT_CHAIN", msg="goes on after the last cluster")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg(20000), "/BOOT/X.DAT": b"z" * 4000}, cluster=512)
    ch, _ = chain_of(p, "/BOOT/SURFOS.BIN")
    ch2, _ = chain_of(p, "/BOOT/X.DAT")
    fat_set(p, ch[-1], ch2[0])
    return p


@scenario("first_cluster_out_of_range", expect="FAT_CHAIN", msg="first cluster")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg()}, cluster=512)
    with mkcard.Image(str(p)) as im:
        im.set_first_cluster("/BOOT/SURFOS.BIN", im.clusters + 100)
    return p


@scenario("first_cluster_zero_with_data", expect="FAT_CHAIN", msg="first cluster 0")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg()}, cluster=512)
    with mkcard.Image(str(p)) as im:
        im.set_first_cluster("/BOOT/SURFOS.BIN", 0)
    return p


@scenario("size_bigger_than_volume", expect="FAT_CHAIN", msg="more clusters than the volume")
def _(p):
    card(p, {"/BOOT/SURFOS.BIN": pkg()}, cluster=512)
    with mkcard.Image(str(p)) as im:
        im.set_size("/BOOT/SURFOS.BIN", 0xFFFFFF00)
    return p


@scenario("directory_longer_than_65536_entries", expect="DIR", msg="longer than")
def _(p):
    card(p, {"/BOOT/A.DAT": b"a"}, cluster=512)
    with mkcard.Image(str(p)) as im:
        boot = im.find("/BOOT")["cluster"]
        start = im.clusters - 5000                          # a run of free clusters near the end
        deleted = (b"\xe5" + b"X" * 10 + b"\x20" + bytes(20)) * 16
        prev = boot
        for k in range(4200):
            c = start + k
            im.fat_set(prev, c)
            im.write(im.cluster_lba(c), deleted)
            prev = c
        im.fat_set(prev, 0x0FFFFFFF)
        # the first cluster of /BOOT must not end the directory early
        first = bytearray(im.read(im.cluster_lba(boot)))
        for k in range(0, 512, 32):
            if first[k] == 0:
                first[k:k + 32] = deleted[:32]
        im.write(im.cluster_lba(boot), bytes(first))
    return p


# ---------------------------------------------------------------- the package

def with_package(p, data, **kw):
    return card(p, {"/BOOT/SURFOS.BIN": data}, **kw)


@scenario("package_shorter_than_header", expect="IMG_SHORT", msg="shorter than the 64-byte header")
def _(p):
    return with_package(p, pkg()[:40])


@scenario("package_is_an_elf", expect="IMG_MAGIC", msg="ELF")
def _(p):
    return with_package(p, b"\x7fELF\x01\x01\x01" + bytes(200))


@scenario("package_is_x86_boot_code", expect="IMG_MAGIC", msg="x86")
def _(p):
    return with_package(p, b"\xeb\x63\x90" + bytes(509))


@scenario("package_version_2", expect="IMG_VERSION", msg="version 2")
def _(p):
    return with_package(p, pkg(version=2))


@scenario("package_header_size_128", expect="IMG_VERSION", msg="size 128")
def _(p):
    return with_package(p, pkg(header_size=128))


@scenario("package_header_crc_wrong", expect="IMG_HCRC", msg="header CRC")
def _(p):
    return with_package(p, pkg(header_crc32=0x12345678))


@scenario("package_header_damaged_after_packing", expect="IMG_HCRC")
def _(p):
    data = bytearray(pkg())
    data[0x30] ^= 0x20                                     # one bit of the name
    return with_package(p, bytes(data))


@scenario("package_needs_unknown_isa", expect="IMG_ISA", msg="0x00000010")
def _(p):
    return with_package(p, pkg(isa=0x1F))


@scenario("package_without_base_isa", expect="IMG_ISA", msg="do not include I (SURF_ISA_I)")
def _(p):
    return with_package(p, pkg(isa=0x0E))                  # M, Zicsr and Zifencei, but not I


@scenario("package_other_platform", expect="IMG_PLATFORM", msg="platform ABI 2")
def _(p):
    return with_package(p, pkg(platform=2))


@scenario("package_truncated", expect="IMG_SIZE", msg="truncated")
def _(p):
    return with_package(p, pkg(30000)[:20000])


@scenario("package_padded", expect="IMG_SIZE", msg="extra bytes")
def _(p):
    return with_package(p, pkg(30000) + bytes(512))


@scenario("package_empty_payload", expect="IMG_SIZE", msg="payload size is 0")
def _(p):
    return with_package(p, surfimg.make_header(load_addr=DDR, payload=b"", mem_size=16, entry=DDR))


@scenario("load_address_misaligned", expect="IMG_ALIGN", msg="not 4-byte aligned")
def _(p):
    return with_package(p, pkg(load=DDR + 2, entry=DDR + 4))


@scenario("load_address_below_ddr", expect="IMG_RANGE", msg="outside usable DDR")
def _(p):
    return with_package(p, pkg(load=0x00010000))


@scenario("load_address_in_ddr_top", expect="IMG_RANGE", msg="ddr_top")
def _(p):
    return with_package(p, pkg(load=0x8F000000))


@scenario("load_range_crosses_ddr_top", expect="IMG_RANGE", msg="0x8f000000)")
def _(p):
    return with_package(p, pkg(4000, load=0x8F000000 - 4096, mem=8192))


@scenario("load_range_overflows_32_bits", expect="IMG_RANGE", msg="+4G")
def _(p):
    return with_package(p, pkg(4000, load=DDR, mem=0xFFFFFF00))


@scenario("mem_size_smaller_than_payload", expect="IMG_RANGE", msg="smaller than payload_size")
def _(p):
    return with_package(p, pkg(4000, mem=100))


@scenario("load_range_beyond_small_ddr", args=["--ddr-size", str(64 << 20)], expect="IMG_RANGE",
          msg="0x83000000)")                               # 64 MiB: ddr_top is [0x83000000, 0x84000000)
def _(p):
    return with_package(p, pkg(4000, load=0x84000000 - 2048, mem=8192))


@scenario("load_range_in_small_ddr_top", args=["--ddr-size", str(64 << 20)], expect="IMG_RANGE",
          msg="ddr_top")
def _(p):
    return with_package(p, pkg(4000, load=0x83000000, mem=8192))


@scenario("ok_small_ddr_up_to_its_top", args=["--ddr-size", str(64 << 20)], logs=["matches (card and DDR)"])
def _(p):
    return with_package(p, pkg(4000, load=0x83000000 - 8192, mem=8192))


@scenario("entry_misaligned", expect="IMG_ENTRY", msg="not 4-byte aligned")
def _(p):
    return with_package(p, pkg(entry=DDR + 6))


@scenario("entry_past_payload", expect="IMG_ENTRY", msg="outside the payload")
def _(p):
    return with_package(p, pkg(4096, entry=DDR + 4096, mem=8192))


@scenario("entry_below_load_address", expect="IMG_ENTRY", msg="outside the payload")
def _(p):
    return with_package(p, pkg(load=DDR + 4096, entry=DDR))


@scenario("payload_damaged", expect="IMG_CRC", msg="as read from the card")
def _(p):
    data = bytearray(pkg())
    data[64 + 777] ^= 0x01
    return with_package(p, bytes(data))


@scenario("ddr_loses_a_bit", args=["--ddr-flip"], expect="IMG_VERIFY", msg="read back from DDR")
def _(p):
    return std(p)


# ---------------------------------------------------------------- backup

@scenario("bad_main_good_backup", flags=1, logs=["as read from the card", "trying /BOOT/BACKUP.BIN"])
def _(p):
    bad = bytearray(pkg())
    bad[2000] ^= 0xFF
    return card(p, {"/BOOT/SURFOS.BIN": bytes(bad), "/BOOT/BACKUP.BIN": pkg(seed=2)})


@scenario("both_bad_reports_the_main_error", expect="IMG_CRC", logs=["trying /BOOT/BACKUP.BIN"])
def _(p):
    bad = bytearray(pkg())
    bad[2000] ^= 0xFF
    return card(p, {"/BOOT/SURFOS.BIN": bytes(bad), "/BOOT/BACKUP.BIN": pkg()[:30]})


@scenario("btn0_boots_backup", args=["--btn0"], flags=1, logs=["boot: /BOOT/BACKUP.BIN"],
          nologs=["boot: /BOOT/SURFOS.BIN"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/BACKUP.BIN": pkg(seed=2)})


@scenario("btn0_without_backup_stops", args=["--btn0"], expect="NOT_FOUND", msg="BACKUP.BIN does not exist",
          nologs=["boot: /BOOT/SURFOS.BIN"])
def _(p):
    return std(p)


# ---------------------------------------------------------------- DDR

@scenario("ddr_absent", args=["--ddr", "absent"], expect="DDR_ABSENT", msg="no DDR controller")
def _(p):
    return std(p)


@scenario("ddr_calibration_fails", args=["--ddr", "fail"], expect="DDR_CALIB", msg="calibration failed")
def _(p):
    return std(p)


@scenario("ddr_calibration_never_ends", args=["--ddr", "never"], expect="DDR_TIMEOUT", msg="2000 ms")
def _(p):
    return std(p)


@scenario("ddr_size_zero", args=["--ddr-size", "0"], expect="DDR_ABSENT", msg="DDR_SIZE")
def _(p):
    return std(p)


@scenario("ddr_size_not_whole_mib", args=["--ddr-size", "0x123456"], expect="DDR_ABSENT", msg="DDR_SIZE")
def _(p):
    return std(p)


# ---------------------------------------------------------------- the monitor

@scenario("monitor_commands", commands=[
    "help", "info", "sd", "part", "fs", "ls /", "ls /BOOT", "ls /USERS/NOPE", "cat /BOOT/CMDLINE.TXT",
    "chain /BOOT/SURFOS.BIN", "check /BOOT/SURFOS.BIN", "check /BOOT/BROKEN.BIN", "check /BOOT/LONGNAME1.BIN",
    "md 0x80000000 8", "mw 0x80000000 0xdeadbeef", "md 0x80000000 1", "md 0x30000000 1", "spi 3", "bogus",
    "boot"],
    logs=["boot [PATH]", "SurfBoot 1.0.0", "sd: SDHC/SDXC, sector addressing", "CID manufacturer",
          "0x0c FAT32 (LBA)", "fs: 126976 clusters", "SURFOS.BIN", "<dir>", "/USERS/NOPE does not exist",
          "hello monitor", "chain:", "fragments", "every check passed", "FAILED", "IMG_CRC",
          "is not an 8.3 name", "80000000:", "deadbeef", "access fault at 0x30000000", "DIV 3",
          "unknown command"])
def _(p):
    bad = bytearray(pkg(seed=4))
    bad[5000] ^= 1
    return card(p, {"/BOOT/SURFOS.BIN": pkg(), "/BOOT/CMDLINE.TXT": b"hello monitor\n",
                    "/BOOT/BROKEN.BIN": bytes(bad)}, cluster=512, fragment="interleave")


@scenario("monitor_spi_divider", commands=["spi", "spi 0", "sd", "spi 2", "sd", "spi auto", "sd"],
          logs=["DIV 1 = 6250 kHz, the default", "DIV 0 = 12500 kHz (applies", "12500 kHz for data (DIV 0)",
                "DIV 2 = 4166 kHz (applies", "4166 kHz for data (DIV 2)", "6250 kHz for data (DIV 1)"])
def _(p):
    return std(p)


# ---------------------------------------------------------------- running

def run(cmd, env=None, timeout=300):
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed ({r.returncode}): {r.stdout}{r.stderr}")
    return r.stdout


# card options only sdcard_sim has (the io model is run without these scenarios)
SIM_ONLY = {"--no-cd", "--dead", "--miso-low", "--acmd41-busy", "--cmd8-bad-echo", "--no-33v", "--no-cmd59",
            "--crc-every", "--read-timeouts", "--remove-after", "--min-div", "--ncr", "--nac", "--busy-bytes"}


def sim_only(sc):
    a = sc["args"]
    return any(x in SIM_ONLY for x in a) or ("--type" in a and a[a.index("--type") + 1] == "mmc")


def run_scenario(sc, fwtest, keep, extra=()):
    path = CARDS / f"{sc['name']}.img"
    if path.exists():
        path.unlink()
    t0 = time.time()
    try:
        img = sc["build"](path)
    except Exception as e:                                  # a broken test, not a SurfBoot failure
        return sc["name"], False, f"building the image failed: {e!r}", 0.0
    cmd = [fwtest, sc["mode"]] + (["--card", str(img)] if img else []) + sc["args"] + ["--quiet"] + list(extra)
    expects = sc["expect"] if isinstance(sc["expect"], tuple) else (sc["expect"],)
    if len(expects) == 1:
        cmd += ["--expect", expects[0]]
    if sc["msg"]:
        cmd += ["--expect-msg", sc["msg"]]
    for line in sc["logs"]:
        cmd += ["--expect-log", line]
    for line in sc["nologs"]:
        cmd += ["--expect-no-log", line]
    if sc["flags"] is not None:
        cmd += ["--expect-flags", str(sc["flags"])]
    if sc["cmdline"] is not None:
        cmd += ["--expect-cmdline", sc["cmdline"]]
    for c in sc["commands"] or ():
        cmd += ["-c", c]
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600, env=env)
    except subprocess.TimeoutExpired:
        return sc["name"], False, "timed out (a hang)", time.time() - t0
    out = r.stdout + r.stderr
    ok = r.returncode == 0
    if ok and len(expects) > 1:
        ok = any(f"code={e} " in out for e in expects)
    result = next((ln for ln in out.splitlines() if ln.startswith("RESULT")), "(no RESULT line)")
    detail = result if ok else f"exit {r.returncode}\n{out[-3000:]}"
    if ok and not keep and img and Path(img).exists():
        Path(img).unlink()
    return sc["name"], ok, detail, time.time() - t0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="SurfBoot's portable code against card images")
    ap.add_argument("--fwtest", default=str(ROOT / "build" / "fw" / "host" / "fwtest"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("-k", dest="filter", help="only scenarios whose name contains this")
    ap.add_argument("--keep", action="store_true", help="keep the images of passing scenarios")
    ap.add_argument("--fuzz-seeds", default="1,2,3,4,5")
    ap.add_argument("--fuzz-iterations", type=int, default=300)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--io-model", action="store_true",
                    help="run through the io track's SD card model (fwtest-io); skips sdcard_sim-only scenarios")
    a = ap.parse_args(argv)
    if a.list:
        for sc in SCENARIOS:
            print(sc["name"])
        return 0
    CARDS.mkdir(parents=True, exist_ok=True)
    chosen = [s for s in SCENARIOS if not a.filter or a.filter in s["name"]]
    skipped = []
    if a.io_model:
        skipped = [s["name"] for s in chosen if sim_only(s)]
        chosen = [s for s in chosen if not sim_only(s)]
        a.fuzz_seeds = ""                                    # the fuzzer drives an in-memory sdcard_sim card
    extra = ["--io-model"] if a.io_model else []
    t0 = time.time()
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(run_scenario, s, a.fwtest, a.keep, extra) for s in chosen]
        for f in concurrent.futures.as_completed(futs):
            name, ok, detail, secs = f.result()
            results.append((name, ok, detail, secs))
            print(f"{'PASS' if ok else 'FAIL'} {name} ({secs:.1f} s){'' if ok else ''}")
            if not ok:
                print("    " + detail.replace("\n", "\n    "))
    fails = [r for r in results if not r[1]]

    fuzz_ok = True
    if a.fuzz_seeds and not a.filter:
        fz = CARDS / "fuzz.img"
        card(fz, {"/BOOT/SURFOS.BIN": pkg(20000), "/BOOT/BACKUP.BIN": pkg(20000, seed=2),
                  "/BOOT/CMDLINE.TXT": b"fuzz\n"}, size=40 << 20, cluster=512, fragment="interleave")
        for seed in [int(s) for s in a.fuzz_seeds.split(",") if s]:
            r = subprocess.run([a.fwtest, "fuzz", "--card", str(fz), "--seed", str(seed), "--iterations",
                                str(a.fuzz_iterations), "--mutations", "6"], capture_output=True, text=True,
                               timeout=1800, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1"))
            out = (r.stdout + r.stderr).strip()
            print(out if r.returncode == 0 else f"FAIL fuzz seed {seed} (exit {r.returncode}):\n{out[-3000:]}")
            fuzz_ok &= r.returncode == 0
        if fuzz_ok:
            fz.unlink()
    if skipped:
        print(f"run_tests: {len(skipped)} scenarios need sdcard_sim's own fault options and were not run: "
              f"{' '.join(sorted(skipped))}")
    print(f"run_tests: {len(results) - len(fails)} of {len(results)} scenarios passed"
          f"{' through the io track SD card model' if a.io_model else ''}"
          f"{', fuzzing passed' if fuzz_ok and a.fuzz_seeds and not a.filter else ''} in {time.time() - t0:.0f} s")
    return 0 if not fails and fuzz_ok else 1


if __name__ == "__main__":
    sys.exit(main())
