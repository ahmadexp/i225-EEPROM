# Safety Checklist

Use this checklist before any raw SPI flash write.

## Before Connecting Hardware

- Confirm the target is an Intel i225/i226 Foxville controller.
- Confirm the current PCI ID and whether the device is blank-NVM.
- Decide whether you need the lower-risk shadow-RAM path or the destructive
  full-flash path.
- Do not use a firmware image from a different controller family.

## Before `flashwrite`

- Build the tool locally with `make clean && make`.
- Set `BDF` to the exact PCI address you intend to recover.
- Unbind `igc` if it is attached to the device.
- Take two explicit-size `flashdump` backups.
- Verify both backups have the expected byte size.
- Compare both backups with `cmp`.
- Record the SHA-256 of the input image.
- Confirm the input image byte size matches the fitted flash.
- Run `flashwrite` once without `--write --force-flash` as a dry run.
- Record the generated MAC and `patched_...mac-...bin` path, or decide on an
  explicit `--mac` value before the real write.
- Confirm any explicit `--mac` value is unique and not a standards special-use
  address.

## During Programming

- Keep power stable.
- Do not reboot or power-cycle during erase/program/verify.
- Treat any verify failure as recoverable only while the device remains powered.

## After Programming

- Dump the programmed flash with the same explicit byte size.
- Compare the dump to the printed `patched_...mac-...bin` file with `cmp`.
  Compare to the input image only if you used `--keep-image-mac`.
- Only reboot after the programmed image compares identical.
- After reboot, confirm the PCI ID, kernel driver, and MAC address.

## If Verify Fails

- Do not reboot.
- Keep the device powered.
- Take a diagnostic dump.
- Restore a known-good full image before power-cycling.
