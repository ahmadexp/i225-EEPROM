# i225/i226 NVM Flashing Notes

## What Intel's x86 updater has to do

For i225/i226, the updater does not need an x86-only hardware primitive for the
normal EEPROM path. The useful hardware interface is the PCI BAR0 CSR window,
which Linux exposes through `/sys/bus/pci/devices/<bdf>/resource0` and which can
be mmaped on ARM the same way as on x86.

The portable shadow-RAM programming flow is:

1. Find the Intel PCI function (`vendor=0x8086`, i225/i226 device ID).
2. Ensure PCI memory decoding is enabled and BAR0 is programmed. On the Pi
   recovery unit, `igc` failed probe because of the bad checksum and left BAR0
   as `0x00000000`; Linux had assigned `resource0` at CPU address
   `0x1b80000000`, which corresponds to PCI bus address `0x80000000`.
3. mmap BAR0.
4. Acquire the NVM resource:
   - use `SWSM` for the software mailbox,
   - set the software side of `SW_FW_SYNC.EEP`.
5. Read/write 16-bit shadow-RAM words:
   - read: write `EERD = START | (word_offset << 2)`, poll `DONE`, data is bits
     `[31:16]`;
   - write: write `SRWR = START | (word_offset << 2) | (word << 16)`, poll
     `DONE`.
6. Recompute checksum word `0x3f` so words `0x00..0x3f` sum to `0xbaba`.
7. Commit shadow RAM to flash with `EECD.FLUPD`, then poll `EECD.FLUDONE`.
8. Release `SW_FW_SYNC.EEP` and the software mailbox.

That is the path implemented in `src/nvm.c`, and it is the one to prefer for
normal MAC/config/EEPROM repair on a Raspberry Pi.

## Raw full-flash path

The raw SPI flash path is only for whole-image work such as a 2 MB combo image.
It uses the same NVM software/firmware semaphore, then a separate flash-mode
data interface:

- `FLSWCTL` at `0x12048`, `FLSWDATA` at `0x1204c`, `FLSWCNT` at `0x12050`.
- `FLSWCTL` command opcodes in bits `[27:24]`:
  - read: `0`
  - write/page-program: `1`
  - sector erase: `2`
- `FLSWCTL` status bits:
  - `CMDV=0x10000000`
  - `FLBUSY=0x20000000`
  - `DONE=0x40000000`
  - `GLDONE=0x80000000`

Transaction shape:

1. Acquire `SWSM`/`SW_FW_SYNC.EEP`.
2. Write byte count to `FLSWCNT`.
3. Write byte address plus opcode to `FLSWCTL`.
4. Confirm hardware set `CMDV`.
5. For reads, poll `DONE`, then read `FLSWDATA`.
6. For writes, write `FLSWDATA`, then poll `DONE`.
7. For erase, poll `DONE` with `FLBUSY` clear.
8. Release `SW_FW_SYNC.EEP`.

This is implemented in `src/flash.c`, but destructive `flashwrite` should only
be used after at least two read-only `flashdump` captures compare identical.

## Bench validation checklist

1. Put the i225/i226 in the Raspberry Pi PCIe slot and boot Linux.
2. Build: `make`.
3. Enumerate: `sudo ./i225nvm list`.
4. Set the target address, for example: `BDF=0001:01:00.0`.
5. Unbind the kernel driver before NVM access:
   `printf '%s\n' "$BDF" | sudo tee /sys/bus/pci/drivers/igc/unbind`.
6. Read shadow RAM: `sudo ./i225nvm dump -b "$BDF" -o shadow.bin`.
7. Validate checksum: `sudo ./i225nvm checksum -b "$BDF"`.
8. Validate raw flash non-destructively:
   - `sudo ./i225nvm flashdump -b "$BDF" -s 2097152 -o full-a.bin`
   - `sudo ./i225nvm flashdump -b "$BDF" -s 2097152 -o full-b.bin`
   - `cmp full-a.bin full-b.bin`
9. Only after the above succeeds, use the dry-run write commands before any
   real write.

## Candidate I226-V images

The `hunghvu/Intel-I226-V-NVM-Firmware` repository carries raw I226-V
`FXVL_125C_V` images in both 1 MB and 2 MB forms. These are more relevant to
the live board than the Intel I225 package because the target enumerates as
`8086:125f`, the blank-NVM I226 ID, while the images' primary PCI ID bytes are
`8086:125c`, the programmed I226-V ID.

For recovery, prefer a same-size full-flash image and use the raw `flashwrite`
path only after two explicit-size `flashdump` reads compare equal. Do not feed
these full images to the shadow-RAM `write` command. The live board rejected a
2 MB program near the halfway point, so treat it as a 1 MB flash unless the
hardware is later inspected and proves otherwise.

Latest inspected candidates:

- `I226-V/2.32/FXVL_125C_V_1MB_2.32.bin`: 1,048,576 bytes, ETrack
  `0x80000425`, SHA-256
  `881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024`.
- `I226-V/2.32/FXVL_125C_V_2MB_2.32.bin`: 2,097,152 bytes, ETrack
  `0x80000422`, SHA-256
  `1bc7ce6aec0dfacb1bd4f156054af0018efee7fbff23687a20efe8c50b346e08`.

Live recovery result:

- The board's SPI flash is 1 MB. A 2 MB write attempt verified-failed near the
  halfway point and the 1 MB image is the correct fit.
- FLSW `Read Status` returned `0x1c`; the block-protect bits had to be cleared
  with FLSW `WREN` + one-byte `WRSR=0`.
- After clearing protection, a 4 KB header write verified successfully, then
  the full 1 MB `FXVL_125C_V_1MB_2.32.bin` write verified successfully.
- Independent post-write dump SHA-256:
  `881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024`,
  matching the input image.
- After reboot, the device enumerated as `8086:125c` (`Intel Ethernet
  Controller I226-V`) and the Linux `igc` driver bound successfully.
- Permanent MAC update result:
  - Writing shadow-NVM words `0x00..0x3f` with `write -n 64 --fix-checksum`
    changed immediate readback but reverted after reboot.
  - Patching bytes `0..5` and checksum word `0x3f` in the 1 MB full-flash image,
    then programming with `flashwrite`, persisted across reboot.
  - Test MAC `02:a0:c9:12:34:56` produced checksum word `0x0095` and image
    SHA-256 `bf49d1bc57fa98ab81ad88e5aaf7224df1a59116fd3690f470d1c85912504ad2`.
  - Post-reboot Linux reported `igc ... eth1: MAC: 02:a0:c9:12:34:56`, and a
    64-word shadow dump started with `02 a0 c9 12 34 56`.
- FLSW write quirk on the tested I226-V/SST flash:
  - A single write command with larger byte counts only programmed the low byte
    of `FLSWDATA`.
  - Correct full-image programming required one-byte FLSW write transactions
    after sector erase, skipping `0xff` bytes.
  - `FLSWCTL.GLDONE` was not reliable after byte writes; idle detection must use
    `DONE && !FLBUSY`.

## Evidence captured from Intel package and Pi

Intel package inspected:

- `nvmupdate64e`: x86-64 PIE, stripped, SHA-256
  `229d219e263b48445500471e050c50584799c602ddf67ed8a599a6efcffd2e9f`.
- `nvmupdate.cfg` contains two I225 15F2 images:
  - 2 MB image `FoxPond1_I225_15F2_2MB_1p94_800003BB.bin`, EEPID
    `800003BB`, SHA-256
    `2099e6e3475ff6fe150770b69eb66ef3dc0568711339811348325644c0a0e483`.
  - 1 MB image `Foxpond1_I225_15F2_LM_1MB_1p94_800003BC.bin`, EEPID
    `800003BC`, SHA-256
    `1170197dd1b66947f9753b1a925adc7aee793a2c2d55297c4e2182c71ed93cd0`.
- `strings` from `nvmupdate64e` show the relevant implementation names and
  diagnostics: `NalFlswEraseFlashRegion`, `_NalFlswReadFlashData`,
  `_NalFlswReadFlashImage`, `_NalFlswWriteFlashData`,
  `_NalFlswWriteFlashRegion`, `e1000_update_nvm_checksum_i225`,
  `e1000_validate_nvm_checksum_i225`, and `Updating PCI command register`.

Live Pi validation on `pi@192.168.10.61`:

- PCI device: `0001:01:00.0`, `8086:125f`, rev `04`, reported by this tool as
  `I226 (blank NVM)`.
- Linux `igc` probe reported `The NVM Checksum Is Not Valid`, then failed,
  leaving no bound driver and BAR0 config at zero.
- Restoring BAR0 to bus address `0x80000000` and enabling `COMMAND.MEM` makes
  BAR0 MMIO work.
- The tool now restores BAR0 automatically from sysfs `resource0`, clears a
  stale `SWSM.SMBI` once like Intel's i225 flow, and reads the checksum.
- Verified read-only operations:
  - `checksum`: reads device and reports `INVALID`.
  - two 64-word shadow dumps compare equal.
  - two 256-byte FLSW flash dumps compare equal.
  - the 256-byte flash prefix is `00 00 00 00 ff ff ...`, consistent with the
    blank/invalid-NVM state.

## Missing local artifact

The Intel x86 updater (`nvmupdate64e`) is not present in this workspace right
now; `.gitignore` intentionally excludes it. If you place it in the repo root,
the next useful RE pass is static string/import inspection plus tracing MMIO
offset constants (`0x12014`, `0x12018`, `0x1201c`, `0x12048`, `0x1204c`,
`0x12050`) to confirm the exact i225 dispatch path used by Intel's package.
