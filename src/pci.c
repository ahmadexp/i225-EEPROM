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
#include <limits.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define SYS_PCI "/sys/bus/pci/devices"
#define PCI_COMMAND_REG 0x04
#define PCI_BAR0_REG    0x10
#define PCI_COMMAND_MEM 0x0002

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst_len)
        return;
    size_t n = strlen(src);
    if (n >= dst_len)
        n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int make_path(char *dst, size_t dst_len, const char *dir, const char *attr)
{
    size_t dlen = strlen(dir);
    size_t alen = strlen(attr);
    if (dlen + 1 + alen + 1 > dst_len)
        return -ENAMETOOLONG;
    memcpy(dst, dir, dlen);
    dst[dlen] = '/';
    memcpy(dst + dlen + 1, attr, alen);
    dst[dlen + 1 + alen] = '\0';
    return 0;
}

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
    case I225_BLANK_NVM: return "I225 (blank NVM)";
    case I226_BLANK_NVM: return "I226 (blank NVM)";
    default:        return NULL;
    }
}

/* Read a small hex value from a sysfs attribute like ".../vendor". */
static int read_hex_attr(const char *dir, const char *attr, uint32_t *out)
{
    char path[PATH_MAX];
    int rc = make_path(path, sizeof(path), dir, attr);
    if (rc)
        return rc;
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
    char path[PATH_MAX];
    if (make_path(path, sizeof(path), dir, "net"))
        return;
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        copy_str(ifname, len, e->d_name);
        break;
    }
    closedir(d);
}

/* Resolve the bound driver name via the "driver" symlink. */
static void read_driver(const char *dir, char *drv, size_t len)
{
    char path[PATH_MAX], target[PATH_MAX];
    if (make_path(path, sizeof(path), dir, "driver"))
        return;
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0)
        return;
    target[n] = '\0';
    const char *base = strrchr(target, '/');
    copy_str(drv, len, base ? base + 1 : target);
}

static int sysfs_write_text(const char *dir, const char *attr, const char *s)
{
    char path[PATH_MAX];
    int rc = make_path(path, sizeof(path), dir, attr);
    if (rc)
        return rc;
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -errno;
    size_t len = strlen(s);
    ssize_t n = write(fd, s, len);
    int err = errno;
    close(fd);
    return n == (ssize_t)len ? 0 : -err;
}

static int config_read_at(const char *dir, off_t off, void *buf, size_t len)
{
    char path[PATH_MAX];
    int rc = make_path(path, sizeof(path), dir, "config");
    if (rc)
        return rc;
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0)
        return -errno;
    ssize_t n = pread(fd, buf, len, off);
    int err = errno;
    close(fd);
    return n == (ssize_t)len ? 0 : -err;
}

static int config_write_at(const char *dir, off_t off, const void *buf, size_t len)
{
    char path[PATH_MAX];
    int rc = make_path(path, sizeof(path), dir, "config");
    if (rc)
        return rc;
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0)
        return -errno;
    ssize_t n = pwrite(fd, buf, len, off);
    int err = errno;
    close(fd);
    return n == (ssize_t)len ? 0 : -err;
}

static int read_resource_line(const char *dir, int idx,
                              uint64_t *start, uint64_t *end, uint64_t *flags)
{
    char path[PATH_MAX];
    int path_rc = make_path(path, sizeof(path), dir, "resource");
    if (path_rc)
        return path_rc;
    FILE *f = fopen(path, "r");
    if (!f)
        return -errno;
    int rc = -EINVAL;
    for (int i = 0; i <= idx; i++) {
        unsigned long long s = 0, e = 0, fl = 0;
        if (fscanf(f, "%llx %llx %llx", &s, &e, &fl) != 3)
            break;
        if (i == idx) {
            *start = (uint64_t)s;
            *end = (uint64_t)e;
            *flags = (uint64_t)fl;
            rc = 0;
            break;
        }
    }
    fclose(f);
    return rc;
}

static int pci_prepare_mmio(struct pci_dev *dv, const char *dir)
{
    uint32_t enabled = 0;
    if (read_hex_attr(dir, "enable", &enabled) == 0 && enabled == 0) {
        int rc = sysfs_write_text(dir, "enable", "1\n");
        if (rc)
            return rc;
        dv->enabled_by_us = 1;
    }

    uint32_t bar0 = 0;
    int rc = config_read_at(dir, PCI_BAR0_REG, &bar0, sizeof(bar0));
    if (rc)
        return rc;
    if ((bar0 & ~0xFu) == 0) {
        uint64_t start = 0, end = 0, flags = 0;
        rc = read_resource_line(dir, 0, &start, &end, &flags);
        if (rc)
            return rc;
        (void)end;
        (void)flags;
        if (start == 0)
            return -ENODEV;
        uint32_t assigned = (uint32_t)(start & 0xFFFFFFF0u);
        rc = config_write_at(dir, PCI_BAR0_REG, &assigned, sizeof(assigned));
        if (rc)
            return rc;
    }

    uint16_t cmd = 0;
    rc = config_read_at(dir, PCI_COMMAND_REG, &cmd, sizeof(cmd));
    if (rc)
        return rc;
    if (!(cmd & PCI_COMMAND_MEM)) {
        cmd |= PCI_COMMAND_MEM;
        rc = config_write_at(dir, PCI_COMMAND_REG, &cmd, sizeof(cmd));
        if (rc)
            return rc;
    }
    return 0;
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

        char dir[PATH_MAX];
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
        copy_str(p->bdf, sizeof(p->bdf), e->d_name);
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
    char path[PATH_MAX];
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s", SYS_PCI, dv->bdf);
    int rc = pci_prepare_mmio(dv, dir);
    if (rc)
        return rc;

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
    if (dv->enabled_by_us) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", SYS_PCI, dv->bdf);
        sysfs_write_text(dir, "enable", "0\n");
    }
    dv->bar0 = NULL;
    dv->bar0_fd = -1;
    dv->enabled_by_us = 0;
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
