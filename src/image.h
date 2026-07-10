/*
 * image.h - NVM image (.bin) loading and sanity checks.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>
#include <stddef.h>

struct nvm_image {
    uint16_t *words;   /* little-endian 16-bit words */
    size_t    nwords;
    size_t    nbytes;
};

/* Load a raw NVM .bin into 16-bit words (little-endian, as stored in flash). */
int  image_load(const char *path, struct nvm_image *img);
void image_free(struct nvm_image *img);

/* Save `nwords` words to a .bin file (little-endian). Used for backups. */
int  image_save(const char *path, const uint16_t *words, size_t nwords);

/* Sum words [0 .. NVM_CHECKSUM_REG] of the image; returns 1 if it equals the
 * expected NVM_SUM (i.e. the image's own checksum word is consistent). */
int  image_checksum_ok(const struct nvm_image *img);

/* Read or patch the permanent MAC in image bytes 0..5. Patching also
 * recomputes checksum word NVM_CHECKSUM_REG. */
int  image_get_mac(const struct nvm_image *img, uint8_t mac[6]);
int  image_patch_mac(struct nvm_image *img, const uint8_t mac[6],
                     uint16_t *checksum_out);

#endif /* IMAGE_H */
