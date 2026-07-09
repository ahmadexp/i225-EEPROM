# Support

This repository documents a tested recovery path for Intel i225/i226 Foxville
controllers on Raspberry Pi-class Linux hosts.

## Good Issue Topics

- Build failures on Linux hosts.
- Read-only command failures such as `list`, `dump`, `checksum`, or
  `flashdump`.
- Reproducible recovery failures with matching prewrite backups.
- Documentation fixes or missing troubleshooting details.
- Reports from additional i225/i226 boards with exact hardware evidence.

## Out of Scope

- Firmware binary distribution.
- Help bypassing vendor licensing or redistribution restrictions.
- Guessing an image for unverified hardware.
- Support for unrelated NIC families.
- Recovery advice after a destructive write without a known-good backup.

For destructive `flashwrite` work, keep the device powered after a failed verify
and collect a diagnostic dump before rebooting.

