# Permanent MAC Address

The tested I226-V loads its permanent MAC from the first six bytes of the full
SPI flash image. These bytes also appear after boot as shadow-NVM words
`0x00..0x02`.

Writing only shadow-NVM words `0x00..0x3f` changed immediate readback but did
not persist after reboot on the tested board. Patch and reprogram the full 1 MB
image instead.

## Patch the Image

Example target MAC:

```sh
BDF=0001:01:00.0
MAC=02:a0:c9:12:34:56
SRC=firmware/FXVL_125C_V_1MB_2.32.bin
DST=firmware/FXVL_125C_V_1MB_2.32_mac-02-a0-c9-12-34-56.bin

cp "$SRC" "$DST"
```

Patch bytes `0..5` and checksum word `0x3f`:

```sh
python3 - "$DST" "$MAC" <<'PY'
import hashlib
import sys
from pathlib import Path

p = Path(sys.argv[1])
mac = bytes(int(x, 16) for x in sys.argv[2].split(":"))
if len(mac) != 6:
    raise SystemExit("MAC must have 6 octets")
if mac[0] & 1:
    raise SystemExit("MAC must be unicast; first octet must not be odd")

data = bytearray(p.read_bytes())
if len(data) != 1048576:
    raise SystemExit("expected the 1 MB I226-V image")

old = bytes(data[:6])
data[:6] = mac

words = [data[i] | (data[i + 1] << 8) for i in range(0, 0x80, 2)]
checksum = (0xBABA - sum(words[:0x3f])) & 0xffff
data[0x7e] = checksum & 0xff
data[0x7f] = checksum >> 8

p.write_bytes(data)
print("old_mac=" + ":".join(f"{b:02x}" for b in old))
print("new_mac=" + ":".join(f"{b:02x}" for b in mac))
print(f"checksum_word_0x3f=0x{checksum:04x}")
print("sha256=" + hashlib.sha256(data).hexdigest())
PY
```

For `02:a0:c9:12:34:56`, expected values:

```text
checksum_word_0x3f=0x0095
sha256=bf49d1bc57fa98ab81ad88e5aaf7224df1a59116fd3690f470d1c85912504ad2
```

## Program the Patched Image

```sh
sudo ./i225nvm flashwrite -b "$BDF" -i "$DST"

if [ -e /sys/bus/pci/devices/$BDF/driver/unbind ]; then
  printf '%s\n' "$BDF" | sudo tee /sys/bus/pci/devices/$BDF/driver/unbind
fi

sudo ./i225nvm flashwrite -b "$BDF" -i "$DST" --write --force-flash
```

## Verify Before Reboot

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/post-mac-1mb.bin
sha256sum "$DST" backups/post-mac-1mb.bin
cmp "$DST" backups/post-mac-1mb.bin
```

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

Expected Linux result:

```text
eth1 DOWN 02:a0:c9:12:34:56
```
