#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NEARIC_ROUGHTIME_REQUEST_SIZE 1036U
#define NEARIC_ROUGHTIME_NONCE_SIZE 32U
#define NEARIC_ROUGHTIME_KEY_SIZE 32U

typedef struct {
    uint64_t midpoint;
    uint32_t radius;
} nearic_roughtime_sample_t;

void nearic_roughtime_build_request(uint8_t request[NEARIC_ROUGHTIME_REQUEST_SIZE], const uint8_t nonce[NEARIC_ROUGHTIME_NONCE_SIZE],
                                   const uint8_t trust_key[NEARIC_ROUGHTIME_KEY_SIZE]);
bool nearic_roughtime_verify_response(const uint8_t request[NEARIC_ROUGHTIME_REQUEST_SIZE],
                                     const uint8_t nonce[NEARIC_ROUGHTIME_NONCE_SIZE],
                                     const uint8_t *packet, size_t packet_size,
                                     const uint8_t trust_key[NEARIC_ROUGHTIME_KEY_SIZE],
                                     nearic_roughtime_sample_t *result);
