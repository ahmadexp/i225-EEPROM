/*
 * flash.h - i225/i226 external SPI-flash access via the FLSW register interface.
 *
 * This is the raw full-image path: it erases and reprograms the backing SPI
 * flash directly (OROM / combo image / everything), as opposed to the
 * word-level Shadow-RAM path in nvm.[ch]. It is the higher-risk path -- a bad
 * write bricks the NIC -- so it is gated behind an explicit opt-in in main.c.
 *
 * The register offsets, NVM semaphore ownership, FLSWCTL command opcodes, and
 * FLSWCTL status bits match the public i210/i225 flash-mode convention.
 * Validate with a non-destructive flashdump before attempting a raw write.
 */
#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"

/* Grab / drop the shared NVM/flash resource. */
int  flash_acquire(const struct pci_dev *d);
void flash_release(const struct pci_dev *d);

/* Raw single FLSW transaction (diagnostics / opcode discovery). Returns the
 * final FLSWCTL status in *ctl_out and FLSWDATA in *data_out. */
int flash_raw_txn(const struct pci_dev *d, uint32_t op, uint32_t addr,
                  uint32_t nbytes, int has_data, uint32_t data_in,
                  uint32_t *ctl_out, uint32_t *data_out);

/* Report detected flash size in bytes (from FLA.FL_SIZE), 0 if unknown. */
size_t flash_size_bytes(const struct pci_dev *d);

/* Read `len` bytes from flash byte-offset `addr` into `buf`.
 * Non-destructive; safe to use for full-image backup. */
int flash_read(const struct pci_dev *d, uint32_t addr, uint8_t *buf, size_t len);

/* Erase the sector containing `addr` (sector size = FLASH_SECTOR_SIZE). */
int flash_erase_sector(const struct pci_dev *d, uint32_t addr);

/* Program `len` bytes at `addr`. Caller must have erased the covered sectors.
 * Handles page-boundary chunking internally. */
int flash_write(const struct pci_dev *d, uint32_t addr,
                const uint8_t *buf, size_t len);

/* Full-image program: erase + write + read-back verify over [0, len).
 * `progress` (may be NULL) is called with (bytes_done, bytes_total). */
int flash_program_image(const struct pci_dev *d, const uint8_t *img, size_t len,
                        void (*progress)(size_t, size_t));

/* Geometry (typical for the SPI parts used on i225 boards; overridable). */
#define FLASH_SECTOR_SIZE   4096
#define FLASH_PAGE_SIZE     256

#endif /* FLASH_H */
