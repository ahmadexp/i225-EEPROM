/*
 * pci.c - sysfs PCIe discovery and BAR0 MMIO for Intel i225/i226.
 */
#include "pci.h"
#include "igc_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define SYS_PCI "/sys/bus/pci/devices"

const char *igc_model_name(uint16_t device)
{
    switch (device) {
    case I225_LM:   return "I225-LM";
    case I225_V:    return "I225-V";
    case I225_I:    return "I225-I";
    case I225_IT:   return "I225-IT";
    case I225_LMVP: return "I225-LMvP";
    case I226_LM:   return "I226-LM";
    case I226_V:    return "I226-V";
    case I226_IT:   return "I226-IT";
    default:        return NULL;
    }
}

/* Read a small hex value from a sysfs attribute like ".../vendor". */
static int read_hex_attr(const char *dir, const char *attr, uint32_t *out)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    FILE *f = fopen(path, "r");
    if (!f)
        return -errno;
    unsigned v = 0;
    int n = fscanf(f, "%x", &v);
    fclose(f);
    if (n != 1)
        return -EINVAL;
    *out = v;
    return 0;
}

/* Resolve the netdev name bound to this PCI function, if the kernel driver
 * (igc) has claimed it. Best-effort; leaves ifname empty on failure. */
static void read_ifname(const char *dir, char *ifname, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/net", dir);
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(ifname, len, "%s", e->d_name);
        break;
    }
    closedir(d);
}

/* Resolve the bound driver name via the "driver" symlink. */
static void read_driver(const char *dir, char *drv, size_t len)
{
    char path[512], target[512];
    snprintf(path, sizeof(path), "%s/driver", dir);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0)
        return;
    target[n] = '\0';
    const char *base = strrchr(target, '/');
    snprintf(drv, len, "%s", base ? base + 1 : target);
}

int pci_scan_i225(struct pci_dev *out, int max)
{
    DIR *d = opendir(SYS_PCI);
    if (!d)
        return -errno;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < max) {
        if (e->d_name[0] == '.')
            continue;

        char dir[512];
        snprintf(dir, sizeof(dir), "%s/%s", SYS_PCI, e->d_name);

        uint32_t vendor = 0, device = 0, sv = 0, sd = 0;
        if (read_hex_attr(dir, "vendor", &vendor) < 0)
            continue;
        if (vendor != INTEL_VENDOR_ID)
            continue;
        if (read_hex_attr(dir, "device", &device) < 0)
            continue;
        if (!igc_model_name((uint16_t)device))
            continue;

        read_hex_attr(dir, "subsystem_vendor", &sv);
        read_hex_attr(dir, "subsystem_device", &sd);

        struct pci_dev *p = &out[count];
        memset(p, 0, sizeof(*p));
        snprintf(p->bdf, sizeof(p->bdf), "%s", e->d_name);
        p->vendor    = (uint16_t)vendor;
        p->device    = (uint16_t)device;
        p->subvendor = (uint16_t)sv;
        p->subdevice = (uint16_t)sd;
        p->bar0_fd   = -1;
        read_ifname(dir, p->ifname, sizeof(p->ifname));
        read_driver(dir, p->driver, sizeof(p->driver));
        count++;
    }
    closedir(d);
    return count;
}

int pci_map_bar0(struct pci_dev *dv)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/resource0", SYS_PCI, dv->bdf);

    struct stat st;
    if (stat(path, &st) < 0)
        return -errno;

    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0)
        return -errno;

    size_t len = (size_t)st.st_size;
    void *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        int err = -errno;
        close(fd);
        return err;
    }

    dv->bar0     = (volatile uint8_t *)base;
    dv->bar0_len = len;
    dv->bar0_fd  = fd;
    return 0;
}

void pci_unmap_bar0(struct pci_dev *dv)
{
    if (dv->bar0 && dv->bar0 != MAP_FAILED)
        munmap((void *)dv->bar0, dv->bar0_len);
    if (dv->bar0_fd >= 0)
        close(dv->bar0_fd);
    dv->bar0 = NULL;
    dv->bar0_fd = -1;
}

/*
 * MMIO accessors. The compiler barrier + volatile prevents reordering across
 * the access; the full memory barrier enforces device-memory ordering on
 * weakly-ordered ARM (resource0 is mapped as device memory by the kernel, so
 * accesses are not cached, but we still fence to keep the SPI/NVM state-machine
 * writes strictly ordered relative to the status polls that follow them).
 */
uint32_t igc_rd32(const struct pci_dev *dv, uint32_t reg)
{
    volatile uint32_t *p = (volatile uint32_t *)(dv->bar0 + reg);
    uint32_t v = *p;
    __sync_synchronize();
    return v;
}

void igc_wr32(const struct pci_dev *dv, uint32_t reg, uint32_t val)
{
    volatile uint32_t *p = (volatile uint32_t *)(dv->bar0 + reg);
    *p = val;
    __sync_synchronize();
}
