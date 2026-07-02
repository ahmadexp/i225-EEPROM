/*
 * image.c - NVM image load/save/validate.
 */
#include "image.h"
#include "igc_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int image_load(const char *path, struct nvm_image *img)
{
    memset(img, 0, sizeof(*img));

    FILE *f = fopen(path, "rb");
    if (!f)
        return -errno;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz & 1)) {          /* must be non-empty, word-aligned */
        fclose(f);
        return -EINVAL;
    }

    uint8_t *raw = malloc((size_t)sz);
    if (!raw) {
        fclose(f);
        return -ENOMEM;
    }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        free(raw);
        fclose(f);
        return -EIO;
    }
    fclose(f);

    size_t nwords = (size_t)sz / 2;
    uint16_t *words = malloc(nwords * sizeof(uint16_t));
    if (!words) {
        free(raw);
        return -ENOMEM;
    }
    /* Flash stores words little-endian; decode explicitly so the tool behaves
     * identically on a big-endian host. */
    for (size_t i = 0; i < nwords; i++)
        words[i] = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
    free(raw);

    img->words  = words;
    img->nwords = nwords;
    img->nbytes = (size_t)sz;
    return 0;
}

void image_free(struct nvm_image *img)
{
    free(img->words);
    img->words = NULL;
    img->nwords = img->nbytes = 0;
}

int image_save(const char *path, const uint16_t *words, size_t nwords)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -errno;
    for (size_t i = 0; i < nwords; i++) {
        uint8_t b[2] = { (uint8_t)(words[i] & 0xFF),
                         (uint8_t)(words[i] >> 8) };
        if (fwrite(b, 1, 2, f) != 2) {
            fclose(f);
            return -EIO;
        }
    }
    fclose(f);
    return 0;
}

int image_checksum_ok(const struct nvm_image *img)
{
    if (img->nwords <= NVM_CHECKSUM_REG)
        return 0;
    uint16_t sum = 0;
    for (uint16_t i = 0; i <= NVM_CHECKSUM_REG; i++)
        sum += img->words[i];
    return sum == NVM_SUM;
}
