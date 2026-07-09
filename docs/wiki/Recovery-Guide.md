# Recovery Guide

This page is the short-form recovery checklist for a blank Intel I226-V that
enumerates as `8086:125f` and fails the Linux `igc` probe with an invalid NVM
checksum.

## Build

```sh
make clean
make
```

## Identify the Device

```sh
lspci -nn | grep -Ei '8086:125f|8086:125c|i225|i226|ethernet'
sudo ./i225nvm list
```

Set the target BDF:

```sh
BDF=0001:01:00.0
```

## Prepare Firmware

```sh
git clone https://github.com/ahmadexp/Intel-I226-V-NVM-Firmware.git firmware-src
cd firmware-src
git checkout 63b84a447449af2368a18bd1cf214ccf22ffbd40
cd -

mkdir -p firmware backups
cp firmware-src/I226-V/2.32/FXVL_125C_V_1MB_2.32.bin firmware/
sha256sum firmware/FXVL_125C_V_1MB_2.32.bin
```

Expected SHA-256:

```text
881434a8e54ebaf70117dd5061c3a2f04b16fe1cc3e443777337fb6774892024
```

## Back Up

Unbind `igc` if it is bound:

```sh
if [ -e /sys/bus/pci/devices/$BDF/driver/unbind ]; then
  printf '%s\n' "$BDF" | sudo tee /sys/bus/pci/devices/$BDF/driver/unbind
fi
```

Take two explicit 1 MB flash dumps and compare them:

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/prewrite-1mb-a.bin
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/prewrite-1mb-b.bin
sha256sum backups/prewrite-1mb-a.bin backups/prewrite-1mb-b.bin
cmp backups/prewrite-1mb-a.bin backups/prewrite-1mb-b.bin
```

Stop if the backups do not match.

## Program

Dry-run:

```sh
sudo ./i225nvm flashwrite -b "$BDF" -i firmware/FXVL_125C_V_1MB_2.32.bin
```

Write:

```sh
sudo ./i225nvm flashwrite -b "$BDF" \
  -i firmware/FXVL_125C_V_1MB_2.32.bin \
  --write --force-flash
```

Expected line:

```text
SUCCESS: full flash programmed and verified. Reboot to apply.
```

## Verify Before Reboot

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/postwrite-1mb.bin
sha256sum firmware/FXVL_125C_V_1MB_2.32.bin backups/postwrite-1mb.bin
cmp firmware/FXVL_125C_V_1MB_2.32.bin backups/postwrite-1mb.bin
```

## Reboot and Confirm

```sh
sudo reboot
```

After the Pi returns:

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
