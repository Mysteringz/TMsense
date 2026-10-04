#pragma once
// Host adapter only. ESP32 compiles the real mbedTLS GCM implementation.
#include <openssl/evp.h>
#include <string.h>
#define MBEDTLS_CIPHER_ID_AES 2
#define MBEDTLS_GCM_ENCRYPT 1
struct mbedtls_gcm_context { unsigned char key[32]; };
static inline void mbedtls_gcm_init(mbedtls_gcm_context* c) { memset(c, 0, sizeof(*c)); }
static inline void mbedtls_gcm_free(mbedtls_gcm_context* c) { memset(c, 0, sizeof(*c)); }
static inline int mbedtls_gcm_setkey(mbedtls_gcm_context* c, int, const unsigned char* key, unsigned bits) {
    if (bits != 256) return -1;
    memcpy(c->key, key, 32); return 0;
}
static inline int host_gcm(mbedtls_gcm_context* c, bool enc, size_t n, const unsigned char* iv, size_t ivn,
    const unsigned char* aad, size_t aadn, const unsigned char* input, unsigned char* output, size_t tagn, unsigned char* tag) {
    EVP_CIPHER_CTX* x = EVP_CIPHER_CTX_new(); if (!x) return -1;
    int count = 0, tail = 0;
    bool ok = EVP_CipherInit_ex(x, EVP_aes_256_gcm(), nullptr, nullptr, nullptr, enc) == 1 &&
        EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_SET_IVLEN, ivn, nullptr) == 1 &&
        EVP_CipherInit_ex(x, nullptr, nullptr, c->key, iv, enc) == 1 &&
        EVP_CipherUpdate(x, nullptr, &count, aad, aadn) == 1 && EVP_CipherUpdate(x, output, &count, input, n) == 1;
    if (!enc) ok = ok && EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_SET_TAG, tagn, tag) == 1;
    ok = ok && EVP_CipherFinal_ex(x, output + count, &tail) == 1;
    if (enc) ok = ok && EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_GET_TAG, tagn, tag) == 1;
    EVP_CIPHER_CTX_free(x); return ok ? 0 : -1;
}
static inline int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context* c, int, size_t n, const unsigned char* iv,
    size_t ivn, const unsigned char* aad, size_t aadn, const unsigned char* in, unsigned char* out, size_t tagn, unsigned char* tag) {
    return host_gcm(c, true, n, iv, ivn, aad, aadn, in, out, tagn, tag);
}
static inline int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context* c, size_t n, const unsigned char* iv,
    size_t ivn, const unsigned char* aad, size_t aadn, const unsigned char* tag, size_t tagn, const unsigned char* in, unsigned char* out) {
    return host_gcm(c, false, n, iv, ivn, aad, aadn, in, out, tagn, const_cast<unsigned char*>(tag));
}
