#include "roughtime_packet.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sodium.h"

typedef struct {
    const uint8_t *data;
    size_t size;
} bytes_t;

static uint32_t little32(const void *pointer)
{
    const uint8_t *b = pointer;
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint64_t little64(const uint8_t *b)
{
    return little32(b) | (uint64_t)little32(b + 4) << 32;
}

static void write32(uint8_t *out, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i) {
        out[i] = (uint8_t)(value >> (i * 8));
    }
}

static bool tag(bytes_t body, const char name[4], bytes_t *value)
{
    if (body.size < 8 || body.size % 4 != 0) {
        return false;
    }
    const uint32_t count = little32(body.data);
    if (count == 0 || count > body.size / 8) {
        return false;
    }
    const uint8_t *map = body.data + 4 * count;
    const size_t data_size = body.size - 8 * count;
    uint32_t previous = 0;
    size_t begin = 0;
    *value = (bytes_t){0};
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t key = little32(map + 4 * i);
        const size_t end = i + 1 < count ? little32(body.data + 4 * (i + 1)) : data_size;
        if ((i != 0 && key <= previous) || end < begin || end > data_size || end % 4 != 0) {
            return false;
        }
        if (key == little32(name)) {
            *value = (bytes_t){body.data + 8 * count + begin, end - begin};
        }
        previous = key;
        begin = end;
    }
    return value->data != NULL;
}

static bool tag_size(bytes_t body, const char name[4], size_t size, bytes_t *value)
{
    return tag(body, name, value) && value->size == size;
}

static void hash32(const uint8_t *data, size_t length, uint8_t out[32])
{
    uint8_t hash[64];
    crypto_hash_sha512(hash, data, length);
    memcpy(out, hash, 32);
}

void nearic_roughtime_build_request(uint8_t request[NEARIC_ROUGHTIME_REQUEST_SIZE], const uint8_t nonce[NEARIC_ROUGHTIME_NONCE_SIZE],
                                   const uint8_t trust_key[NEARIC_ROUGHTIME_KEY_SIZE])
{
    memset(request, 0, NEARIC_ROUGHTIME_REQUEST_SIZE);
    memcpy(request, "ROUGHTIM", 8);
    write32(request + 8, NEARIC_ROUGHTIME_REQUEST_SIZE - 12);
    uint8_t *body = request + 12;
    // Four offsets precede the five tags, followed by their values and padding.
    write32(body, 5);
    write32(body + 4, 4);
    write32(body + 8, 36);
    write32(body + 12, 68);
    write32(body + 16, 72);
    memcpy(body + 20, "VER\0SRV\0NONCTYPEZZZZ", 20);
    write32(body + 40, 1);
    uint8_t server_id[33] = { 0xff };
    memcpy(server_id + 1, trust_key, NEARIC_ROUGHTIME_KEY_SIZE);
    hash32(server_id, sizeof(server_id), body + 44);
    memcpy(body + 76, nonce, NEARIC_ROUGHTIME_NONCE_SIZE);
}

static bool signed_value(const char *context, bytes_t value, bytes_t signature, const uint8_t key[32])
{
    uint8_t message[NEARIC_ROUGHTIME_REQUEST_SIZE + 64];
    const size_t context_size = strlen(context) + 1;
    if (signature.size != 64 || value.size > sizeof(message) - context_size) {
        return false;
    }
    memcpy(message, context, context_size);
    memcpy(message + context_size, value.data, value.size);
    return crypto_sign_ed25519_verify_detached(signature.data, message, context_size + value.size, key) == 0;
}

static bool valid_tree(const uint8_t request[NEARIC_ROUGHTIME_REQUEST_SIZE], bytes_t path, uint32_t position,
                       bytes_t root)
{
    if (path.size % 32 != 0 || path.size / 32 > 32) {
        return false;
    }
    uint8_t leaf[NEARIC_ROUGHTIME_REQUEST_SIZE + 1] = {0};
    memcpy(leaf + 1, request, NEARIC_ROUGHTIME_REQUEST_SIZE);
    uint8_t h[32];
    hash32(leaf, sizeof(leaf), h);
    for (size_t i = 0; i < path.size; i += 32) {
        uint8_t node[65] = {1};
        const bool right = (position & 1) != 0;
        memcpy(node + 1, right ? path.data + i : h, 32);
        memcpy(node + 33, right ? h : path.data + i, 32);
        hash32(node, sizeof(node), h);
        position >>= 1;
    }
    return position == 0 && memcmp(h, root.data, 32) == 0;
}

bool nearic_roughtime_verify_response(const uint8_t request[NEARIC_ROUGHTIME_REQUEST_SIZE],
                                     const uint8_t nonce[NEARIC_ROUGHTIME_NONCE_SIZE],
                                     const uint8_t *packet, size_t packet_size,
                                     const uint8_t trust_key[NEARIC_ROUGHTIME_KEY_SIZE],
                                     nearic_roughtime_sample_t *result)
{
    if (packet_size < 12 || packet_size > NEARIC_ROUGHTIME_REQUEST_SIZE || memcmp(packet, "ROUGHTIM", 8) != 0 ||
        little32(packet + 8) != packet_size - 12) {
        return false;
    }
    const bytes_t body = {packet + 12, packet_size - 12};
    bytes_t type, echo, cert, signed_reply, signature, index, path;
    if (!tag_size(body, "TYPE", 4, &type) || little32(type.data) != 1 ||
        !tag_size(body, "NONC", NEARIC_ROUGHTIME_NONCE_SIZE, &echo) || memcmp(echo.data, nonce, NEARIC_ROUGHTIME_NONCE_SIZE) != 0 ||
        !tag(body, "CERT", &cert) || !tag(body, "SREP", &signed_reply) ||
        !tag_size(body, "SIG\0", 64, &signature) ||
        !tag_size(body, "INDX", 4, &index) || !tag(body, "PATH", &path)) {
        return false;
    }

    bytes_t delegation, cert_signature, signing_key, earliest, latest;
    if (!tag(cert, "DELE", &delegation) || !tag_size(cert, "SIG\0", 64, &cert_signature) ||
        !signed_value("Roughtime v1 delegation signature", delegation, cert_signature, trust_key) ||
        !tag_size(delegation, "PUBK", NEARIC_ROUGHTIME_KEY_SIZE, &signing_key) ||
        !tag_size(delegation, "MINT", 8, &earliest) || !tag_size(delegation, "MAXT", 8, &latest) ||
        !signed_value("Roughtime v1 response signature", signed_reply, signature, signing_key.data)) {
        return false;
    }

    bytes_t version, versions, midpoint, radius, root;
    if (!tag_size(signed_reply, "VER\0", 4, &version) || little32(version.data) != 1 ||
        !tag(signed_reply, "VERS", &versions) || versions.size == 0 || versions.size > 128 ||
        !tag_size(signed_reply, "MIDP", 8, &midpoint) || !tag_size(signed_reply, "RADI", 4, &radius) ||
        !tag_size(signed_reply, "ROOT", 32, &root) || !valid_tree(request, path, little32(index.data), root)) {
        return false;
    }

    uint32_t previous = 0;
    bool supports_one = false;
    for (size_t i = 0; i < versions.size; i += 4) {
        uint32_t offered = little32(versions.data + i);
        if (i != 0 && offered <= previous) {
            return false;
        }
        supports_one |= offered == 1;
        previous = offered;
    }
    const uint64_t timestamp = little64(midpoint.data);
    const uint32_t uncertainty = little32(radius.data);
    if (!supports_one || uncertainty == 0 || timestamp < little64(earliest.data) ||
        timestamp > little64(latest.data)) {
        return false;
    }
    *result = (nearic_roughtime_sample_t){timestamp, uncertainty};
    return true;
}
