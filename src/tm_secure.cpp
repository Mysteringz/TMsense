#include "tm_secure.h"
#include <string.h>
#include "mbedtls/md.h"
#include "mbedtls/gcm.h"

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = v >> (8 * i); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
uint32_t tm_wire_boot(const uint8_t* in) { return in[2] == 2 ? get32(in + 10) : (uint32_t)in[10] | ((uint32_t)in[11]<<8); }
uint32_t tm_wire_seq(const uint8_t* in) { return get32(in + (in[2] == 2 ? 14 : 12)); }

bool tm_secure_derive(const uint8_t master[32], const uint8_t uid[6], uint16_t id,
                      const char* role, const uint8_t* salt, size_t salt_len, uint8_t out[32]) {
    uint8_t zero[32] = {0}, prk[32], info[80];
    const size_t n = strlen(role);
    if (!id || n > 50) return false;
    const auto* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(md, salt_len ? salt : zero, salt_len ? salt_len : sizeof(zero), master, 32, prk)) return false;
    memcpy(info, "tmsense2/", 9); memcpy(info + 9, role, n); info[9 + n] = 0;
    memcpy(info + 10 + n, uid, 6); put16(info + 16 + n, id); info[18 + n] = 1;
    const int r = mbedtls_md_hmac(md, prk, 32, info, 19 + n, out);
    memset(prk, 0, sizeof(prk)); memset(info, 0, sizeof(info));
    return r == 0;
}

size_t tm_secure_seal(uint8_t* out, size_t capacity, uint8_t type, const uint8_t uid[6],
                      uint32_t boot, uint32_t seq, uint32_t uptime, uint16_t id,
                      const uint8_t epoch[16], const uint8_t key[32], const uint8_t* payload, size_t len) {
    if (!id || len > 776 || capacity < TM_SECURE_HEADER + len + TM_SECURE_TAG) return 0;
    // Move before writing the header: packet builders deliberately seal in place.
    memmove(out + TM_SECURE_HEADER, payload, len);
    out[0] = 'T'; out[1] = 'M'; out[2] = 2; out[3] = type;
    memcpy(out + 4, uid, 6); put32(out + 10, boot); put32(out + 14, seq); put32(out + 18, uptime);
    put16(out + 22, len); put16(out + 24, id); memcpy(out + 26, epoch, 16);
    uint8_t nonce[12] = {0}; put32(nonce + 4, boot); put32(nonce + 8, seq);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    int r = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!r) r = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, sizeof(nonce), out,
        TM_SECURE_HEADER, out + TM_SECURE_HEADER, out + TM_SECURE_HEADER, TM_SECURE_TAG, out + TM_SECURE_HEADER + len);
    mbedtls_gcm_free(&gcm);
    if (r) { memset(out, 0, TM_SECURE_HEADER + len + TM_SECURE_TAG); return 0; }
    return TM_SECURE_HEADER + len + TM_SECURE_TAG;
}

int tm_secure_open(const uint8_t* in, size_t len, const uint8_t key[32], uint8_t* payload, size_t capacity) {
    if (len < TM_SECURE_HEADER + TM_SECURE_TAG || in[0] != 'T' || in[1] != 'M' || in[2] != 2) return -1;
    const size_t n = (size_t)in[22] | ((size_t)in[23] << 8);
    if (n > 776 || n > capacity || len != TM_SECURE_HEADER + n + TM_SECURE_TAG || !(in[24] | in[25])) return -1;
    uint8_t nonce[12] = {0}; memcpy(nonce + 4, in + 10, 8);
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    int r = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!r) r = mbedtls_gcm_auth_decrypt(&gcm, n, nonce, sizeof(nonce), in, TM_SECURE_HEADER,
        in + TM_SECURE_HEADER + n, TM_SECURE_TAG, in + TM_SECURE_HEADER, payload);
    mbedtls_gcm_free(&gcm);
    if (r) { memset(payload, 0, n); return -1; }
    return (int)n;
}
