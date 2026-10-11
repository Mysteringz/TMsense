#ifndef TM_SECURE_H
#define TM_SECURE_H
#include <stddef.h>
#include <stdint.h>

// v2: magic/version/type/uid as v1, boot32@10, seq32@14, uptime32@18,
// payload length16@22, key ID16@24, fresh epoch[16]@26, ciphertext, tag[16].
// The entire header is AAD. HKDF binds UID, key ID, role and epoch. A fresh
// epoch protects nonce uniqueness even after a saved counter is restored.
#define TM_SECURE_VERSION 2
#define TM_SECURE_HEADER 42
#define TM_SECURE_TAG 16
#define TM_SECURE_MAX (TM_SECURE_HEADER + 776 + TM_SECURE_TAG)
#define TM_SECURE_REPORT_DETECTIONS 21
#define TM_TYPE_ACK 0x12
#define TM_TYPE_CONTROL 0x13

bool tm_secure_derive(const uint8_t master[32], const uint8_t uid[6], uint16_t id,
                      const char* role, const uint8_t* salt, size_t salt_len, uint8_t out[32]);
size_t tm_secure_seal(uint8_t* out, size_t capacity, uint8_t type, const uint8_t uid[6],
                      uint32_t boot, uint32_t seq, uint32_t uptime, uint16_t id,
                      const uint8_t epoch[16], const uint8_t key[32], const uint8_t* payload, size_t len);
int tm_secure_open(const uint8_t* in, size_t len, const uint8_t key[32], uint8_t* payload, size_t capacity);
uint32_t tm_wire_boot(const uint8_t* in);
uint32_t tm_wire_seq(const uint8_t* in);
#endif
