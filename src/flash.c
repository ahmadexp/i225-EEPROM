/*
 * flash.c - i225/i226 external SPI-flash access (FLSW register interface).
 *
 * Independent implementation from recovered/ documented hardware facts:
 *
 *   CONFIRMED from the stock binary (interoperability RE):
 *     - register offsets FLSWCTL/FLSWDATA/FLSWCNT and FLA (see igc_regs.h)
 *     - FLA.FL_REQ = 0x10, FL_GNT = 0x20 (request/grant handshake)
 *     - FLSWCTL.BUSY = 0x40000000 (poll until clear)
 *     - FLSWCTL.DONE = 0x10000000 (transaction complete)
 *
 *   DATASHEET CONVENTION -- CONFIRM ON A SACRIFICIAL UNIT before trusting a
 *   destructive write (marked [VERIFY] below): the CMD opcode field encoding
 *   and the command-valid/fail bits. Read-only access degrades safely if these
 *   are off (the transaction simply times out), which is why full-image backup
 *   via flash_read() is the safe way to validate the interface first.
 */
#include "flash.h"
#include "igc_regs.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* --- FLSW register offsets (BAR0), confirmed from binary ---------------- */
#define IGC_FLSWCTL   0x12048
#define IGC_FLSWDATA  0x1204C
#define IGC_FLSWCNT   0x12050

/* --- FLSWCTL fields ----------------------------------------------------- */
#define FLSW_ADDR_MASK   0x00FFFFFF   /* [23:0] flash byte address */
#define FLSW_CMD_SHIFT   24           /* [26:24] command opcode      [VERIFY] */
#define FLSW_CMD_READ    0u           /* read                        [VERIFY] */
#define FLSW_CMD_WRITE   1u           /* page program                [VERIFY] */
#define FLSW_CMD_ERASE   2u           /* sector erase                [VERIFY] */
#define FLSW_CMDV        0x08000000   /* command valid / start       [VERIFY] */
#define FLSW_DONE        0x10000000   /* transaction complete   (confirmed)   */
#define FLSW_FLASH_FAIL  0x20000000   /* transaction failed          [VERIFY] */
#define FLSW_BUSY        0x40000000   /* cycle in progress      (confirmed)   */

/* --- FLA request/grant, confirmed from binary --------------------------- */
#define FLA_REQ  0x10
#define FLA_GNT  0x20

#define FLSW_TIMEOUT   20000   /* ~"flash cycle did not complete" budget */

/* ----- resource acquire / release --------------------------------------- */

int flash_acquire(const struct pci_dev *d)
{
    uint32_t fla = igc_rd32(d, IGC_FLA);
    igc_wr32(d, IGC_FLA, fla | FLA_REQ);

    for (int i = 0; i < FLSW_TIMEOUT; i++) {
        if (igc_rd32(d, IGC_FLA) & FLA_GNT)
            return 0;
        usleep(5);
    }
    /* back out */
    igc_wr32(d, IGC_FLA, igc_rd32(d, IGC_FLA) & ~FLA_REQ);
    return -EBUSY;
}

void flash_release(const struct pci_dev *d)
{
    igc_wr32(d, IGC_FLA, igc_rd32(d, IGC_FLA) & ~FLA_REQ);
}

size_t flash_size_bytes(const struct pci_dev *d)
{
    uint32_t eec = igc_rd32(d, IGC_EEC);
    if (!(eec & IGC_EEC_FLASH_DETECTED))
        return 0;
    /* FL_SIZE encodes log2 size; the exact base varies by part, so treat it as
     * a hint only. Callers should prefer the image length they intend to write. */
    uint32_t fl = (eec & IGC_FLA_FL_SIZE_MASK) >> IGC_FLA_FL_SIZE_SHIFT;
    return (size_t)(64 * 1024) << fl;   /* 64KB * 2^fl (hint) */
}

/* ----- one FLSW transaction --------------------------------------------- */

static int flsw_wait(const struct pci_dev *d, uint32_t *final)
{
    for (int i = 0; i < FLSW_TIMEOUT; i++) {
        uint32_t v = igc_rd32(d, IGC_FLSWCTL);
        if (!(v & FLSW_BUSY) && (v & FLSW_DONE)) {
            if (final)
                *final = v;
            return (v & FLSW_FLASH_FAIL) ? -EIO : 0;
        }
        usleep(5);
    }
    return -ETIMEDOUT;
}

/* Issue a command word to FLSWCTL: addr + opcode + count + start. */
static int flsw_cmd(const struct pci_dev *d, uint32_t cmd, uint32_t addr,
                    uint32_t nbytes)
{
    igc_wr32(d, IGC_FLSWCNT, nbytes);
    uint32_t ctl = (addr & FLSW_ADDR_MASK) |
                   (cmd << FLSW_CMD_SHIFT) |
                   FLSW_CMDV;
    igc_wr32(d, IGC_FLSWCTL, ctl);
    return flsw_wait(d, NULL);
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
        uint32_t data = igc_rd32(d, IGC_FLSWDATA);
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
    return flsw_cmd(d, FLSW_CMD_ERASE, base, 0);
}

/* ----- write ------------------------------------------------------------ */

int flash_write(const struct pci_dev *d, uint32_t addr,
                const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        /* stay within a page and a 4-byte FLSWDATA transfer */
        uint32_t page_left = FLASH_PAGE_SIZE -
                             ((addr + (uint32_t)off) & (FLASH_PAGE_SIZE - 1));
        uint32_t chunk = (len - off) >= 4 ? 4 : (uint32_t)(len - off);
        if (chunk > page_left)
            chunk = page_left;

        uint32_t data = 0;
        for (uint32_t b = 0; b < chunk; b++)
            data |= (uint32_t)buf[off + b] << (8 * b);
        igc_wr32(d, IGC_FLSWDATA, data);

        int rc = flsw_cmd(d, FLSW_CMD_WRITE, addr + (uint32_t)off, chunk);
        if (rc)
            return rc;
        off += chunk;
    }
    return 0;
}

/* ----- full image: erase + program + verify ----------------------------- */

int flash_program_image(const struct pci_dev *d, const uint8_t *img, size_t len,
                        void (*progress)(size_t, size_t))
{
    int rc;

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
