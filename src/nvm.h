/*
 * nvm.h - i225/i226 Shadow-RAM (EEPROM) access primitives.
 *
 * The i225 exposes its NVM as a word-addressable "Shadow RAM" that is mirrored
 * to the backing flash. Words are read via EERD (SRRD) and written via SRWR;
 * a separate FLUPD commit copies the shadow RAM into the flash bank. All of
 * this is gated by a software/firmware semaphore so the driver, firmware and
 * this tool never touch the NVM at the same time.
 */
#ifndef NVM_H
#define NVM_H

#include <stdint.h>
#include "pci.h"

/* Semaphore: must bracket every NVM read/write session. */
int  nvm_acquire(const struct pci_dev *d);
void nvm_release(const struct pci_dev *d);

/* Word-level access (offset/count are in 16-bit words). Caller must already
 * hold the semaphore. Return 0 on success, negative errno on failure. */
int nvm_read_word(const struct pci_dev *d, uint16_t offset, uint16_t *val);
int nvm_write_word(const struct pci_dev *d, uint16_t offset, uint16_t val);
int nvm_read_block(const struct pci_dev *d, uint16_t offset,
                   uint16_t count, uint16_t *buf);
int nvm_write_block(const struct pci_dev *d, uint16_t offset,
                    uint16_t count, const uint16_t *buf);

/* Commit the shadow RAM to the backing flash (EECD.FLUPD -> poll FLUDONE). */
int nvm_flash_commit(const struct pci_dev *d);

/* Checksum helpers over words [0x00 .. NVM_CHECKSUM_REG]. */
int nvm_validate_checksum(const struct pci_dev *d, int *ok);
int nvm_update_checksum(const struct pci_dev *d);

/* Convenience: is an external flash reported present? */
int nvm_flash_present(const struct pci_dev *d);

#endif /* NVM_H */
