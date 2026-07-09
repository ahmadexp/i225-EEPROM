# Hardware and Firmware

## Tested Hardware

| Item | Value |
| --- | --- |
| Host | Raspberry Pi, ARM64 Linux |
| Target | Intel I226-V / TimeHat-style I226-V board |
| Recovery BDF | `0001:01:00.0` |
| Blank PCI ID | `8086:125f` |
| Recovered PCI ID | `8086:125c` |
| Driver after recovery | `igc` |
| Fitted flash size | 1 MB |
| Flash vendor observed | SST/Microchip-compatible SPI flash |

## Tested Image

The firmware binary is not vendored in this repository.

| Field | Value |
| --- | --- |
| Source repo | `https://github.com/ahmadexp/Intel-I226-V-NVM-Firmware` |
| Source commit | `63b84a447449af2368a18bd1cf214ccf22ffbd40` |
| File | `I226-V/2.32/FXVL_125C_V_1MB_2.32.bin` |
| Size | `1048576` bytes |
| SHA-256 | `881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024` |
| ETrack ID | `0x80000425` |
| Programmed PCI ID | `8086:125c` |

## Image Selection Notes

- Use a same-size full-flash image for raw `flashwrite` recovery.
- Do not use I225 `15F2` images for an I226 blank-NVM `8086:125f` device.
- Do not use the 2 MB I226 image on the tested 1 MB flash board.
- Treat untested boards as unverified until two matching dumps and hardware
  evidence show the fitted flash size and controller identity.

## Reporting New Boards

Open a recovery or hardware report with:

- Host model and architecture.
- Full `sudo ./i225nvm list` output.
- `lspci -nn -vv -s "$BDF"` identity lines.
- Explicit flash dump size used.
- Input image file name, byte size, and SHA-256.
- Whether two prewrite backups compared identical.
- Final PCI ID and driver state after reboot, if recovered.

