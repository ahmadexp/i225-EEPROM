/*
 * nvm.c - i225/i226 Shadow-RAM (EEPROM) access.
 *
 * Clean-room implementation of the documented register protocol, matching the
 * semantics of the GPL Linux igc/igb drivers (igc_i225.c, e1000_i210.c).
 */
#include "nvm.h"
#include "igc_regs.h"

#include <errno.h>
#include <unistd.h>

/* ----- low-level semaphore (SWSM SMBI + SWESMBI) ------------------------ */

static int get_hw_semaphore(const struct pci_dev *d)
{
    /* Phase 1: acquire the driver mailbox bit (SMBI). */
    int i;
    for (i = 0; i < IGC_SWSM_ATTEMPTS; i++) {
        uint32_t swsm = igc_rd32(d, IGC_SWSM);
        if (!(swsm & IGC_SWSM_SMBI))
            break;
        usleep(50);
    }
    if (i == IGC_SWSM_ATTEMPTS)
        return -EBUSY;   /* SMBI stuck: another SW instance holds it */

    /* Phase 2: acquire the SW EEPROM semaphore (SWESMBI). */
    for (i = 0; i < IGC_SWSM_ATTEMPTS; i++) {
        uint32_t swsm = igc_rd32(d, IGC_SWSM);
        igc_wr32(d, IGC_SWSM, swsm | IGC_SWSM_SWESMBI);
        swsm = igc_rd32(d, IGC_SWSM);
        if (swsm & IGC_SWSM_SWESMBI)
            return 0;
        usleep(50);
    }

    /* Back out SMBI on failure. */
    igc_wr32(d, IGC_SWSM, igc_rd32(d, IGC_SWSM) & ~IGC_SWSM_SMBI);
    return -EBUSY;
}

static void put_hw_semaphore(const struct pci_dev *d)
{
    uint32_t swsm = igc_rd32(d, IGC_SWSM);
    swsm &= ~(IGC_SWSM_SMBI | IGC_SWSM_SWESMBI);
    igc_wr32(d, IGC_SWSM, swsm);
}

int nvm_acquire(const struct pci_dev *d)
{
    int i;
    for (i = 0; i < IGC_SWSM_ATTEMPTS; i++) {
        int rc = get_hw_semaphore(d);
        if (rc)
            return rc;

        uint32_t sync = igc_rd32(d, IGC_SW_FW_SYNC);
        /* Free if neither SW nor FW currently owns the EEPROM resource. */
        if (!(sync & (IGC_SWFW_EEP_SM | (IGC_SWFW_EEP_SM << 16)))) {
            sync |= IGC_SWFW_EEP_SM;
            igc_wr32(d, IGC_SW_FW_SYNC, sync);
            put_hw_semaphore(d);
            return 0;
        }
        /* Someone else holds it: drop the mailbox and retry. */
        put_hw_semaphore(d);
        usleep(5000);
    }
    return -EBUSY;
}

void nvm_release(const struct pci_dev *d)
{
    if (get_hw_semaphore(d))
        return;
    uint32_t sync = igc_rd32(d, IGC_SW_FW_SYNC);
    sync &= ~IGC_SWFW_EEP_SM;
    igc_wr32(d, IGC_SW_FW_SYNC, sync);
    put_hw_semaphore(d);
}

/* ----- word read / write ------------------------------------------------ */

int nvm_read_word(const struct pci_dev *d, uint16_t offset, uint16_t *val)
{
    uint32_t cmd = ((uint32_t)offset << IGC_NVM_RW_ADDR_SHIFT) |
                   IGC_NVM_RW_REG_START;
    igc_wr32(d, IGC_EERD, cmd);

    for (int i = 0; i < IGC_NVM_RW_ATTEMPTS; i++) {
        uint32_t r = igc_rd32(d, IGC_EERD);
        if (r & IGC_NVM_RW_REG_DONE) {
            *val = (uint16_t)(r >> IGC_NVM_RW_REG_DATA);
            return 0;
        }
        usleep(1);
    }
    return -ETIMEDOUT;
}

int nvm_write_word(const struct pci_dev *d, uint16_t offset, uint16_t val)
{
    uint32_t cmd = ((uint32_t)offset << IGC_NVM_RW_ADDR_SHIFT) |
                   ((uint32_t)val << IGC_NVM_RW_REG_DATA) |
                   IGC_NVM_RW_REG_START;
    igc_wr32(d, IGC_SRWR, cmd);

    for (int i = 0; i < IGC_NVM_RW_ATTEMPTS; i++) {
        uint32_t r = igc_rd32(d, IGC_SRWR);
        if (r & IGC_NVM_RW_REG_DONE)
            return 0;
        usleep(1);
    }
    return -ETIMEDOUT;
}

int nvm_read_block(const struct pci_dev *d, uint16_t offset,
                   uint16_t count, uint16_t *buf)
{
    for (uint16_t i = 0; i < count; i++) {
        int rc = nvm_read_word(d, offset + i, &buf[i]);
        if (rc)
            return rc;
    }
    return 0;
}

int nvm_write_block(const struct pci_dev *d, uint16_t offset,
                    uint16_t count, const uint16_t *buf)
{
    for (uint16_t i = 0; i < count; i++) {
        int rc = nvm_write_word(d, offset + i, buf[i]);
        if (rc)
            return rc;
    }
    return 0;
}

/* ----- flash commit ----------------------------------------------------- */

static int poll_flash_done(const struct pci_dev *d)
{
    for (int i = 0; i < IGC_FLUDONE_ATTEMPTS; i++) {
        if (igc_rd32(d, IGC_EEC) & IGC_EEC_FLUDONE)
            return 0;
        usleep(5);
    }
    return -ETIMEDOUT;
}

int nvm_flash_commit(const struct pci_dev *d)
{
    /* Ensure no prior update is still pending. */
    if (poll_flash_done(d))
        return -ETIMEDOUT;

    uint32_t eec = igc_rd32(d, IGC_EEC);
    eec |= IGC_EEC_FLUPD;
    igc_wr32(d, IGC_EEC, eec);

    return poll_flash_done(d);
}

int nvm_flash_present(const struct pci_dev *d)
{
    return (igc_rd32(d, IGC_EEC) & IGC_EEC_FLASH_DETECTED) ? 1 : 0;
}

/* ----- checksum --------------------------------------------------------- */

int nvm_validate_checksum(const struct pci_dev *d, int *ok)
{
    uint16_t sum = 0, w;
    for (uint16_t i = 0; i <= NVM_CHECKSUM_REG; i++) {
        int rc = nvm_read_word(d, i, &w);
        if (rc)
            return rc;
        sum += w;
    }
    *ok = (sum == NVM_SUM);
    return 0;
}

int nvm_update_checksum(const struct pci_dev *d)
{
    uint16_t sum = 0, w;
    for (uint16_t i = 0; i < NVM_CHECKSUM_REG; i++) {
        int rc = nvm_read_word(d, i, &w);
        if (rc)
            return rc;
        sum += w;
    }
    uint16_t checksum = (uint16_t)(NVM_SUM - sum);
    int rc = nvm_write_word(d, NVM_CHECKSUM_REG, checksum);
    if (rc)
        return rc;
    return nvm_flash_commit(d);
}
