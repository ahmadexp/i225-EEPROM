# Contributing

Thanks for helping improve `i225 NVM Flash 4 Pi`.

This project touches destructive NIC firmware paths. Contributions are welcome,
but changes should keep the tool conservative, reproducible, and explicit about
what has been tested on real hardware.

## Ground Rules

- Do not commit firmware binaries, Intel updater binaries, private board dumps,
  MAC addresses from production hardware, or other redistributable-unsafe files.
- Keep write paths dry-run by default and gated behind explicit options.
- Prefer small, reviewable changes with exact command output or hardware notes.
- Document hardware-tested behavior separately from inferred behavior.
- By submitting a contribution, you agree that it may be included under the
  repository license.

## Local Build

Native build:

```sh
make clean
make
./i225nvm --help
```

Cross-compile for a Raspberry Pi-class ARM64 host:

```sh
make clean
make CROSS=aarch64-linux-gnu-
```

## Documentation Changes

The source-controlled wiki lives in `docs/wiki/`. Update those pages when a
change affects recovery steps, hardware support, safety guidance, command
behavior, or troubleshooting.

To publish the source-controlled wiki to GitHub Wiki after review:

```sh
scripts/publish-wiki.sh
```

The repository also includes a manual GitHub Actions workflow named
`Publish Wiki` for maintainers who prefer to publish from GitHub.

## Hardware Reports

For reports that involve a real NIC, include:

- Host and architecture.
- PCI BDF, vendor ID, device ID, revision, and subsystem IDs.
- Flash size used for dumps and writes.
- Exact firmware image file name, size, and SHA-256.
- `sudo ./i225nvm list` output.
- Whether `igc` was bound or unbound.
- Confirmation that two prewrite backups compared identical.

Never attach full private NVM dumps unless a maintainer explicitly requests a
sanitized artifact.

