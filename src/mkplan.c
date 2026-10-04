/* mkplan - build the /config enable-flag plan from a device's OWN /config.
 *
 * Reads the stdout of `zte_config_read --read-config` on stdin (the
 * EFS_DATA_HEX lines), reconstructs the 15073-byte /config, validates it,
 * and writes a 30146-byte plan file: original[15073] + candidate[15073],
 * where candidate == original except flag102 (@499) set 0->1 and the format
 * CRC (@12..15) recomputed. No device identifiers are embedded anywhere;
 * the plan is derived entirely from the device it runs on.
 *
 *   zte_config_read --read-config | mkplan /path/plan.cfg
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIZE 15073u

/* /config format CRC: MSB-first, poly 0x04C11DB7, init 0, bytes 12..15 as 0. */
static uint32_t cfg_crc(const uint8_t *p) {
    uint32_t c = 0;
    for (unsigned i = 0; i < SIZE; i++) {
        c ^= (uint32_t)((i >= 12 && i < 16) ? 0 : p[i]) << 24;
        for (int j = 0; j < 8; j++)
            c = (c << 1) ^ ((c & 0x80000000u) ? 0x04c11db7u : 0);
    }
    return c;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: zte_config_read --read-config | mkplan <out.plan>\n"); return 2; }
    static uint8_t buf[SIZE];
    memset(buf, 0, sizeof buf);
    unsigned filled = 0;
    static char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char *d = strstr(line, "EFS_DATA_HEX offset=");
        if (!d) continue;
        unsigned off, len;
        if (sscanf(d, "EFS_DATA_HEX offset=%u length=%u data=", &off, &len) != 2) continue;
        char *hex = strstr(d, "data=");
        if (!hex) continue;
        hex += 5;
        if (len == 0 || off > SIZE || len > SIZE - off) { fprintf(stderr, "mkplan: chunk out of range\n"); return 3; }
        for (unsigned i = 0; i < len; i++) {
            unsigned b;
            if (sscanf(hex + 2 * i, "%2x", &b) != 1) { fprintf(stderr, "mkplan: bad hex at %u\n", off + i); return 3; }
            buf[off + i] = (uint8_t)b;
        }
        if (off + len > filled) filled = off + len;
    }
    if (filled != SIZE) { fprintf(stderr, "mkplan: got %u bytes, need %u (is /config readable?)\n", filled, SIZE); return 4; }
    if (le32(buf) != 0x78563412u || le32(buf + 8) != SIZE || le32(buf + SIZE - 4) != 0x21436587u) {
        fprintf(stderr, "mkplan: /config magic/size/trailer mismatch\n"); return 5;
    }
    if (cfg_crc(buf) != le32(buf + 12)) { fprintf(stderr, "mkplan: /config CRC mismatch\n"); return 6; }
    if (buf[499] != 0) { fprintf(stderr, "mkplan: flag102 is %u, need 0 (factory). Reboot and retry.\n", buf[499]); return 7; }

    static uint8_t cand[SIZE];
    memcpy(cand, buf, SIZE);
    cand[499] = 1;
    uint32_t c = cfg_crc(cand);
    cand[12] = (uint8_t)c; cand[13] = (uint8_t)(c >> 8); cand[14] = (uint8_t)(c >> 16); cand[15] = (uint8_t)(c >> 24);

    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror("mkplan: fopen"); return 8; }
    if (fwrite(buf, 1, SIZE, f) != SIZE || fwrite(cand, 1, SIZE, f) != SIZE) { fprintf(stderr, "mkplan: write failed\n"); fclose(f); return 9; }
    fclose(f);
    fprintf(stderr, "mkplan: wrote %s (30146 bytes; original flag102=0, candidate flag102=1)\n", argv[1]);
    return 0;
}
