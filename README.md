# i225nvm — native NVM/EEPROM tool for Intel i225/i226 from source

A clean-room, ARM-compilable reimplementation of the shadow-RAM programming
path performed by Intel's proprietary `nvmupdate64e`, intended to run natively
on a Raspberry Pi 5 / CM4 against an Intel **i225/i226 (Foxville)** NIC on the
PCIe bus.

## Why this exists

`nvmupdate64e` is a 4.5 MB, stripped, proprietary **x86-64** ELF. It cannot run
natively on the Pi's ARM CPU. Rather than decompile Intel's binary (impractical
and legally fraught — it bundles a dozen controller families), this tool
re-implements only the piece you need for the i225, from the **publicly
documented register interface** (Intel i225 datasheet/EDS) and the semantics of
the **GPL Linux `igc`/`igb` drivers**.

### How the original binary reaches the hardware (from RE)

Analysis of `nvmupdate64e` (imports + strings) shows it accesses the NIC's
control/status registers (CSRs) via, in order of relevance to ARM:

1. **BAR0 MMIO** through `/dev/mem` or sysfs `resource0` — *architecture
   neutral*, works identically on the Pi. **This is what this port uses.**
2. **PCI config space** via `/sys/bus/pci/devices/<bdf>/config` — also neutral.
3. **x86 port-I/O** (`ioperm`) + an Intel `/dev/nal` kernel shim — an x86-only
   *fallback*. Not needed on ARM as long as (1) works.

The i225 NVM itself is a word-addressable **Shadow RAM** mirrored to backing
flash: words are read via `EERD`, written via `SRWR`, arbitrated by a
software/firmware semaphore (`SWSM`/`SW_FW_SYNC`), committed to flash with
`EEC.FLUPD`, and protected by a `0xBABA` checksum at word `0x3F`. All of that is
documented and reimplemented here in `src/nvm.c`.

## Build

On the Raspberry Pi (native):

```sh
make
```

Cross-compiling from an x86 Linux host:

```sh
make CROSS=aarch64-linux-gnu-
```

## Usage

Run as **root** (BAR0 mapping + NVM access require it).

```sh
./i225nvm list                        # enumerate i225/i226 controllers
./i225nvm dump  -o backup.bin         # back up the shadow RAM
./i225nvm checksum                    # report checksum validity
./i225nvm write -i image.bin          # DRY RUN (default): shows plan + backup
./i225nvm write -i image.bin --write --fix-checksum   # program shadow RAM
./i225nvm verify -i image.bin         # compare device vs image

# Full external SPI flash (OROM / combo / whole 2 MB image):
./i225nvm flashdump  -o full.bin      # read the WHOLE flash (safe, do this first)
./i225nvm flashwrite -i image.bin     # DRY RUN: full backup + shows plan
./i225nvm flashwrite -i image.bin --write --force-flash   # DESTRUCTIVE full rewrite
```

Select a specific device with `-b 0000:01:00.0` when more than one is present.

### Before writing

The kernel `igc` driver will be bound to the NIC. Unbind it first so it doesn't
race the NVM state machine:

```sh
echo 0000:01:00.0 > /sys/bus/pci/drivers/igc/unbind
```

## Safety model

- **Dry-run by default.** `write` only plans + backs up unless you pass `--write`.
- **Mandatory backup** is taken (`backup_<bdf>_<timestamp>.bin`) before any write.
- **Verify-after**: every programmed word is read back and compared.
- **Checksum** is recomputed and committed last with `--fix-checksum`.

If verification fails, restore with:
`./i225nvm write -i backup_<...>.bin --write`

## Two programming paths

**1. Shadow-RAM (`write`)** — word-addressable EEPROM region (config words,
MAC/PBA, PHY, OROM pointers, checksum), committed to flash via `EEC.FLUPD`.
This is the driver-supported, lower-risk path and covers the common
"reprogram the device configuration / EEPID" use case.

**2. Raw external flash (`flashwrite`)** — sector-erase + page-program + verify
of the whole SPI flash through the `FLSW` register interface (`FLSWCTL`
`0x12048`, `FLSWDATA` `0x1204c`, `FLSWCNT` `0x12050`) after the `FLA`
request/grant handshake. This rewrites the entire image (OROM / combo /
everything) and is what's needed for a full 2 MB `.bin`.

### What was recovered vs. what you must confirm

The raw-flash register facts came from RE of the stock binary plus the
i210/i225 datasheet:

- **Confirmed from the binary**: the `FLSWCTL`/`FLSWDATA`/`FLSWCNT` and `FLA`
  offsets; `FLA.FL_REQ`=`0x10` / `FL_GNT`=`0x20`; `FLSWCTL.BUSY`=`0x40000000`,
  `DONE`=`0x10000000`; the "flash cycle did not complete" timeout path.
- **Datasheet convention, marked `[VERIFY]` in `src/flash.c`**: the CMD-opcode
  field values (read/write/erase), the command-valid (`CMDV`) and fail bits, and
  the sector/page geometry. **Confirm these against your i225 EDS on a
  sacrificial unit before trusting a destructive write.** Read-only access
  (`flashdump`) degrades safely if they're off (it just times out), so validate
  the interface with a full dump + a re-read-compares-equal check first.

## Safety warnings

- **You can brick the NIC** with `flashwrite`. It is double-gated (`--write`
  **and** `--force-flash`) and always takes a full-flash backup first.
- Always keep the auto-backup (`backup_<bdf>_<timestamp>.bin`).
- Register offsets for the shadow-RAM path match the public `igc` driver but
  were validated against the datasheet, not your specific silicon stepping —
  verify with `dump` + `verify` on a spare unit first.
- The firmware image referenced by `nvmupdate.cfg`
  (`FoxPond1_I225_15F2_2MB_1p94_800003BB.bin`) and the EEPROM map file are **not
  in this repo** — supply your own `.bin` to `-i`.

## Files

| File | Purpose |
|------|---------|
| `src/igc_regs.h` | i225/i226 register + bit definitions (public/datasheet) |
| `src/pci.[ch]`   | sysfs PCIe discovery + BAR0 mmap + MMIO accessors |
| `src/nvm.[ch]`   | semaphore, `EERD`/`SRWR`, checksum, `FLUPD` commit |
| `src/flash.[ch]` | raw SPI flash via `FLSW` regs: read/erase/program/verify |
| `src/image.[ch]` | `.bin` load/save/validate |
| `src/main.c`     | CLI (list/dump/verify/checksum/write/flashdump/flashwrite) |
