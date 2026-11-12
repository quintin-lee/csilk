#include "csilk/drivers/db/migration.h"
#include "csilk/core/primitives/hash.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
csilk_migration_checksum_content(const char* content, size_t len, char out_checksum[65])
{
    if (!content || !out_checksum || len > (size_t)(64 * 1024 * 1024)) {
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
    while (norm_len > 0 && norm_len <= len &&
           (norm[norm_len - 1] == ' ' || norm[norm_len - 1] == '\t' || norm[norm_len - 1] == '\n' ||
            norm[norm_len - 1] == '\r' || norm[norm_len - 1] == '\v' ||
            norm[norm_len - 1] == '\f')) {
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
    if (len < 0 || (unsigned long)len > (unsigned long)(64 * 1024 * 1024)) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    size_t fsz = (size_t)len;
    char*  buf = calloc(fsz + 1, 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t pos = 0;
    while (pos < fsz && !feof(f) && !ferror(f)) {
        size_t n = fread(buf + pos, 1, fsz - pos, f);
        if (n == 0) {
            break;
        }
        pos += n;
    }
    fclose(f);

    int rc = csilk_migration_checksum_content(buf, pos, out_checksum);
    free(buf);
    return rc;
}
