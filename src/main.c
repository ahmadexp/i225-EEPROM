/*
 * i225nvm - native ARM/Linux NVM tool for Intel i225/i226 (Foxville).
 *
 * A clean-room reimplementation of the shadow-RAM programming path that
 * Intel's proprietary nvmupdate64e performs, written to compile and run
 * natively on the Raspberry Pi (aarch64) against an i225 on the PCIe bus.
 *
 * Safety model:
 *   - dry-run by default; writing requires an explicit --write
 *   - a full backup is always taken before any write
 *   - the shadow RAM is verified word-for-word after programming
 *   - the checksum is recomputed and committed last
 */
#include "pci.h"
#include "nvm.h"
#include "flash.h"
#include "image.h"
#include "igc_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MAX_DEV 16

enum cmd { CMD_LIST, CMD_DUMP, CMD_VERIFY, CMD_CHECKSUM, CMD_WRITE,
           CMD_FLASHDUMP, CMD_FLASHWRITE, CMD_FLSW, CMD_HELP };

struct opts {
    enum cmd cmd;
    const char *bdf;     /* select device by PCI address */
    const char *image;   /* .bin for write */
    const char *outfile; /* backup / dump path */
    uint16_t words;      /* shadow span to operate on */
    uint32_t flash_len;  /* bytes for flash dump (0 = detected size) */
    int do_write;        /* actually program (not dry-run) */
    int fix_checksum;    /* recompute checksum after write */
    int force_flash;     /* arm the destructive raw-flash write */
    int use_fixed_mac;   /* flashwrite: use --mac instead of random */
    int keep_image_mac;  /* flashwrite: preserve image bytes 0..5 */
    uint8_t mac[6];      /* flashwrite: requested permanent MAC */
};

static void usage(void)
{
    printf(
"i225nvm - Intel i225/i226 NVM tool (native ARM)\n\n"
"Usage: i225nvm <command> [options]\n\n"
"Commands:\n"
"  list                      Enumerate i225/i226 controllers\n"
"  dump    -o FILE           Read the shadow RAM to FILE (backup)\n"
"  verify  -i FILE           Compare on-device shadow RAM against FILE\n"
"  checksum                  Print / fix the NVM checksum\n"
"  write   -i FILE           Program shadow RAM from FILE\n"
"  flashdump  -o FILE        Read the WHOLE external flash to FILE (safe)\n"
"  flashwrite -i FILE        Reprogram the WHOLE external flash (destructive)\n\n"
"Options:\n"
"  -b BDF        Target PCI address (e.g. 0000:01:00.0). Default: first found\n"
"  -i FILE       Input NVM image (.bin)\n"
"  -o FILE       Output file (dump/backup)\n"
"  -n WORDS      Shadow-RAM span in 16-bit words (default %d)\n"
"  -s BYTES      Flash size for flashdump (default: auto-detected)\n"
"  --write       Actually program the device (default is a dry run)\n"
"  --fix-checksum   Recompute + commit checksum after write\n"
"  --force-flash    Arm the destructive full-flash write (required)\n"
"  --mac MAC         flashwrite: patch image with this unicast MAC\n"
"  --keep-image-mac flashwrite: preserve input image MAC; default is random\n"
"  -h, --help    This help\n\n"
"Two write paths:\n"
"  * write      -> word-addressable Shadow-RAM / EEPROM region (lower risk)\n"
"  * flashwrite -> raw full external SPI flash: OROM/combo/everything. This\n"
"                  CAN BRICK the NIC. Requires --write AND --force-flash, and\n"
"                  by default patches a random locally administered MAC into\n"
"                  bytes 0..5 and fixes checksum word 0x3f. Validate with\n"
"                  repeatable flashdump reads first (see README).\n",
        IGC_NVM_SHADOW_WORDS);
}

static int pick_device(struct pci_dev *devs, int n, const char *bdf)
{
    if (n <= 0)
        return -1;
    if (!bdf)
        return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(devs[i].bdf, bdf) == 0)
            return i;
    return -1;
}

static void print_dev(const struct pci_dev *d)
{
    printf("  %s  %04x:%04x  %-9s  sub=%04x:%04x  driver=%-6s  if=%s\n",
           d->bdf, d->vendor, d->device,
           igc_model_name(d->device) ? igc_model_name(d->device) : "?",
           d->subvendor, d->subdevice,
           d->driver[0] ? d->driver : "-",
           d->ifname[0] ? d->ifname : "-");
}

static char *timestamped(char *buf, size_t n, const char *bdf)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "backup_%s_%04d%02d%02d_%02d%02d%02d.bin",
             bdf, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

static char *timestamped_patched(char *buf, size_t n, const char *bdf,
                                 const uint8_t mac[6])
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n,
             "patched_%s_%04d%02d%02d_%02d%02d%02d_mac-%02x-%02x-%02x-%02x-%02x-%02x.bin",
             bdf, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

static void mac_format(const uint8_t mac[6], char *buf, size_t n)
{
    snprintf(buf, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int mac_is_all_zero(const uint8_t mac[6])
{
    return mac[0] == 0 && mac[1] == 0 && mac[2] == 0 &&
           mac[3] == 0 && mac[4] == 0 && mac[5] == 0;
}

static int parse_mac(const char *s, uint8_t mac[6])
{
    unsigned int b[6];
    char extra;
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x%c",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &extra);
    if (n != 6)
        return -EINVAL;

    for (size_t i = 0; i < 6; i++) {
        if (b[i] > 0xff)
            return -EINVAL;
        mac[i] = (uint8_t)b[i];
    }

    if ((mac[0] & 0x01) || mac_is_all_zero(mac))
        return -EINVAL;
    return 0;
}

static int random_mac(uint8_t mac[6])
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -errno;
    size_t n = fread(mac, 1, 6, f);
    int err = ferror(f) ? errno : 0;
    fclose(f);
    if (n != 6)
        return err ? -err : -EIO;

    mac[0] = (uint8_t)((mac[0] & 0xfc) | 0x02);
    return 0;
}

/* Read the shadow span into a freshly allocated buffer. */
static int read_shadow(const struct pci_dev *d, uint16_t words, uint16_t **out)
{
    uint16_t *buf = malloc(words * sizeof(uint16_t));
    if (!buf)
        return -ENOMEM;
    int rc = nvm_acquire(d);
    if (rc) { free(buf); return rc; }
    rc = nvm_read_block(d, 0, words, buf);
    nvm_release(d);
    if (rc) { free(buf); return rc; }
    *out = buf;
    return 0;
}

static int do_dump(const struct pci_dev *d, const struct opts *o)
{
    uint16_t *buf = NULL;
    int rc = read_shadow(d, o->words, &buf);
    if (rc) {
        fprintf(stderr, "read failed: %s\n", strerror(-rc));
        return 1;
    }
    char autoname[128];
    const char *out = o->outfile ? o->outfile
                                 : timestamped(autoname, sizeof(autoname), d->bdf);
    rc = image_save(out, buf, o->words);
    free(buf);
    if (rc) {
        fprintf(stderr, "write %s failed: %s\n", out, strerror(-rc));
        return 1;
    }
    printf("Dumped %u words (%u bytes) to %s\n",
           o->words, o->words * 2, out);
    return 0;
}

static int do_verify(const struct pci_dev *d, const struct opts *o)
{
    if (!o->image) { fprintf(stderr, "verify needs -i FILE\n"); return 2; }
    struct nvm_image img;
    int rc = image_load(o->image, &img);
    if (rc) { fprintf(stderr, "load %s: %s\n", o->image, strerror(-rc)); return 1; }

    uint16_t span = o->words;
    if (img.nwords < span) span = (uint16_t)img.nwords;

    uint16_t *dev = NULL;
    rc = read_shadow(d, span, &dev);
    if (rc) { image_free(&img); fprintf(stderr, "read: %s\n", strerror(-rc)); return 1; }

    int mism = 0;
    for (uint16_t i = 0; i < span; i++) {
        if (dev[i] != img.words[i]) {
            if (mism < 16)
                printf("  word 0x%04x: device=%04x image=%04x\n",
                       i, dev[i], img.words[i]);
            mism++;
        }
    }
    free(dev);
    image_free(&img);
    if (mism)
        printf("MISMATCH: %d of %u words differ\n", mism, span);
    else
        printf("MATCH: %u words identical\n", span);
    return mism ? 1 : 0;
}

static int do_checksum(const struct pci_dev *d, const struct opts *o)
{
    int rc = nvm_acquire(d);
    if (rc) { fprintf(stderr, "acquire: %s\n", strerror(-rc)); return 1; }

    int ok = 0;
    rc = nvm_validate_checksum(d, &ok);
    if (rc) { nvm_release(d); fprintf(stderr, "read: %s\n", strerror(-rc)); return 1; }
    printf("On-device checksum: %s\n", ok ? "VALID" : "INVALID");

    if (!ok && o->do_write) {
        printf("Recomputing and committing checksum...\n");
        rc = nvm_update_checksum(d);
        if (rc) { nvm_release(d); fprintf(stderr, "update: %s\n", strerror(-rc)); return 1; }
        rc = nvm_validate_checksum(d, &ok);
        printf("After fix: %s\n", (rc == 0 && ok) ? "VALID" : "STILL BAD");
    } else if (!ok) {
        printf("(dry run: pass --write to commit a corrected checksum)\n");
    }
    nvm_release(d);
    return 0;
}

static int do_write(const struct pci_dev *d, const struct opts *o)
{
    if (!o->image) { fprintf(stderr, "write needs -i FILE\n"); return 2; }

    struct nvm_image img;
    int rc = image_load(o->image, &img);
    if (rc) { fprintf(stderr, "load %s: %s\n", o->image, strerror(-rc)); return 1; }

    printf("Image: %s  (%zu words, %zu bytes)\n",
           o->image, img.nwords, img.nbytes);
    printf("Image internal checksum: %s\n",
           image_checksum_ok(&img) ? "consistent" : "INCONSISTENT (will fix on write)");

    uint16_t span = o->words;
    if (img.nwords < span) span = (uint16_t)img.nwords;

    /* Always back up first. */
    uint16_t *backup = NULL;
    rc = read_shadow(d, o->words, &backup);
    if (rc) { image_free(&img); fprintf(stderr, "backup read: %s\n", strerror(-rc)); return 1; }
    char bname[128];
    timestamped(bname, sizeof(bname), d->bdf);
    if (image_save(bname, backup, o->words) == 0)
        printf("Backup saved: %s\n", bname);
    free(backup);

    if (!o->do_write) {
        printf("\nDRY RUN: would program %u words to %s.\n", span, d->bdf);
        printf("Re-run with --write to actually program.\n");
        image_free(&img);
        return 0;
    }

    if (d->driver[0] && strcmp(d->driver, "igc") == 0)
        fprintf(stderr,
            "WARNING: kernel driver 'igc' is bound to %s (%s). Unbind it first:\n"
            "  echo %s > /sys/bus/pci/drivers/igc/unbind\n",
            d->bdf, d->ifname, d->bdf);

    printf("Programming %u words...\n", span);
    rc = nvm_acquire(d);
    if (rc) { image_free(&img); fprintf(stderr, "acquire: %s\n", strerror(-rc)); return 1; }

    rc = nvm_write_block(d, 0, span, img.words);
    if (rc) { nvm_release(d); image_free(&img);
              fprintf(stderr, "write: %s\n", strerror(-rc)); return 1; }

    rc = nvm_flash_commit(d);
    if (rc) { nvm_release(d); image_free(&img);
              fprintf(stderr, "flash commit: %s\n", strerror(-rc)); return 1; }

    if (o->fix_checksum) {
        printf("Updating checksum...\n");
        rc = nvm_update_checksum(d);
        if (rc) fprintf(stderr, "checksum update: %s\n", strerror(-rc));
    }

    /* Verify. */
    uint16_t *rb = malloc(span * sizeof(uint16_t));
    int mism = 0;
    if (rb && nvm_read_block(d, 0, span, rb) == 0) {
        for (uint16_t i = 0; i < span; i++)
            if (rb[i] != img.words[i]) mism++;
    }
    free(rb);
    nvm_release(d);
    image_free(&img);

    if (mism) {
        fprintf(stderr, "VERIFY FAILED: %d words differ. Restore from %s\n",
                mism, bname);
        return 1;
    }
    printf("SUCCESS: %u words programmed and verified. Reboot to apply.\n", span);
    return 0;
}

static void flash_progress(size_t done, size_t total)
{
    static int last = -1;
    int pct = total ? (int)((done * 100) / total) : 0;
    if (pct != last) {
        printf("\r  progress: %3d%%", pct);
        fflush(stdout);
        last = pct;
    }
    if (done >= total)
        printf("\n");
}

static int do_flashdump(const struct pci_dev *d, const struct opts *o)
{
    size_t len = o->flash_len ? o->flash_len : flash_size_bytes(d);
    if (!len) {
        fprintf(stderr, "could not detect flash size; pass -s BYTES\n");
        return 1;
    }
    uint8_t *buf = malloc(len);
    if (!buf) { fprintf(stderr, "oom\n"); return 1; }

    int rc = flash_acquire(d);
    if (rc) { free(buf); fprintf(stderr, "flash acquire: %s\n", strerror(-rc)); return 1; }
    printf("Reading %zu bytes of flash...\n", len);
    rc = flash_read(d, 0, buf, len);
    flash_release(d);
    if (rc) { free(buf); fprintf(stderr, "flash read: %s\n", strerror(-rc)); return 1; }

    char autoname[128];
    const char *out = o->outfile ? o->outfile
                    : timestamped(autoname, sizeof(autoname), d->bdf);
    FILE *f = fopen(out, "wb");
    if (!f) { free(buf); fprintf(stderr, "open %s: %s\n", out, strerror(errno)); return 1; }
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    if (w != len) { fprintf(stderr, "short write to %s\n", out); return 1; }
    printf("Full flash image (%zu bytes) saved to %s\n", len, out);
    return 0;
}

static int do_flashwrite(const struct pci_dev *d, const struct opts *o)
{
    if (!o->image) { fprintf(stderr, "flashwrite needs -i FILE\n"); return 2; }

    struct nvm_image img;
    int rc = image_load(o->image, &img);
    if (rc) { fprintf(stderr, "load %s: %s\n", o->image, strerror(-rc)); return 1; }
    size_t len = img.nbytes;

    printf("Full-flash image: %s (%zu bytes)\n", o->image, len);

    uint8_t old_mac[6];
    rc = image_get_mac(&img, old_mac);
    if (rc) {
        image_free(&img);
        fprintf(stderr, "image is too small to contain a permanent MAC\n");
        return 1;
    }
    char old_mac_s[18];
    mac_format(old_mac, old_mac_s, sizeof(old_mac_s));

    char patched_name[160];
    const char *patched_path = NULL;
    char new_mac_s[18] = "";
    int mac_was_random = 0;

    if (o->keep_image_mac) {
        printf("Image MAC preserved: %s\n", old_mac_s);
    } else {
        uint8_t new_mac[6];
        uint16_t checksum;

        if (o->use_fixed_mac) {
            memcpy(new_mac, o->mac, sizeof(new_mac));
        } else {
            rc = random_mac(new_mac);
            if (rc) {
                image_free(&img);
                fprintf(stderr, "random MAC generation failed: %s\n", strerror(-rc));
                return 1;
            }
            mac_was_random = 1;
        }

        mac_format(new_mac, new_mac_s, sizeof(new_mac_s));
        rc = image_patch_mac(&img, new_mac, &checksum);
        if (rc) {
            image_free(&img);
            fprintf(stderr, "image is too small to patch checksum word 0x%02x\n",
                    NVM_CHECKSUM_REG);
            return 1;
        }

        printf("Image MAC patched: %s -> %s (%s)\n",
               old_mac_s, new_mac_s,
               o->use_fixed_mac ? "requested" : "random locally administered");
        printf("Updated image checksum word 0x%02x: 0x%04x\n",
               NVM_CHECKSUM_REG, checksum);

        timestamped_patched(patched_name, sizeof(patched_name), d->bdf, new_mac);
        rc = image_save(patched_name, img.words, img.nwords);
        if (rc) {
            image_free(&img);
            fprintf(stderr, "save %s: %s\n", patched_name, strerror(-rc));
            return 1;
        }
        patched_path = patched_name;
        printf("Patched flash image saved: %s\n", patched_path);
    }

    /* image_load stores words; rebuild a byte view for the raw flash. */
    uint8_t *bytes = malloc(len);
    if (!bytes) { image_free(&img); fprintf(stderr, "oom\n"); return 1; }
    for (size_t i = 0; i < img.nwords; i++) {
        bytes[2 * i]     = (uint8_t)(img.words[i] & 0xFF);
        bytes[2 * i + 1] = (uint8_t)(img.words[i] >> 8);
    }

    /* Always take a full backup first. */
    size_t fsz = flash_size_bytes(d);
    if (fsz)
        printf("Detected flash-size hint: %zu bytes\n", fsz);
    else
        printf("Flash-size hint unavailable; using image length for backup.\n");
    size_t blen = fsz > len ? fsz : len;
    if (fsz && fsz < len)
        printf("Image is larger than detected hint; backing up %zu bytes anyway.\n", blen);
    uint8_t *backup = malloc(blen);
    if (backup && flash_acquire(d) == 0) {
        if (flash_read(d, 0, backup, blen) == 0) {
            char bname[128];
            timestamped(bname, sizeof(bname), d->bdf);
            FILE *bf = fopen(bname, "wb");
            if (bf) { fwrite(backup, 1, blen, bf); fclose(bf);
                      printf("Full-flash backup saved: %s (%zu bytes)\n", bname, blen); }
        }
        flash_release(d);
    }
    free(backup);

    if (!(o->do_write && o->force_flash)) {
        printf("\nDRY RUN: would erase+program+verify %zu bytes of raw flash on %s.\n",
               len, d->bdf);
        printf("This is DESTRUCTIVE and can brick the NIC.\n");
        if (patched_path)
            printf("Compare a later post-write flashdump against %s.\n", patched_path);
        else if (o->keep_image_mac)
            printf("Compare a later post-write flashdump against %s.\n", o->image);
        if (mac_was_random)
            printf("Use --mac %s to reuse this dry-run MAC on the real write.\n",
                   new_mac_s);
        printf("Re-run with BOTH --write and --force-flash to proceed,\n");
        printf("and confirm the FLSW CMD constants in flash.c against your datasheet.\n");
        free(bytes); image_free(&img);
        return 0;
    }

    if (d->driver[0])
        fprintf(stderr, "WARNING: driver '%s' still bound to %s -- unbind it first.\n",
                d->driver, d->bdf);

    printf("Programming raw flash (%zu bytes)...\n", len);
    rc = flash_acquire(d);
    if (rc) { free(bytes); image_free(&img); fprintf(stderr, "acquire: %s\n", strerror(-rc)); return 1; }
    rc = flash_program_image(d, bytes, len, flash_progress);
    flash_release(d);
    free(bytes); image_free(&img);

    if (rc == -EILSEQ) { fprintf(stderr, "VERIFY FAILED -- restore from backup!\n"); return 1; }
    if (rc) { fprintf(stderr, "flash program: %s -- restore from backup!\n", strerror(-rc)); return 1; }
    printf("SUCCESS: full flash programmed and verified. Reboot to apply.\n");
    if (patched_path)
        printf("Compare a post-write flashdump against %s before rebooting.\n",
               patched_path);
    else if (o->keep_image_mac)
        printf("Compare a post-write flashdump against %s before rebooting.\n",
               o->image);
    return 0;
}

static int do_flsw(const struct pci_dev *d)
{
    /* env-driven: I225NVM_OP (opcode), I225NVM_ADDR, optional I225NVM_DATA */
    const char *e_op = getenv("I225NVM_OP");
    const char *e_ad = getenv("I225NVM_ADDR");
    const char *e_da = getenv("I225NVM_DATA");
    const char *e_ct = getenv("I225NVM_COUNT");
    if (!e_op) { fprintf(stderr, "set I225NVM_OP=<n> [I225NVM_ADDR=..] [I225NVM_DATA=..]\n"); return 2; }
    uint32_t op   = (uint32_t)strtoul(e_op, 0, 0);
    uint32_t addr = e_ad ? (uint32_t)strtoul(e_ad, 0, 0) : 0;
    uint32_t cnt  = e_ct ? (uint32_t)strtoul(e_ct, 0, 0) : 4;
    int has_data  = e_da ? 1 : 0;
    uint32_t din  = e_da ? (uint32_t)strtoul(e_da, 0, 0) : 0;

    const char *e_rep = getenv("I225NVM_REPEAT");
    int reps = e_rep ? atoi(e_rep) : 1;
    if (reps < 1) reps = 1;

    int rc = flash_acquire(d);
    if (rc) { fprintf(stderr, "flash acquire: %s\n", strerror(-rc)); return 1; }
    for (int i = 0; i < reps; i++) {
        uint32_t ctl = 0, dout = 0;
        rc = flash_raw_txn(d, op, addr, cnt, has_data, din, &ctl, &dout);
        if (rc) { flash_release(d); fprintf(stderr, "txn: %s\n", strerror(-rc)); return 1; }
        printf("[%d] op=%u addr=0x%06x -> ctl=0x%08x FLSWDATA=0x%08x\n",
               i, op, addr, ctl, dout);
    }
    flash_release(d);
    return 0;
}

static enum cmd parse_cmd(const char *s)
{
    if (!strcmp(s, "flsw"))       return CMD_FLSW;
    if (!strcmp(s, "list"))       return CMD_LIST;
    if (!strcmp(s, "dump"))       return CMD_DUMP;
    if (!strcmp(s, "verify"))     return CMD_VERIFY;
    if (!strcmp(s, "checksum"))   return CMD_CHECKSUM;
    if (!strcmp(s, "write"))      return CMD_WRITE;
    if (!strcmp(s, "flashdump"))  return CMD_FLASHDUMP;
    if (!strcmp(s, "flashwrite")) return CMD_FLASHWRITE;
    return CMD_HELP;
}

int main(int argc, char **argv)
{
    struct opts o = { .cmd = CMD_HELP, .words = IGC_NVM_SHADOW_WORDS };

    if (argc < 2) { usage(); return 2; }
    o.cmd = parse_cmd(argv[1]);
    if (o.cmd == CMD_HELP) { usage(); return 0; }

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-b") && i + 1 < argc)      o.bdf = argv[++i];
        else if (!strcmp(a, "-i") && i + 1 < argc) o.image = argv[++i];
        else if (!strcmp(a, "-o") && i + 1 < argc) o.outfile = argv[++i];
        else if (!strcmp(a, "-n") && i + 1 < argc) o.words = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "-s") && i + 1 < argc) o.flash_len = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--write"))            o.do_write = 1;
        else if (!strcmp(a, "--fix-checksum"))     o.fix_checksum = 1;
        else if (!strcmp(a, "--force-flash"))      o.force_flash = 1;
        else if (!strcmp(a, "--mac") && i + 1 < argc) {
            int prc = parse_mac(argv[++i], o.mac);
            if (prc) {
                fprintf(stderr,
                        "invalid --mac: use a nonzero unicast address such as 02:a0:c9:12:34:56\n");
                return 2;
            }
            o.use_fixed_mac = 1;
        }
        else if (!strcmp(a, "--mac")) {
            fprintf(stderr, "--mac needs an address\n");
            return 2;
        }
        else if (!strcmp(a, "--keep-image-mac"))   o.keep_image_mac = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", a); return 2; }
    }

    if (o.use_fixed_mac && o.keep_image_mac) {
        fprintf(stderr, "--mac and --keep-image-mac are mutually exclusive\n");
        return 2;
    }
    if ((o.use_fixed_mac || o.keep_image_mac) && o.cmd != CMD_FLASHWRITE) {
        fprintf(stderr, "--mac and --keep-image-mac apply only to flashwrite\n");
        return 2;
    }

    struct pci_dev devs[MAX_DEV];
    int n = pci_scan_i225(devs, MAX_DEV);
    if (n < 0) { fprintf(stderr, "scan: %s\n", strerror(-n)); return 1; }
    if (n == 0) { fprintf(stderr, "No i225/i226 controllers found.\n"); return 1; }

    if (o.cmd == CMD_LIST) {
        printf("Found %d controller(s):\n", n);
        for (int i = 0; i < n; i++) print_dev(&devs[i]);
        return 0;
    }

    int idx = pick_device(devs, n, o.bdf);
    if (idx < 0) { fprintf(stderr, "device %s not found\n", o.bdf ? o.bdf : "?"); return 1; }
    struct pci_dev *d = &devs[idx];

    int rc = pci_map_bar0(d);
    if (rc) {
        fprintf(stderr, "map BAR0 of %s: %s\n", d->bdf, strerror(-rc));
        fprintf(stderr, "(run as root; the igc driver may need unbinding first)\n");
        return 1;
    }
    printf("Target: "); print_dev(d);
    printf("Flash detected: %s\n", nvm_flash_present(d) ? "yes" : "no");

    int ret = 1;
    switch (o.cmd) {
    case CMD_DUMP:     ret = do_dump(d, &o);     break;
    case CMD_VERIFY:   ret = do_verify(d, &o);   break;
    case CMD_CHECKSUM: ret = do_checksum(d, &o); break;
    case CMD_WRITE:    ret = do_write(d, &o);    break;
    case CMD_FLASHDUMP:  ret = do_flashdump(d, &o);  break;
    case CMD_FLASHWRITE: ret = do_flashwrite(d, &o); break;
    case CMD_FLSW:       ret = do_flsw(d);           break;
    default:           usage();                  break;
    }

    pci_unmap_bar0(d);
    return ret;
}
