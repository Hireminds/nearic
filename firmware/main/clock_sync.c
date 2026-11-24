#include "clock_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "roughtime_packet.h"
#include "sodium.h"

static const char *rng_name(void)
{
    return "esp-idf-hardware";
}

static randombytes_implementation rng = {
    .implementation_name = rng_name,
    .random = esp_random,
    .buf = esp_fill_random,
};

static esp_err_t create_socket(const nearic_clock_source_t *source, int *fd)
{
    char port[6];
    snprintf(port, sizeof(port), "%u", (unsigned int)source->port);
    const struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM, .ai_protocol = IPPROTO_UDP};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(source->host, port, &hints, &addresses) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const struct timeval timeout = {
        .tv_sec = source->max_round_trip_ms / 1000,
        .tv_usec = (source->max_round_trip_ms % 1000) * 1000,
    };
    esp_err_t error = ESP_FAIL;
    for (const struct addrinfo *addr = addresses; addr != NULL; addr = addr->ai_next) {
        const int candidate = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (candidate < 0) {
            continue;
        }
        if (setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
            setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
            connect(candidate, addr->ai_addr, addr->ai_addrlen) == 0) {
            *fd = candidate;
            error = ESP_OK;
            break;
        }
        close(candidate);
    }
    freeaddrinfo(addresses);
    return error;
}

static esp_err_t read_signed_time(const nearic_clock_source_t *source, int fd,
                                  nearic_roughtime_sample_t *sample)
{
    uint8_t *request = malloc(2 * NEARIC_ROUGHTIME_REQUEST_SIZE + 1);
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t *response = request + NEARIC_ROUGHTIME_REQUEST_SIZE;
    uint8_t nonce[NEARIC_ROUGHTIME_NONCE_SIZE];
    esp_fill_random(nonce, sizeof(nonce));
    nearic_roughtime_build_request(request, nonce, source->root_key);

    esp_err_t error = ESP_FAIL;
    const int64_t started = esp_timer_get_time();
    if (send(fd, request, NEARIC_ROUGHTIME_REQUEST_SIZE, 0) == NEARIC_ROUGHTIME_REQUEST_SIZE) {
        const ssize_t received = recv(fd, response, NEARIC_ROUGHTIME_REQUEST_SIZE + 1, 0);
        const int read_error = errno;
        const int64_t elapsed = esp_timer_get_time() - started;
        if (elapsed > (int64_t)source->max_round_trip_ms * 1000 ||
            (received < 0 && (read_error == EAGAIN || read_error == EWOULDBLOCK || read_error == ETIMEDOUT))) {
            error = ESP_ERR_TIMEOUT;
        } else if (received >= 0) {
            error = nearic_roughtime_verify_response(request, nonce, response, (size_t)received,
                                                    source->root_key, sample) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
    }
    free(request);
    return error;
}

esp_err_t nearic_clock_sync(const nearic_clock_source_t *source)
{
    if (source == NULL || source->host == NULL || source->host[0] == '\0' || source->port == 0 ||
        source->max_radius_seconds == 0 || source->max_round_trip_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (randombytes_set_implementation(&rng) != 0 || sodium_init() < 0) {
        return ESP_FAIL;
    }

    int fd;
    esp_err_t error = create_socket(source, &fd);
    if (error != ESP_OK) {
        return error;
    }
    nearic_roughtime_sample_t sample;
    error = read_signed_time(source, fd, &sample);
    close(fd);
    if (error != ESP_OK) {
        return error;
    }

    const time_t seconds = (time_t)sample.midpoint;
    if (seconds <= 0 || (uint64_t)seconds != sample.midpoint || sample.radius > source->max_radius_seconds) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const struct timeval clock = {.tv_sec = seconds};
    return settimeofday(&clock, NULL) == 0 ? ESP_OK : ESP_FAIL;
}
