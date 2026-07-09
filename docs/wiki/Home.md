# i225 NVM Flash 4 Pi Wiki

This wiki collects the tested recovery procedure for Intel i225/i226 Foxville
NVM and SPI flash work on Raspberry Pi-class Linux hosts.

## Tested Result

| Item | Result |
| --- | --- |
| Host | Raspberry Pi, ARM64 Linux |
| Target | Intel I226-V |
| Blank device ID | `8086:125f` |
| Recovered device ID | `8086:125c` |
| Driver after recovery | `igc` |
| Known-good image | `FXVL_125C_V_1MB_2.32.bin` |
| Permanent MAC test | `02:a0:c9:12:34:56` |

## Pages

- [Safety Checklist](Safety-Checklist.md)
- [Recovery Guide](Recovery-Guide.md)
- [Permanent MAC Address](Permanent-MAC-Address.md)
- [Command Reference](Command-Reference.md)
- [Hardware and Firmware](Hardware-and-Firmware.md)
- [Troubleshooting](Troubleshooting.md)
- [Publishing the Wiki](Publishing-the-Wiki.md)

## Safety Notes

- Always unbind `igc` before raw flash work.
- Always take two matching prewrite backups.
- Always dry-run `flashwrite` before `--write --force-flash`.
- Always dump and compare the programmed image before rebooting.
- Do not use the I225 `15F2` images for an I226 blank-NVM device.
- Do not use the 2 MB I226 image on the tested 1 MB flash board.

## Firmware Source

The firmware binary is not included in this repository. The tested recovery used:

- Repository: `https://github.com/ahmadexp/Intel-I226-V-NVM-Firmware`
- Commit: `63b84a447449af2368a18bd1cf214ccf22ffbd40`
- File: `I226-V/2.32/FXVL_125C_V_1MB_2.32.bin`
- SHA-256: `881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024`

## License

Copyright (c) 2026 Ahmad Byagowi. All rights reserved.

See [LICENSE](../../LICENSE).
