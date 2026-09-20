#include "util.h"

/* cksum FILE: the CRC-32 of the contents and the size -- to check that two copies match. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= (uint8_t)t[i];
        for (int k = 0; k < 8; k++) { crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    }
    crc = ~crc;
    user_write_int((long long)crc); user_write(" ");
    user_write_int(len); user_write(" ");
    user_write(file);
    user_write("\n");
    user_exit();
}
