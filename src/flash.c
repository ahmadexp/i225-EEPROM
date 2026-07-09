/*
 * flash.c - i225/i226 external SPI-flash access (FLSW register interface).
 *
 * Independent implementation from public i210/i225 flash-mode implementations
 * and the hardware register interface:
 *
 *   Cross-checked facts:
 *     - register offsets FLSWCTL/FLSWDATA/FLSWCNT and FLA (see igc_regs.h)
 *     - FLSW access is protected with the NVM SW/FW semaphore
 *     - FLSWCTL.CMDV = 0x10000000, FLBUSY = 0x20000000
 *     - FLSWCTL.DONE = 0x40000000, GLDONE = 0x80000000
 *
 * Read-only access is still the safe validation step before trusting any
 * destructive erase/write operation on a specific board.
 */
#include "flash.h"
#include "igc_regs.h"
#include "nvm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- FLSW register offsets (BAR0) --------------------------------------- */
#define IGC_FLSWCTL   0x12048
#define IGC_FLSWDATA  0x1204C
#define IGC_FLSWCNT   0x12050

/* --- FLSWCTL fields ----------------------------------------------------- */
#define FLSW_ADDR_MASK   0x00FFFFFF   /* [23:0] flash byte address */
#define FLSW_CMD_SHIFT   24           /* [27:24] command opcode */
#define FLSW_CMD_READ    0u           /* read */
#define FLSW_CMD_WRITE   1u           /* page program */
#define FLSW_CMD_ERASE   2u           /* sector erase */
#define FLSW_CMD_RDSR    4u           /* read SPI status register */
#define FLSW_CMD_WRSR    5u           /* write SPI status register */
#define FLSW_CMD_WREN    6u           /* write enable */
#define FLSW_CMDV        0x10000000   /* command accepted by hardware */
#define FLSW_FLBUSY      0x20000000   /* flash erase/write busy */
#define FLSW_DONE        0x40000000   /* transaction complete */
#define FLSW_GLDONE      0x80000000   /* global flash engine idle */
#define FLSW_CMD_ADDR_MASK 0x0FFFFFFF

#define FLSW_TIMEOUT   200000  /* ~1s at 5us polls */

/* ----- resource acquire / release --------------------------------------- */

int flash_acquire(const struct pci_dev *d)
{
    return nvm_acquire(d);
}

void flash_release(const struct pci_dev *d)
{
    nvm_release(d);
}

size_t flash_size_bytes(const struct pci_dev *d)
{
    uint32_t eec = igc_rd32(d, IGC_EEC);
    if (!(eec & IGC_EEC_FLASH_DETECTED))
        return 0;
    /* FL_SIZE encodes log2 size; the exact base varies by part, so treat it as
     * a hint only. On blank/invalid NVM parts the field can read as zero even
     * when a 1 MB or 2 MB SPI flash is present, so reject implausibly small
     * hints and let callers use an explicit size or image length instead. */
    uint32_t fla = igc_rd32(d, IGC_FLA);
    uint32_t fl = (fla & IGC_FLA_FL_SIZE_MASK) >> IGC_FLA_FL_SIZE_SHIFT;
    size_t hint = (size_t)(64 * 1024) << fl;   /* 64KB * 2^fl (hint) */
    return hint >= (1024 * 1024) ? hint : 0;
}

/* ----- one FLSW transaction --------------------------------------------- */

static int flsw_dbg = -1;
static int flsw_dbg_left = 0;
static int dbg_on(void)
{
    if (flsw_dbg < 0) {
        flsw_dbg = getenv("I225NVM_DEBUG") ? 1 : 0;
        flsw_dbg_left = 24;
    }
    return flsw_dbg;
}

static int flsw_wait_done(const struct pci_dev *d, int wait_not_busy,
                          uint32_t *final)
{
    for (int i = 0; i < FLSW_TIMEOUT; i++) {
        uint32_t v = igc_rd32(d, IGC_FLSWCTL);
        if ((v & FLSW_DONE) && (!wait_not_busy || !(v & FLSW_FLBUSY))) {
            if (final)
                *final = v;
            if (dbg_on() && flsw_dbg_left > 0)
                fprintf(stderr, "  [flsw] final ctl=0x%08x CMDV=%d FLBUSY=%d DONE=%d GLDONE=%d\n",
                        v, !!(v & FLSW_CMDV), !!(v & FLSW_FLBUSY),
                        !!(v & FLSW_DONE), !!(v & FLSW_GLDONE));
            return 0;
        }
        usleep(5);
    }
    return -ETIMEDOUT;
}

static int flsw_wait_idle(const struct pci_dev *d)
{
    for (int i = 0; i < FLSW_TIMEOUT; i++) {
        uint32_t v = igc_rd32(d, IGC_FLSWCTL);
        if ((v & (FLSW_DONE | FLSW_GLDONE)) == (FLSW_DONE | FLSW_GLDONE))
            return 0;
        usleep(5);
    }
    return -ETIMEDOUT;
}

static int flsw_check_cmdv(const struct pci_dev *d)
{
    uint32_t v = igc_rd32(d, IGC_FLSWCTL);
    if (dbg_on() && flsw_dbg_left > 0)
        fprintf(stderr, "  [flsw] after command ctl=0x%08x CMDV=%d FLBUSY=%d DONE=%d GLDONE=%d\n",
                v, !!(v & FLSW_CMDV), !!(v & FLSW_FLBUSY),
                !!(v & FLSW_DONE), !!(v & FLSW_GLDONE));
    return (v & FLSW_CMDV) ? 0 : -EIO;
}

static int flsw_start_cmd(const struct pci_dev *d, uint32_t cmd, uint32_t addr,
                          uint32_t nbytes)
{
    /* Command-word composition is tunable via env for read-only encoding
     * discovery: I225NVM_RDCMD overrides the opcode value, I225NVM_CTLOR adds
     * extra bits. Defaults match the public Intel flash-mode convention. */
    const char *e_cmd = getenv("I225NVM_RDCMD");
    const char *e_or  = getenv("I225NVM_CTLOR");
    uint32_t opcode = (cmd == FLSW_CMD_READ && e_cmd) ? (uint32_t)strtoul(e_cmd, 0, 0) : cmd;
    uint32_t extra  = e_or ? (uint32_t)strtoul(e_or, 0, 0) : 0;
    uint32_t ctl = (igc_rd32(d, IGC_FLSWCTL) & ~FLSW_CMD_ADDR_MASK) |
                   (addr & FLSW_ADDR_MASK) |
                   (opcode << FLSW_CMD_SHIFT) |
                   extra;
    if (dbg_on() && flsw_dbg_left > 0) {
        fprintf(stderr, "  [flsw] write ctl=0x%08x (cmd=%u addr=0x%x n=%u) cnt=0x%08x\n",
                ctl, cmd, addr, nbytes, igc_rd32(d, IGC_FLSWCNT));
        flsw_dbg_left--;
    }
    igc_wr32(d, IGC_FLSWCTL, ctl);
    return 0;
}

/* Issue a command word to FLSWCTL: addr + opcode + count. */
static int flsw_cmd(const struct pci_dev *d, uint32_t cmd, uint32_t addr,
                    uint32_t nbytes)
{
    int rc = flsw_wait_idle(d);
    if (rc)
        return rc;
    igc_wr32(d, IGC_FLSWCNT, nbytes);
    rc = flsw_start_cmd(d, cmd, addr, nbytes);
    if (rc)
        return rc;
    return flsw_check_cmdv(d);
}

/* ----- raw single-transaction primitive (for opcode discovery/diagnostics) - */

int flash_raw_txn(const struct pci_dev *d, uint32_t op, uint32_t addr,
                  uint32_t nbytes, int has_data, uint32_t data_in,
                  uint32_t *ctl_out, uint32_t *data_out)
{
    int rc = flsw_wait_idle(d);
    if (rc)
        return rc;
    igc_wr32(d, IGC_FLSWCNT, nbytes);
    rc = flsw_start_cmd(d, op, addr, nbytes);
    if (rc)
        return rc;
    if (has_data)
        igc_wr32(d, IGC_FLSWDATA, data_in);

    rc = flsw_check_cmdv(d);
    if (rc)
        return rc;
    rc = flsw_wait_done(d, op != FLSW_CMD_READ, ctl_out);
    if (!rc && data_out)
        *data_out = igc_rd32(d, IGC_FLSWDATA);
    return rc;
}

/* ----- read ------------------------------------------------------------- */

int flash_read(const struct pci_dev *d, uint32_t addr, uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        uint32_t chunk = (len - off) >= 4 ? 4 : (uint32_t)(len - off);
        int rc = flsw_cmd(d, FLSW_CMD_READ, addr + (uint32_t)off, chunk);
        if (rc)
            return rc;
        rc = flsw_wait_done(d, 0, NULL);
        if (rc)
            return rc;
        uint32_t data = igc_rd32(d, IGC_FLSWDATA);
        if (dbg_on() && flsw_dbg_left > 0)
            fprintf(stderr, "  [flsw] read @0x%x data=0x%08x\n",
                    addr + (uint32_t)off, data);
        for (uint32_t b = 0; b < chunk; b++)
            buf[off + b] = (uint8_t)(data >> (8 * b));
        off += chunk;
    }
    return 0;
}

/* ----- erase ------------------------------------------------------------ */

int flash_erase_sector(const struct pci_dev *d, uint32_t addr)
{
    uint32_t base = addr & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);
    int rc = flsw_wait_idle(d);
    if (rc)
        return rc;
    rc = flsw_start_cmd(d, FLSW_CMD_ERASE, base, 0);
    if (rc)
        return rc;
    rc = flsw_check_cmdv(d);
    if (rc)
        return rc;
    return flsw_wait_done(d, 1, NULL);
}

/* ----- write ------------------------------------------------------------ */

int flash_write(const struct pci_dev *d, uint32_t addr,
                const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        uint32_t page_len = FLASH_PAGE_SIZE -
                            ((addr + (uint32_t)off) & (FLASH_PAGE_SIZE - 1));
        if (page_len > len - off)
            page_len = (uint32_t)(len - off);

        int rc = flsw_wait_idle(d);
        if (rc)
            return rc;
        igc_wr32(d, IGC_FLSWCNT, page_len);
        rc = flsw_start_cmd(d, FLSW_CMD_WRITE, addr + (uint32_t)off, page_len);
        if (rc)
            return rc;

        uint32_t page_off = 0;
        while (page_off < page_len) {
            uint32_t chunk = (page_len - page_off) >= 4 ? 4 : page_len - page_off;
            uint32_t data = 0;
            for (uint32_t b = 0; b < chunk; b++)
                data |= (uint32_t)buf[off + page_off + b] << (8 * b);
            igc_wr32(d, IGC_FLSWDATA, data);
            rc = flsw_check_cmdv(d);
            if (rc)
                return rc;
            rc = flsw_wait_done(d, 0, NULL);
            if (rc)
                return rc;
            page_off += chunk;
        }
        rc = flsw_wait_done(d, 1, NULL);
        if (rc)
            return rc;
        off += page_len;
    }
    return 0;
}

/* ----- protection/status ------------------------------------------------ */

static int flash_read_status(const struct pci_dev *d, uint8_t *status)
{
    int rc = flsw_cmd(d, FLSW_CMD_RDSR, 0, 1);
    if (rc)
        return rc;
    rc = flsw_wait_done(d, 0, NULL);
    if (rc)
        return rc;
    *status = (uint8_t)igc_rd32(d, IGC_FLSWDATA);
    return 0;
}

static int flash_write_status(const struct pci_dev *d, uint8_t status)
{
    int rc = flsw_cmd(d, FLSW_CMD_WREN, 0, 0);
    if (rc)
        return rc;
    rc = flsw_wait_done(d, 1, NULL);
    if (rc)
        return rc;

    rc = flsw_cmd(d, FLSW_CMD_WRSR, 0, 1);
    if (rc)
        return rc;
    igc_wr32(d, IGC_FLSWDATA, status);
    rc = flsw_wait_done(d, 1, NULL);
    if (rc)
        return rc;

    for (int i = 0; i < FLSW_TIMEOUT; i++) {
        uint8_t sr = 0;
        rc = flash_read_status(d, &sr);
        if (rc)
            return rc;
        if (!(sr & 0x01))
            return 0;
        usleep(5);
    }
    return -ETIMEDOUT;
}

static int flash_clear_block_protect(const struct pci_dev *d)
{
    uint8_t sr = 0;
    int rc = flash_read_status(d, &sr);
    if (rc)
        return rc;

    /* SST/Microchip and many JEDEC SPI-NOR parts use SR bits 2..4 as block
     * protect bits. A blank I226 recovery board was observed with SR=0x1c,
     * causing FLSW erase/program commands to complete without changing cells. */
    uint8_t cleared = sr & (uint8_t)~0x1c;
    if (cleared == sr)
        return 0;

    fprintf(stderr, "Clearing SPI flash block-protect bits: status 0x%02x -> 0x%02x\n",
            sr, cleared);
    rc = flash_write_status(d, cleared);
    if (rc)
        return rc;

    rc = flash_read_status(d, &sr);
    if (rc)
        return rc;
    return (sr & 0x1c) ? -EACCES : 0;
}

/* ----- full image: erase + program + verify ----------------------------- */

int flash_program_image(const struct pci_dev *d, const uint8_t *img, size_t len,
                        void (*progress)(size_t, size_t))
{
    int rc;

    rc = flash_clear_block_protect(d);
    if (rc)
        return rc;

    /* 1. Erase every sector the image spans. */
    for (uint32_t a = 0; a < len; a += FLASH_SECTOR_SIZE) {
        rc = flash_erase_sector(d, a);
        if (rc)
            return rc;
        if (progress)
            progress(a, len * 2);   /* first half = erase phase */
    }

    /* 2. Program. */
    rc = flash_write(d, 0, img, len);
    if (rc)
        return rc;

    /* 3. Verify by read-back. */
    uint8_t chk[256];
    for (size_t off = 0; off < len; off += sizeof(chk)) {
        size_t n = (len - off) < sizeof(chk) ? (len - off) : sizeof(chk);
        rc = flash_read(d, (uint32_t)off, chk, n);
        if (rc)
            return rc;
        if (memcmp(chk, img + off, n) != 0)
            return -EILSEQ;   /* verify mismatch */
        if (progress)
            progress(len + off, len * 2);   /* second half = verify phase */
    }
    return 0;
}
