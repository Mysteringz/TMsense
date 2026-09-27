// Host stand-in for the one SHA-1 call tm_ws.cpp makes (the WebSocket
// Sec-WebSocket-Accept check). CommonCrypto on macOS, OpenSSL elsewhere.
#ifndef HOST_MBEDTLS_SHA1_H
#define HOST_MBEDTLS_SHA1_H

#include <stddef.h>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
static inline int mbedtls_sha1_ret(const unsigned char* input, size_t ilen, unsigned char output[20]) {
    CC_SHA1(input, (CC_LONG) ilen, output);
    return 0;
}
#else
#include <openssl/sha.h>
static inline int mbedtls_sha1_ret(const unsigned char* input, size_t ilen, unsigned char output[20]) {
    SHA1(input, ilen, output);
    return 0;
}
#endif

#endif
