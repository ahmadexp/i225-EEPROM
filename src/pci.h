/*
 * pci.h - PCIe device discovery + BAR0 memory-mapped register access.
 *
 * Uses the Linux sysfs PCI interface (/sys/bus/pci/devices/...), which is
 * architecture-neutral. On the Raspberry Pi 5 / CM4 the i225 shows up on the
 * PCIe root complex exactly like on x86, so mmap of resource0 gives direct
 * access to the controller's CSR block.
 */
#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stddef.h>

struct pci_dev {
    char     bdf[16];          /* "0000:01:00.0" */
    uint16_t vendor;
    uint16_t device;
    uint16_t subvendor;
    uint16_t subdevice;
    char     ifname[32];       /* bound netdev name, if any */
    char     driver[32];       /* bound kernel driver, if any */

    volatile uint8_t *bar0;    /* mmapped BAR0 base */
    size_t   bar0_len;
    int      bar0_fd;
};

/* Scan sysfs for Intel i225/i226 controllers. Fills up to `max` entries and
 * returns the count found (>=0), or a negative errno on failure. */
int pci_scan_i225(struct pci_dev *out, int max);

/* Map / unmap the controller's BAR0 register block. */
int  pci_map_bar0(struct pci_dev *d);
void pci_unmap_bar0(struct pci_dev *d);

/* 32-bit CSR access (BAR0-relative byte offset). MMIO-ordered. */
uint32_t igc_rd32(const struct pci_dev *d, uint32_t reg);
void     igc_wr32(const struct pci_dev *d, uint32_t reg, uint32_t val);

/* True if the human string looks like a supported i225/i226 device id. */
const char *igc_model_name(uint16_t device);

#endif /* PCI_H */
