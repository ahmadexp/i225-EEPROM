/*
 * flash.h - i225/i226 external SPI-flash access via the FLSW register interface.
 *
 * This is the raw full-image path: it erases and reprograms the backing SPI
 * flash directly (OROM / combo image / everything), as opposed to the
 * word-level Shadow-RAM path in nvm.[ch]. It is the higher-risk path -- a bad
 * write bricks the NIC -- so it is gated behind an explicit opt-in in main.c.
 *
 * The register offsets and the FLA request/grant + FLSWCTL BUSY/DONE poll bits
 * were recovered from the stock nvmupdate binary (interoperability RE of
 * hardware facts) and cross-checked against the i210/i225 datasheet. The CMD
 * opcode field values are the datasheet convention and are flagged for
 * on-hardware confirmation -- see flash.c.
 */
#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"

/* Grab / drop the shared flash resource (FLA.FL_REQ / FL_GNT). */
int  flash_acquire(const struct pci_dev *d);
void flash_release(const struct pci_dev *d);

/* Report detected flash size in bytes (from EEC.FL_SIZE), 0 if unknown. */
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
