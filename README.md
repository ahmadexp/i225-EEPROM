# i225nvm

Native Linux/ARM NVM/EEPROM recovery tool for Intel i225/i226 Foxville
controllers, built from source.

This repo was built to recover an Intel I226-V on a Raspberry Pi after the
controller enumerated as the blank-NVM PCI ID `8086:125f` and the Linux `igc`
driver refused to bind with an invalid NVM checksum. The successful recovery
programmed a 1 MB I226-V image and the controller now enumerates as
`8086:125c` with `igc` bound.

## Tested Recovery

Tested target:

| Item | Value |
| --- | --- |
| Host | Raspberry Pi, ARM64 Linux |
| PCI BDF | `0001:01:00.0` |
| Before flash | `8086:125f`, `I226 (blank NVM)` |
| After flash | `8086:125c`, `Intel Ethernet Controller I226-V` |
| Driver after reboot | `igc` |
| Flash size on this board | 1 MB |
| Programmed image | `FXVL_125C_V_1MB_2.32.bin` |

The tested board had an SST/Microchip SPI flash. Its status register initially
reported `0x1c`, meaning block-protect bits were set. Current `flashwrite`
clears the common protection bits before erase/program.

## Firmware Image

The firmware binary is not vendored in this repo. Fetch it from the exact
source commit used for the recovery:

```sh
git clone https://github.com/hunghvu/Intel-I226-V-NVM-Firmware.git firmware-src
cd firmware-src
git checkout 63b84a447449af2368a18bd1cf214ccf22ffbd40
sha256sum I226-V/2.32/FXVL_125C_V_1MB_2.32.bin
```

Expected binary details:

| Field | Value |
| --- | --- |
| Source repo | `https://github.com/hunghvu/Intel-I226-V-NVM-Firmware` |
| Source commit | `63b84a447449af2368a18bd1cf214ccf22ffbd40` |
| File | `I226-V/2.32/FXVL_125C_V_1MB_2.32.bin` |
| Size | `1048576` bytes |
| SHA-256 | `881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024` |
| ETrack ID | `0x80000425` |
| Programmed PCI ID in image | `8086:125c` |

Do not use the I225 `15F2` images for an I226 blank-NVM device. Do not use the
2 MB I226 image on the tested board; a 2 MB attempt failed near the halfway
point because the fitted flash is 1 MB.

## Build

Build natively on the Raspberry Pi:

```sh
make clean
make
```

Cross-compile from another Linux host:

```sh
make CROSS=aarch64-linux-gnu-
```

The tool must run as root because it maps PCI BAR0 and accesses NVM control
registers.

## Quick Identification

Find the controller and confirm whether it is still blank:

```sh
lspci -nn | grep -Ei '8086:125f|8086:125c|i225|i226|ethernet'
sudo ./i225nvm list
```

For the tested Pi setup:

```sh
BDF=0001:01:00.0
```

Before recovery, `igc` logged:

```text
The NVM Checksum Is Not Valid
probe with driver igc failed with error -5
```

Blank or invalid NVM can leave PCI `COMMAND.MEM` disabled and BAR0 programmed as
zero after the failed driver probe. `i225nvm` repairs that locally by restoring
BAR0 from sysfs `resource0` and enabling MMIO before mapping BAR0.

## Reproducible Flash Procedure

These commands assume the firmware repo was cloned next to this repo. Adjust
paths if your layout differs.

1. Prepare directories and copy the known-good 1 MB image:

```sh
mkdir -p firmware backups
cp ../firmware-src/I226-V/2.32/FXVL_125C_V_1MB_2.32.bin firmware/
sha256sum firmware/FXVL_125C_V_1MB_2.32.bin
```

Expected hash:

```text
881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024
```

2. If `igc` is bound, unbind it before raw NVM work:

```sh
if [ -e /sys/bus/pci/devices/$BDF/driver/unbind ]; then
  printf '%s\n' "$BDF" | sudo tee /sys/bus/pci/devices/$BDF/driver/unbind
fi
```

3. Take two explicit 1 MB prewrite backups and compare them:

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/prewrite-1mb-a.bin
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/prewrite-1mb-b.bin
sha256sum backups/prewrite-1mb-a.bin backups/prewrite-1mb-b.bin
cmp backups/prewrite-1mb-a.bin backups/prewrite-1mb-b.bin
```

Stop if the two backups differ.

4. Optional: read the SPI flash status register. On the recovery board this was
`0x1c` before clearing protection.

```sh
sudo I225NVM_OP=4 I225NVM_COUNT=1 ./i225nvm flsw -b "$BDF"
```

Current `flashwrite` clears common block-protect bits automatically. If you are
using an older build, the manual clear sequence is:

```sh
sudo I225NVM_OP=6 I225NVM_COUNT=0 ./i225nvm flsw -b "$BDF"
sudo I225NVM_OP=5 I225NVM_COUNT=1 I225NVM_DATA=0x00 ./i225nvm flsw -b "$BDF"
sudo I225NVM_OP=4 I225NVM_COUNT=1 ./i225nvm flsw -b "$BDF"
```

5. Dry-run the write. This takes another automatic backup but does not erase:

```sh
sudo ./i225nvm flashwrite -b "$BDF" \
  -i firmware/FXVL_125C_V_1MB_2.32.bin
```

6. Perform the destructive write:

```sh
sudo ./i225nvm flashwrite -b "$BDF" \
  -i firmware/FXVL_125C_V_1MB_2.32.bin \
  --write --force-flash
```

Expected success line:

```text
SUCCESS: full flash programmed and verified. Reboot to apply.
```

7. Take an independent post-write dump and compare it to the input image:

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/postwrite-1mb.bin
sha256sum backups/postwrite-1mb.bin firmware/FXVL_125C_V_1MB_2.32.bin
cmp backups/postwrite-1mb.bin firmware/FXVL_125C_V_1MB_2.32.bin
```

Both hashes should be:

```text
881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024
```

8. Reboot so PCIe/NVM auto-load uses the new flash contents:

```sh
sudo reboot
```

9. Verify after reboot:

```sh
lspci -nn -s "$BDF"
lspci -nn -vv -s "$BDF" | sed -n '1,20p'
dmesg | grep -Ei 'igc|i225|i226|125c|125f|NVM' | tail -40
ip -br link
```

Expected result:

```text
Intel Corporation Ethernet Controller I226-V [8086:125c]
Kernel driver in use: igc
```

On the recovered board, the interface appeared as `eth1` with MAC
`00:a0:c9:00:00:00`.

## Permanent MAC Address

The permanent MAC address is stored in the first three 16-bit shadow-NVM words:

| NVM word | File bytes | Meaning |
| --- | --- | --- |
| `0x00` | `0..1` | MAC bytes 0 and 1 |
| `0x01` | `2..3` | MAC bytes 2 and 3 |
| `0x02` | `4..5` | MAC bytes 4 and 5 |

The dump file is little-endian, so the first six bytes of `shadow.bin` are the
MAC address in normal display order. After changing those bytes, recompute
checksum word `0x3f` so words `0x00..0x3f` sum to `0xbaba`.

Use a unique unicast MAC address. A `02:...` prefix is suitable for a locally
administered address; do not reuse Intel's public OUI unless you have an
assigned address.

Example, setting `02:a0:c9:12:34:56`:

```sh
BDF=0001:01:00.0
MAC=02:a0:c9:12:34:56

sudo ./i225nvm dump -b "$BDF" -o shadow-before.bin
cp shadow-before.bin shadow-mac.bin

python3 - "$MAC" <<'PY'
import sys
from pathlib import Path

mac = bytes(int(x, 16) for x in sys.argv[1].split(":"))
if len(mac) != 6:
    raise SystemExit("MAC must have 6 octets")
if mac[0] & 1:
    raise SystemExit("MAC must be unicast; first octet must not be odd")

p = Path("shadow-mac.bin")
data = bytearray(p.read_bytes())
if len(data) < 0x80:
    raise SystemExit("shadow dump too small")

data[0:6] = mac

# Intel NVM checksum: words 0x00..0x3f must sum to 0xbaba.
words = [data[i] | (data[i + 1] << 8) for i in range(0, 0x80, 2)]
checksum = (0xBABA - sum(words[:0x3f])) & 0xffff
data[0x7e] = checksum & 0xff
data[0x7f] = checksum >> 8

p.write_bytes(data)
print(f"checksum word 0x3f = 0x{checksum:04x}")
PY
```

Dry-run first. Then write only the first 64 words, which include the MAC words
and checksum word:

```sh
sudo ./i225nvm write -b "$BDF" -i shadow-mac.bin -n 64

if [ -e /sys/bus/pci/devices/$BDF/driver/unbind ]; then
  printf '%s\n' "$BDF" | sudo tee /sys/bus/pci/devices/$BDF/driver/unbind
fi

sudo ./i225nvm write -b "$BDF" -i shadow-mac.bin -n 64 --write --fix-checksum
sudo ./i225nvm verify -b "$BDF" -i shadow-mac.bin -n 64
sudo ./i225nvm checksum -b "$BDF"
sudo reboot
```

After reboot, confirm the address from Linux:

```sh
ip -br link
cat /sys/class/net/eth1/address
```

## Restore

For raw-flash recovery, restore with `flashwrite`, not the shadow-RAM `write`
command:

```sh
sudo ./i225nvm flashwrite -b "$BDF" \
  -i backups/prewrite-1mb-a.bin \
  --write --force-flash
```

Keep every `backup_<bdf>_<timestamp>.bin` produced by `flashwrite`; those are
the automatic prewrite raw-flash backups.

## Command Summary

Read-only commands:

```sh
sudo ./i225nvm list
sudo ./i225nvm checksum -b "$BDF"
sudo ./i225nvm dump -b "$BDF" -o shadow.bin
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o full.bin
```

Shadow-RAM commands, for config-word repair only:

```sh
sudo ./i225nvm write -b "$BDF" -i shadow.bin
sudo ./i225nvm write -b "$BDF" -i shadow.bin --write --fix-checksum
sudo ./i225nvm verify -b "$BDF" -i shadow.bin
```

Raw full-flash commands, for whole firmware images:

```sh
sudo ./i225nvm flashwrite -b "$BDF" -i image.bin
sudo ./i225nvm flashwrite -b "$BDF" -i image.bin --write --force-flash
```

## Implementation Notes

The tool uses BAR0 MMIO through Linux sysfs resources. It does not need Intel's
x86-only `/dev/nal` path.

Relevant Foxville registers:

| Register | Offset | Purpose |
| --- | --- | --- |
| `EERD` | `0x12014` | EEPROM-mode shadow-RAM read |
| `SRWR`/`EEWR` | `0x12018` | EEPROM-mode shadow-RAM write |
| `FLA` | `0x1201c` | Flash access/status, size hint, abort clear |
| `FLSWCTL` | `0x12048` | Software flash command/address/status |
| `FLSWDATA` | `0x1204c` | Software flash data |
| `FLSWCNT` | `0x12050` | Software flash byte count |
| `FLSECU` | `0x12114` | Flash security status |

FLSW command opcodes used here:

| Opcode | Meaning |
| --- | --- |
| `0` | Read |
| `1` | Write/page program |
| `2` | 4 KB sector erase |
| `3` | Device erase |
| `4` | Read SPI status register |
| `5` | Write SPI status register |
| `6` | Write enable |
| `8` | Read JEDEC ID |

FLSW status bits:

| Bit | Mask |
| --- | --- |
| `CMDV` | `0x10000000` |
| `FLBUSY` | `0x20000000` |
| `DONE` | `0x40000000` |
| `GLDONE` | `0x80000000` |

`flashwrite` waits for idle, erases 4 KB sectors, writes in 256-byte pages, and
verifies by reading the image back.

## Files

| File | Purpose |
| --- | --- |
| `src/igc_regs.h` | i225/i226 register and bit definitions |
| `src/pci.[ch]` | sysfs PCIe discovery, BAR0 repair, mmap, MMIO accessors |
| `src/nvm.[ch]` | semaphore, `EERD`/`SRWR`, checksum, `FLUPD` commit |
| `src/flash.[ch]` | raw SPI flash via `FLSW`, unprotect, erase, program, verify |
| `src/image.[ch]` | `.bin` load/save helpers |
| `src/main.c` | CLI commands |
| `RE_NOTES.md` | reverse-engineering notes and bench log |

## References

- Firmware image source:
  `https://github.com/hunghvu/Intel-I226-V-NVM-Firmware`
- Foxville/I225 software manual mirror used for register/flow confirmation:
  `https://dokumen.pub/intel-foxville-i225-25-gbps-ethernet-controller-software-user-manual-13nbsped.html`
- Public i210 FLSW implementation used for cross-checking command flow:
  `https://lore.barebox.org/barebox/1453089161-6697-19-git-send-email-andrew.smirnov%40gmail.com/`
