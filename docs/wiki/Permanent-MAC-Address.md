# Permanent MAC Address

The tested I226-V loads its permanent MAC from the first six bytes of the full
SPI flash image. These bytes also appear after boot as shadow-NVM words
`0x00..0x02`.

Writing only shadow-NVM words `0x00..0x3f` changed immediate readback but did
not persist after reboot on the tested board. Use `flashwrite` so the full
image is patched and programmed.

## Default Random MAC

By default, `flashwrite` picks a fresh locally administered unicast MAC,
patches image bytes `0..5`, recomputes checksum word `0x3f`, and saves the
exact image it will program as `patched_...mac-...bin`.

Generated and explicit MAC addresses are checked against all-zero, broadcast,
group/multicast, and common standards special-use prefixes before the image is
patched.

```sh
BDF=0001:01:00.0
SRC=firmware/FXVL_125C_V_1MB_2.32.bin

sudo ./i225nvm flashwrite -b "$BDF" -i "$SRC"
```

The dry-run prints the generated MAC. To use that same address on the real
write, pass it back with `--mac`:

```sh
MAC=02:a0:c9:12:34:56  # replace with the MAC printed by the dry-run

sudo ./i225nvm flashwrite -b "$BDF" -i "$SRC" \
  --mac "$MAC" \
  --write --force-flash
```

If you omit `--mac`, the write command intentionally chooses a new random MAC.

## Deterministic MAC

Use a unique nonzero unicast MAC address. The tool rejects all-zero, broadcast,
group/multicast, and common standards special-use prefixes. A `02:...` prefix
is suitable for a locally administered address; do not reuse Intel's public OUI
unless you have an assigned address.

```sh
BDF=0001:01:00.0
SRC=firmware/FXVL_125C_V_1MB_2.32.bin
MAC=02:a0:c9:12:34:56

sudo ./i225nvm flashwrite -b "$BDF" -i "$SRC" --mac "$MAC"

if [ -e /sys/bus/pci/devices/$BDF/driver/unbind ]; then
  printf '%s\n' "$BDF" | sudo tee /sys/bus/pci/devices/$BDF/driver/unbind
fi

sudo ./i225nvm flashwrite -b "$BDF" -i "$SRC" --mac "$MAC" --write --force-flash
```

Use `--keep-image-mac` only when you intentionally want to preserve the input
image's existing MAC bytes.

## Verify Before Reboot

Compare the post-write dump to the `patched_...mac-...bin` file printed by the
write command:

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/post-mac-1mb.bin
sha256sum patched_0001:01:00.0_YYYYMMDD_HHMMSS_mac-02-a0-c9-12-34-56.bin backups/post-mac-1mb.bin
cmp patched_0001:01:00.0_YYYYMMDD_HHMMSS_mac-02-a0-c9-12-34-56.bin backups/post-mac-1mb.bin
```

If you used `--keep-image-mac`, compare against the original input image
instead.

## Reboot and Confirm

```sh
sudo reboot
```

After reboot:

```sh
ip -br link
cat /sys/class/net/eth1/address
sudo ./i225nvm dump -b "$BDF" -n 64 -o shadow-after-mac.bin
sudo ./i225nvm checksum -b "$BDF"
```

Expected Linux result for the deterministic example:

```text
eth1 DOWN 02:a0:c9:12:34:56
```

## Image Layout

| NVM word | File bytes | Meaning |
| --- | --- | --- |
| `0x00` | `0..1` | MAC bytes 0 and 1 |
| `0x01` | `2..3` | MAC bytes 2 and 3 |
| `0x02` | `4..5` | MAC bytes 4 and 5 |
| `0x3f` | `0x7e..0x7f` | Checksum word; words `0x00..0x3f` must sum to `0xbaba` |
