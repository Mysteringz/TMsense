// Host stand-in for the one mbedTLS call tm_packet.cpp makes, so the real
// serializer compiles on a desktop. CommonCrypto on macOS, OpenSSL elsewhere.
#ifndef HOST_MBEDTLS_MD_H
#define HOST_MBEDTLS_MD_H

#include <stddef.h>

typedef enum { MBEDTLS_MD_SHA256 = 6 } mbedtls_md_type_t;
typedef struct { int type; } mbedtls_md_info_t;

static inline const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t t) {
    static const mbedtls_md_info_t sha256 = {MBEDTLS_MD_SHA256};
    return t == MBEDTLS_MD_SHA256 ? &sha256 : 0;
}

#if defined(__APPLE__)
#include <CommonCrypto/CommonHMAC.h>
static inline int mbedtls_md_hmac(const mbedtls_md_info_t*, const unsigned char* key, size_t keylen,
                                  const unsigned char* input, size_t ilen, unsigned char* output) {
    CCHmac(kCCHmacAlgSHA256, key, keylen, input, ilen, output);
    return 0;
}
#else
#include <openssl/hmac.h>
static inline int mbedtls_md_hmac(const mbedtls_md_info_t*, const unsigned char* key, size_t keylen,
                                  const unsigned char* input, size_t ilen, unsigned char* output) {
    unsigned int n = 32;
    HMAC(EVP_sha256(), key, (int) keylen, input, ilen, output, &n);
    return 0;
}
#endif

#endif
