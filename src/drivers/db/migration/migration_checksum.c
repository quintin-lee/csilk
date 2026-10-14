#include "csilk/drivers/db/migration.h"
#include "csilk/core/hash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
csilk_migration_checksum_content(const char* content, size_t len, char out_checksum[65])
{
    if (!content || !out_checksum) {
        return -1;
    }

    /* Normalization: filter out '\r', keep '\n' */
    char* norm = malloc(len + 1);
    if (!norm) {
        return -1;
    }

    size_t norm_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (content[i] != '\r') {
            norm[norm_len++] = content[i];
        }
    }

    /* Trim trailing whitespace and newlines */
    while (norm_len > 0 && isspace((unsigned char)norm[norm_len - 1])) {
        norm_len--;
    }
    norm[norm_len] = '\0';

    csilk_sha256_ctx ctx;
    csilk_sha256_init(&ctx);
    csilk_sha256_update(&ctx, (const uint8_t*)norm, norm_len);
    uint8_t md[32];
    csilk_sha256_final(&ctx, md);
    free(norm);

    for (int i = 0; i < 32; i++) {
        sprintf(out_checksum + (i * 2), "%02x", md[i]);
    }
    out_checksum[64] = '\0';
    return 0;
}

int
csilk_migration_checksum_file(const char* filepath, char out_checksum[65])
{
    if (!filepath || !out_checksum) {
        return -1;
    }

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long len = ftell(f);
    rewind(f);

    if (len < 0) {
        fclose(f);
        return -1;
    }

    char* buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read_bytes] = '\0';

    int rc = csilk_migration_checksum_content(buf, read_bytes, out_checksum);
    free(buf);
    return rc;
}
