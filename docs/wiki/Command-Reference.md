# Command Reference

`i225nvm` must run as root for commands that map PCI BAR0.

## Read-Only Commands

| Command | Purpose |
| --- | --- |
| `list` | Enumerate i225/i226 controllers. |
| `dump -o FILE` | Read shadow RAM words to a file. |
| `verify -i FILE` | Compare on-device shadow RAM against an image. |
| `checksum` | Validate the on-device NVM checksum. |
| `flashdump -o FILE` | Read external SPI flash to a file. |

## Write Commands

| Command | Risk | Notes |
| --- | --- | --- |
| `checksum --write` | Medium | Recomputes and commits the shadow-NVM checksum. |
| `write -i FILE --write --fix-checksum` | Medium | Programs the word-addressable shadow-RAM/EEPROM region. |
| `flashwrite -i FILE --write --force-flash` | High | Erases and programs the raw external SPI flash. By default, patches a random locally administered MAC and saves a `patched_...mac-...bin` reference image. Can brick the NIC. |

## Global Options

| Option | Meaning |
| --- | --- |
| `-b BDF` | Target PCI address, such as `0001:01:00.0`. |
| `-i FILE` | Input image. |
| `-o FILE` | Output dump or backup path. |
| `-n WORDS` | Shadow-RAM span in 16-bit words. |
| `-s BYTES` | Explicit flash size for `flashdump`. |
| `--write` | Actually program the device. Without it, write commands dry-run. |
| `--fix-checksum` | Recompute checksum after shadow writes. |
| `--force-flash` | Required with `--write` for destructive full-flash writes. |
| `--mac MAC` | `flashwrite` only: patch the image with this nonzero unicast permanent MAC instead of a random one. |
| `--keep-image-mac` | `flashwrite` only: preserve the input image MAC bytes instead of patching a random MAC. |

## Diagnostic FLSW Access

The `flsw` command is environment-driven and intended for low-level diagnosis.
Use it only when the normal commands are insufficient.

```sh
sudo I225NVM_OP=4 I225NVM_COUNT=1 ./i225nvm flsw -b "$BDF"
```

For the tested board, this read the SPI status register and returned `0x1c`
before block-protect bits were cleared.
