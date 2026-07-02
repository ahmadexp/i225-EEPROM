/*
 * igc_regs.h - Intel i225/i226 (Foxville) CSR register + bit definitions.
 *
 * These definitions describe the documented register interface of the
 * controller (Intel i225 datasheet / EDS) and mirror the public, GPL-licensed
 * Linux kernel `igc` and `igb` drivers. They are architecture-neutral: the
 * registers live in BAR0 and are reached by memory-mapped I/O, which behaves
 * identically on x86 and on the Raspberry Pi's ARM PCIe root complex.
 *
 * This is a clean-room reference; nothing here is derived from Intel's
 * proprietary nvmupdate binary.
 */
#ifndef IGC_REGS_H
#define IGC_REGS_H

#include <stdint.h>

/* --- PCI identity ------------------------------------------------------- */
#define INTEL_VENDOR_ID          0x8086

/* i225 / i226 device IDs (Foxville). 15F2/15F3 are the common LM/V parts. */
#define I225_LM                  0x15F2
#define I225_V                   0x15F3
#define I225_I                   0x15F8
#define I225_IT                  0x0D9F
#define I225_LMVP                0x5502
#define I226_LM                  0x125B
#define I226_V                   0x125C
#define I226_IT                  0x125D

/* --- Core CSRs (BAR0 byte offsets) -------------------------------------- */
#define IGC_CTRL                 0x00000  /* Device Control */
#define IGC_STATUS               0x00008  /* Device Status */
#define IGC_CTRL_EXT             0x00018  /* Extended Device Control */
#define IGC_MDIC                 0x00020  /* MDI Control */

/* --- NVM / EEPROM / Flash control (the block that matters here) --------- */
#define IGC_EEC                  0x12010  /* EEPROM/Flash Control & Data */
#define IGC_EERD                 0x12014  /* EEPROM Read (Shadow RAM read) */
#define IGC_SRWR                 0x12018  /* Shadow RAM Write Register */
#define IGC_FLA                  0x1201C  /* Flash Access (SPI bit-bang) */
#define IGC_EEARBC_I225          0x12024  /* NVM Auto Read Bus Control */

/* --- Software / firmware semaphores ------------------------------------- */
#define IGC_SWSM                 0x05B50  /* Software Semaphore */
#define IGC_FWSM                 0x05B54  /* Firmware Semaphore */
#define IGC_SW_FW_SYNC           0x05B5C  /* Software-Firmware Synchronization */

/* --- STATUS bits -------------------------------------------------------- */
#define IGC_STATUS_LU            0x00000002  /* Link Up */

/* --- EEC (0x12010) bits ------------------------------------------------- */
#define IGC_EEC_FLASH_DETECTED   0x00080000  /* External flash present */
#define IGC_EEC_FLUPD            0x00800000  /* Update Flash (commit) */
#define IGC_EEC_FLUDONE          0x04000000  /* Flash update done */
#define IGC_EEC_SEC1VAL          0x00400000  /* Sector 1 valid (active bank) */

/* --- Shadow RAM read/write register layout (EERD / SRWR) ---------------- */
#define IGC_NVM_RW_REG_DATA      16   /* data field shift within the reg */
#define IGC_NVM_RW_REG_DONE      0x02 /* read/write done bit */
#define IGC_NVM_RW_REG_START     0x01 /* start operation bit */
#define IGC_NVM_RW_ADDR_SHIFT    2    /* word-address field shift */

/* --- SWSM (0x05B50) bits ------------------------------------------------ */
#define IGC_SWSM_SMBI            0x00000001  /* SW mailbox / driver semaphore */
#define IGC_SWSM_SWESMBI         0x00000002  /* SW EEPROM semaphore */

/* SW_FW_SYNC resource mask for the NVM/EEPROM (SW side). */
#define IGC_SWFW_EEP_SM          0x00000001

/* --- FLA (0x1201C) SPI bit-bang bits (i210/i225 external flash) --------- */
#define IGC_FLA_FL_SIZE_SHIFT    17
#define IGC_FLA_FL_SIZE_MASK     0x000E0000  /* flash size field */
#define IGC_FLA_FL_BUSY          0x40000000  /* SPI cycle in progress */
#define IGC_FLA_FL_ER            0x80000000  /* erase in progress */
#define IGC_FLA_SCK              0x00000001  /* SPI clock */
#define IGC_FLA_CS               0x00000002  /* SPI chip select (active low) */
#define IGC_FLA_SI               0x00000004  /* serial data in (host->flash) */
#define IGC_FLA_SO               0x00000008  /* serial data out (flash->host) */
#define IGC_FLA_REQ              0x00000010  /* request flash access */
#define IGC_FLA_GNT              0x00000020  /* flash access granted */
#define IGC_FLA_FL_SEC          0x00000004   /* (alias, part-dependent) */

/* --- Checksum ----------------------------------------------------------- */
#define NVM_CHECKSUM_REG         0x003F  /* word holding the checksum */
#define NVM_SUM                  0xBABA  /* words 0x00..0x3F must sum to this */

/* --- Poll limits (documented worst-case timings) ------------------------ */
#define IGC_NVM_RW_ATTEMPTS      100000  /* SRRD/SRWR done poll */
#define IGC_FLUDONE_ATTEMPTS     100000  /* flash-commit done poll */
#define IGC_SWSM_ATTEMPTS        100     /* semaphore acquire retries */

/* i225 word-sized shadow-RAM span, in 16-bit words. The mirrored EEPROM
 * region (config words, PBA, MAC, checksum, PHY, OROM pointers) is small
 * relative to the full external flash. 4 KB = 2048 words is the safe span. */
#define IGC_NVM_SHADOW_WORDS     2048

#endif /* IGC_REGS_H */
