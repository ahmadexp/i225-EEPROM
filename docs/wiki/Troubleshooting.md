# Troubleshooting

## `igc` Reports Invalid NVM Checksum

Symptom:

```text
The NVM Checksum Is Not Valid
probe with driver igc failed with error -5
```

This is the expected blank-NVM failure. Use the full-flash recovery flow with
the 1 MB I226-V image.

## BAR0 Is Zero or MMIO Fails

Blank or invalid NVM can leave PCI `COMMAND.MEM` disabled and BAR0 programmed
as zero after a failed driver probe. The tool repairs this locally by reading
Linux sysfs `resource0`, restoring BAR0, and enabling PCI memory decoding.

## Flash Status Reads `0x1c`

The tested board's SST/Microchip flash reported status `0x1c`, meaning block
protect bits were set. Current `flashwrite` clears the common protection bits
automatically before erase/program.

Manual diagnostic:

```sh
sudo I225NVM_OP=4 I225NVM_COUNT=1 ./i225nvm flsw -b "$BDF"
```

## Full-Flash Verify Fails

Do not reboot after a failed full-flash verify. Keep the device powered, take a
diagnostic dump, and restore a known-good full image.

Useful checks:

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/failed-readback.bin
sha256sum backups/failed-readback.bin
od -Ax -tx1 -N 128 backups/failed-readback.bin
```

The tested I226-V/SST flash requires one-byte FLSW write transactions. If a
readback shows only the first byte of each 32-bit word programmed and the rest
left as `0xff`, rebuild from a version containing the byte-write fix.

## Shadow MAC Write Does Not Persist

On the tested I226-V, this flow verified immediately but reverted after reboot:

```sh
sudo ./i225nvm write -b "$BDF" -i shadow-mac.bin -n 64 --write --fix-checksum
```

Use the full-image MAC patching flow in
[Permanent MAC Address](Permanent-MAC-Address.md) instead.

## Device Still Uses the Old MAC After Reboot

Confirm the first six bytes of the full flash image:

```sh
sudo ./i225nvm flashdump -b "$BDF" -s 1048576 -o backups/current-1mb.bin
od -Ax -tx1 -N 128 backups/current-1mb.bin
```

The first six bytes should be the expected MAC in normal display order.

Then check the shadow view:

```sh
sudo ./i225nvm dump -b "$BDF" -n 64 -o shadow-after-reboot.bin
od -Ax -tx1 -N 128 shadow-after-reboot.bin
sudo ./i225nvm checksum -b "$BDF"
```
