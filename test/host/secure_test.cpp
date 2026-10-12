#include <stdio.h>
#include <string.h>
#include "tm_packet.h"
#include <mbedtls/gcm.h>
static int checks = 0, failed = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failed; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
    // AES-256-GCM known answer: zero key, zero 96-bit IV, zero 128-bit plaintext.
    uint8_t key[32] = {0}, nonce[12] = {0}, zero[16] = {0}, cipher[16], tag[16];
    const uint8_t expected[16] = {0xce,0xa7,0x40,0x3d,0x4d,0x60,0x6b,0x6e,0x07,0x4e,0xc5,0xd3,0xba,0xf3,0x9d,0x18};
    const uint8_t expected_tag[16] = {0xd0,0xd1,0xc8,0xa7,0x99,0x99,0x6b,0xf0,0x26,0x5b,0x98,0xb5,0xd4,0x8a,0xb9,0x19};
    mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
    CHECK(mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0);
    CHECK(mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 16, nonce, 12, NULL, 0, zero, cipher, 16, tag) == 0);
    CHECK(!memcmp(cipher, expected, 16) && !memcmp(tag, expected_tag, 16)); mbedtls_gcm_free(&gcm);
    TmPacketContext ctx = {}; ctx.secure_id = 7; ctx.boot = 70000; ctx.seq = 100;
    const uint8_t uid[6] = {1,2,3,4,5,6}; memcpy(ctx.uid, uid, 6);
    memset(ctx.master, 0x11, 32); memset(ctx.epoch, 0x22, 16);
    static uint8_t packet[TM_PACKET_MAX_SIZE], changed[TM_PACKET_MAX_SIZE], old[TM_PACKET_MAX_SIZE];
    TmCommand command = {123, TM_CMD_IDENTIFY, 0, 5}, parsed = {};
    const size_t n = tm_build_command(packet, &ctx, &command);
    CHECK(n == TM_SECURE_HEADER + TM_COMMAND_SIZE + TM_SECURE_TAG);
    CHECK(tm_parse_command(packet, n, &ctx, &parsed) == 0 && parsed.seq == 123 && parsed.value == 5);
    for (size_t i = 0; i < n; ++i) {
        memcpy(changed, packet, n); changed[i] ^= 1;
        CHECK(tm_parse_command(changed, n, &ctx, &parsed) != 0);
    }
    TmPacketContext legacy = ctx; legacy.secure_id = 0; legacy.key_len = 4; memcpy(legacy.key, "test", 4);
    const size_t old_n = tm_build_command(old, &legacy, &command);
    CHECK(tm_parse_command(old, old_n, &ctx, &parsed) == TM_PARSE_VERSION);
    CHECK(tm_parse_command(packet, n, &legacy, &parsed) == TM_PARSE_VERSION);
    uint8_t type = 0;
    const size_t ack_n = tm_build_ack(packet, &ctx, ctx.boot, 100, TM_TYPE_REPORT);
    CHECK(tm_parse_ack(packet, ack_n, &ctx, &type) == 0 && type == TM_TYPE_REPORT && tm_wire_seq(packet) == 100);
    ctx.boot++; CHECK(tm_parse_ack(packet, ack_n, &ctx, &type) != 0); ctx.boot--;
    TmReportInfo info = {}; TmDetection dets[24] = {};
    const size_t report_n = tm_build_report(packet, &ctx, 1000, &info, dets, 24);
    CHECK(report_n == 219 && report_n <= 222 && tm_wire_boot(packet) == 70000);
    ctx.seq = UINT32_MAX; CHECK(tm_build_report(packet, &ctx, 1000, &info, dets, 0) == 0 && ctx.seq == UINT32_MAX);
    printf("secure_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
