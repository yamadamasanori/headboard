/**
 * @file ml_derp.c
 * @brief Unified DERP I/O Task + Connection Management
 *
 * Single task handles BOTH reading and writing to DERP TLS connection.
 * This eliminates the need for a TLS mutex since only one task touches
 * the SSL context. Matches v1's single-threaded DERP model.
 *
 * Architecture:
 * - Poll for incoming DERP frames (TLS read) every iteration
 * - Drain TX queue between reads (TLS write)
 * - No mutex needed — single task owns the SSL context exclusively
 *
 * Backpressure strategy (from tailscaled):
 * When queue is full, dequeue oldest packet and retry up to 3 times.
 * If still full, drop the new packet.
 *
 * Reference: tailscale/wgengine/magicsock/derp.go (runDerpWriter)
 *            tailscale/derp/derphttp/derphttp_client.go
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "esp_vfs_eventfd.h"
#include <sys/select.h>
#include <unistd.h>
#if CONFIG_ML_DERP_VERIFY_CERT
#include "esp_crt_bundle.h"
#elif !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
/* ML_DERP_VERIFY_CERT depends on the bundle, so turning the bundle off
 * silently turns relay certificate checking off as well. Say so. */
#warning "MBEDTLS_CERTIFICATE_BUNDLE is disabled: DERP relay TLS certificates will not be verified (ML_DERP_VERIFY_CERT unavailable)"
#endif
#include "nacl_box.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "ml_derp";

/* Timeout for DERP connection handshake operations */
#define DERP_CONNECT_TIMEOUT_MS  10000

/* headboard: connections to peers' home DERP regions (ml->derp_alt[]).
 *
 * DERP servers in different regions do not forward packets to each other, so a
 * packet for a peer whose home region differs from ours must be sent through a
 * connection to the peer's region (tailscale/wgengine/magicsock/derp.go does the
 * same). Upstream MicroLink only ever connects to its own home region, so peers
 * homed elsewhere were unreachable unless a direct UDP path happened to work.
 *
 * Extra connections are opened on demand from the DERP I/O task, read like the
 * home connection, and closed after DERP_ALT_IDLE_MS without traffic. Each one
 * needs a TLS session (~30 KB: in PSRAM with CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC,
 * otherwise internal RAM, which is too small on a busy ESP32-S3), so a
 * connection is only opened when enough memory is free; otherwise the packet
 * goes out on the home connection as before. */
#define DERP_ALT_IDLE_MS            60000   /* close after this long without traffic */
#define DERP_ALT_RETRY_MS           30000   /* after a failed connect */
#define DERP_ALT_NOMEM_RETRY_MS     60000   /* after skipping for low memory */
#define DERP_ALT_KEEP_MS            10000   /* don't evict a slot used this recently */
#if CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
#define DERP_ALT_MIN_FREE_TLS       (96 * 1024)   /* PSRAM */
#define DERP_ALT_MIN_BLOCK_TLS      (32 * 1024)
#define DERP_ALT_TLS_CAPS           (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define DERP_ALT_MIN_FREE_INTERNAL  (16 * 1024)   /* socket, lwIP */
#else
#define DERP_ALT_MIN_FREE_TLS       (48 * 1024)   /* internal RAM */
#define DERP_ALT_MIN_BLOCK_TLS      (20 * 1024)
#define DERP_ALT_TLS_CAPS           (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DERP_ALT_MIN_FREE_INTERNAL  0
#endif

static esp_err_t derp_conn_open(microlink_t *ml, ml_derp_conn_t *c, uint16_t region, bool home);
static void derp_conn_close(ml_derp_conn_t *c);

/* ============================================================================
 * Custom BIO callbacks for TLS I/O
 *
 * These wrap the socket layer (ml_read_sock/ml_write_sock) so that both
 * lwIP and AT socket backends work transparently.
 *
 * recv is provided as a blocking recv (f_recv). With the recv_timeout BIO
 * (f_recv_timeout) combined with mbedtls_ssl_conf_read_timeout, the TLS 1.3
 * handshake fails with MBEDTLS_ERR_SSL_BAD_INPUT_DATA (Bad input
 * parameters), so we align with the scheme that already works on the
 * control plane side (coord_bio_recv in ml_coord_tls.c). Timeouts are
 * enforced by SO_RCVTIMEO on the socket; a timeout surfaces as EAGAIN ->
 * MBEDTLS_ERR_SSL_WANT_READ.
 * ========================================================================== */

/**
 * Custom blocking recv for mbedtls BIO (f_recv signature, no timeout arg).
 * SO_RCVTIMEO on the socket bounds the wait; a timeout surfaces as
 * EAGAIN/EWOULDBLOCK which we map to MBEDTLS_ERR_SSL_WANT_READ.
 */
static int ml_derp_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_read_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (ret == 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return ret;
}

/**
 * Custom send for mbedtls BIO.
 */
static int ml_derp_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_write_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

/**
 * Common cleanup for failure paths in ml_derp_connect() after TLS init.
 *
 * Previously we only closed the socket and never freed the mbedTLS
 * contexts, leaking entropy/ctr_drbg/ssl_conf on every connection failure,
 * and — for failures after a successful ssl_setup — the ~20KB TLS I/O
 * buffers as well (internal RAM, since CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y).
 * Combined with infinite retries, internal RAM shrank monotonically,
 * making mbedtls_ssl_setup's ALLOC_FAILED -> handshake's Bad input
 * parameters progressively worse on its own. So always free everything.
 *
 * mbedtls_*_free() zeroizes the structs after freeing, so a later
 * ml_derp_disconnect() freeing the same contexts is not a double free.
 */
static void derp_tls_abort(ml_derp_conn_t *c, int sock) {
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->ssl_conf);
#if ML_TLS_LEGACY_RNG
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
#endif
    if (sock >= 0) {
        ml_close_sock(sock);
    }
    c->sockfd = -1;
}

/* DERP frame types */
#define DERP_FRAME_SERVER_KEY   0x01
#define DERP_FRAME_CLIENT_INFO  0x02
#define DERP_FRAME_SERVER_INFO  0x03
#define DERP_FRAME_SEND_PACKET  0x04
#define DERP_FRAME_RECV_PACKET  0x05
#define DERP_FRAME_KEEP_ALIVE   0x06
#define DERP_FRAME_NOTE_PREFERRED 0x07
#define DERP_FRAME_PEER_GONE    0x08
#define DERP_FRAME_PING         0x12
#define DERP_FRAME_PONG         0x13

/* DISCO magic bytes: "TS" + sparkles emoji UTF-8 */
static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };

/* ============================================================================
 * TLS Read/Write Helpers
 * ========================================================================== */

/**
 * Read exactly `len` bytes via TLS with timeout and WANT_READ retry.
 * Returns number of bytes read on success, -1 on error, -2 on timeout.
 */
static int derp_tls_read_all(ml_derp_conn_t *c, uint8_t *data, size_t len, int timeout_ms) {
    size_t received = 0;
    uint64_t start_ms = ml_get_time_ms();

    while (received < len) {
        if (timeout_ms > 0 && (ml_get_time_ms() - start_ms) > (uint64_t)timeout_ms) {
            ESP_LOGW(TAG, "derp_tls_read_all timeout (%d/%d bytes in %dms)",
                     (int)received, (int)len, timeout_ms);
            return -2;
        }

        int ret = mbedtls_ssl_read(&c->ssl, data + received, len - received);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                ESP_LOGW(TAG, "DERP server closed connection");
                return -1;
            }
            ESP_LOGE(TAG, "TLS read failed: -0x%04x", -ret);
            return -1;
        }
        if (ret == 0) {
            ESP_LOGW(TAG, "TLS connection closed by peer (%d/%d bytes)",
                     (int)received, (int)len);
            return -1;
        }
        received += ret;
    }
    return (int)received;
}

/**
 * Read a DERP frame header (5 bytes: type + 4-byte BE length) with timeout.
 */
static esp_err_t derp_recv_frame_header(ml_derp_conn_t *c, uint8_t *type,
                                          uint32_t *len, int timeout_ms) {
    uint8_t header[5];
    int ret = derp_tls_read_all(c, header, 5, timeout_ms);
    if (ret < 0) {
        return (ret == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    *type = header[0];
    *len = ((uint32_t)header[1] << 24) |
           ((uint32_t)header[2] << 16) |
           ((uint32_t)header[3] << 8) |
           (uint32_t)header[4];

    return ESP_OK;
}

/**
 * Write exactly `len` bytes via TLS with WANT_WRITE retry.
 * Returns bytes written on success, -1 on error.
 * No mutex needed — called only from the DERP I/O task.
 */
static int derp_tls_write_all(ml_derp_conn_t *c, const uint8_t *data, size_t len) {
    size_t written = 0;
    int retries = 0;
    const int max_retries = 50;  /* 50 * 10ms = 500ms max */

    while (written < len) {
        int ret = mbedtls_ssl_write(&c->ssl, data + written, len - written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > max_retries) {
                    ESP_LOGW(TAG, "TLS write timeout after %d retries", retries);
                    return -1;
                }
                continue;
            }
            ESP_LOGE(TAG, "TLS write failed: -0x%04x", -ret);
            return -1;
        }
        written += ret;
        retries = 0;
    }
    return (int)written;
}

/* Write a complete DERP frame via TLS */
static int derp_write_frame(ml_derp_conn_t *c, uint8_t type,
                             const uint8_t *payload, uint32_t len) {
    /* 5-byte header: 1 type + 4 length (big-endian) */
    uint8_t header[5];
    header[0] = type;
    header[1] = (len >> 24) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = (len >> 8) & 0xFF;
    header[4] = len & 0xFF;

    if (derp_tls_write_all(c, header, 5) < 0) return -1;

    if (len > 0 && payload) {
        if (derp_tls_write_all(c, payload, len) < 0) return -1;
    }
    return 0;
}

/* Send a packet to a peer via DERP */
static int derp_send_packet(ml_derp_conn_t *c, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    /* SendPacket frame: 32-byte dest key + payload */
    size_t frame_len = 32 + len;
    uint8_t *frame = malloc(frame_len);
    if (!frame) return -1;

    memcpy(frame, dest_key, 32);
    memcpy(frame + 32, data, len);

    int ret = derp_write_frame(c, DERP_FRAME_SEND_PACKET, frame, frame_len);
    if (ret < 0) {
        ESP_LOGW(TAG, "derp_send_packet FAILED: dest=%02x%02x%02x%02x len=%d",
                 dest_key[0], dest_key[1], dest_key[2], dest_key[3], (int)len);
    }
    free(frame);
    return ret;
}

/* ============================================================================
 * DERP Frame Reading and Dispatch (runs on DERP I/O task)
 * ========================================================================== */

/* Packet classification */
typedef enum {
    PKT_DISCO,
    PKT_STUN,
    PKT_WIREGUARD,
    PKT_UNKNOWN,
} pkt_type_t;

static pkt_type_t classify_packet(const uint8_t *data, size_t len) {
    if (len >= 20 && (data[0] == 0x00 || data[0] == 0x01) && data[1] == 0x01) {
        return PKT_STUN;
    }
    if (len >= 62 && memcmp(data, DISCO_MAGIC, 6) == 0) {
        return PKT_DISCO;
    }
    if (len >= 4) {
        return PKT_WIREGUARD;
    }
    return PKT_UNKNOWN;
}

static void route_derp_packet(microlink_t *ml, uint8_t *data, size_t len,
                               const uint8_t *src_pubkey) {
    pkt_type_t type = classify_packet(data, len);

    ml_rx_packet_t pkt = {
        .data = data,
        .len = len,
        .via_derp = true,
    };
    memcpy(pkt.src_pubkey, src_pubkey, 32);

    QueueHandle_t target = (type == PKT_DISCO) ? ml->disco_rx_queue : ml->wg_rx_queue;
    if (xQueueSend(target, &pkt, 0) != pdTRUE) {
        free(data);
    } else {
        ml_wg_mgr_wake(ml);
    }
}

/* Dispatch a received DERP frame */
static void dispatch_derp_frame(microlink_t *ml, ml_derp_conn_t *c, uint8_t frame_type,
                                 uint8_t *src_key, uint8_t *payload, size_t payload_len) {
    switch (frame_type) {
    case DERP_FRAME_RECV_PACKET:
        if (payload) {
            c->last_used_ms = ml_get_time_ms();
            ESP_LOGD(TAG, "DERP RecvPacket: %d bytes from %02x%02x%02x%02x, hdr=%02x",
                     (int)payload_len,
                     src_key[0], src_key[1], src_key[2], src_key[3],
                     payload_len > 0 ? payload[0] : 0xFF);
            route_derp_packet(ml, payload, payload_len, src_key);
            return;  /* payload ownership transferred */
        }
        break;

    case DERP_FRAME_KEEP_ALIVE:
        ESP_LOGD(TAG, "DERP KeepAlive received");
        break;

    case DERP_FRAME_PING:
        /* Echo ping data back as PONG directly (single-threaded, safe to write) */
        if (payload && payload_len > 0) {
            ESP_LOGD(TAG, "DERP PING received, sending PONG");
            derp_write_frame(c, DERP_FRAME_PONG, payload, payload_len);
        }
        break;

    case DERP_FRAME_PONG:
        ESP_LOGD(TAG, "DERP PONG received");
        break;

    case DERP_FRAME_PEER_GONE:
        if (payload && payload_len >= 32) {
            ESP_LOGD(TAG, "DERP PeerGone: %02x%02x%02x%02x (len=%d)",
                     payload[0], payload[1], payload[2], payload[3],
                     (int)payload_len);
        }
        break;

    default:
        ESP_LOGD(TAG, "DERP frame type 0x%02x ignored (%d bytes)",
                 frame_type, (int)payload_len);
        break;
    }

    if (payload) free(payload);
}

/**
 * Try to read one DERP frame.
 * SO_RCVTIMEO on the socket bounds each read, so ssl_read never blocks
 * indefinitely (timeout surfaces as WANT_READ from the blocking-recv BIO).
 * Returns: 1 = frame read and dispatched, 0 = timeout (no data), <0 = error
 */
static int poll_derp_read(microlink_t *ml, ml_derp_conn_t *c) {
    if (!c->connected || c->sockfd < 0) return -1;

    /* Read 5-byte frame header.
     * SO_RCVTIMEO=100ms ensures read() returns within 100ms if no data. */
    uint8_t header[5];
    int n = mbedtls_ssl_read(&c->ssl, header, 5);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
        n == MBEDTLS_ERR_SSL_TIMEOUT) {
        return 0;  /* No data available / timeout */
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "DERP header read returned %d (0x%04x)", n, n < 0 ? -n : 0);
        return n;
    }
    if (n < 5) {
        /* ssl_read returns at most the rest of the current TLS record, and
         * a relay may split a frame header across records. Finish it; the
         * stream is only misaligned if the rest never arrives. Same 5 s
         * as the payload: the next record can be slow on the AT bridge. */
        if (derp_tls_read_all(c, header + n, 5 - n, 5000) < 0) {
            ESP_LOGW(TAG, "DERP partial header: got %d of 5 bytes", n);
            return -1;
        }
    }

    uint8_t frame_type = header[0];
    uint32_t len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];

    uint8_t src_key[32] = {0};
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (len == 0) {
        dispatch_derp_frame(ml, c, frame_type, src_key, NULL, 0);
        return 1;
    }

    if (len > 65536) {
        ESP_LOGW(TAG, "DERP frame too large: %lu", (unsigned long)len);
        return -1;
    }

    /* Read frame payload - we already got the header so payload should follow.
     * Use longer timeout (2s) since we KNOW data is coming. */
    uint8_t *buf = ml_psram_malloc(len);
    if (!buf) return -1;

    size_t total_read = 0;
    uint64_t payload_start = ml_get_time_ms();
    while (total_read < len) {
        /* Safety timeout: 5 seconds for payload */
        if (ml_get_time_ms() - payload_start > 5000) {
            ESP_LOGW(TAG, "DERP payload timeout at %d/%lu bytes",
                     (int)total_read, (unsigned long)len);
            free(buf);
            return -1;
        }
        n = mbedtls_ssl_read(&c->ssl, buf + total_read, len - total_read);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "DERP payload read error: %d (0x%04x) at %d/%lu bytes",
                     n, n < 0 ? -n : 0, (int)total_read, (unsigned long)len);
            free(buf);
            return n;
        }
        total_read += n;
    }

    /* For RecvPacket (0x05): first 32 bytes are sender's public key */
    if (frame_type == DERP_FRAME_RECV_PACKET && len <= 32) {
        /* Too short to carry a source key + payload: drop rather than route
         * it with an all-zero source key. */
        free(buf);
        return 1;
    }
    if (frame_type == DERP_FRAME_RECV_PACKET) {
        memcpy(src_key, buf, 32);
        payload = malloc(len - 32);
        if (payload) {
            memcpy(payload, buf + 32, len - 32);
            payload_len = len - 32;
        }
        free(buf);
    } else {
        payload = buf;
        payload_len = len;
    }

    dispatch_derp_frame(ml, c, frame_type, src_key, payload, payload_len);
    return 1;
}

/* ============================================================================
 * DERP TX Queue Processing
 * ========================================================================== */

/* ============================================================================
 * Idle waiting (no fixed-interval polling)
 *
 * The DERP I/O task used to spin with vTaskDelay(1) (~1000 wakeups/s while
 * idle), which also kept the chip out of light sleep. It now blocks in
 * select() on the DERP socket plus an eventfd that ml_derp_queue_send()
 * signals, so it wakes for incoming data, outgoing packets, or a periodic
 * timeout. The AT-socket bridge has no real fd: there it waits on the TX
 * queue with a short timeout (incoming data is polled at that rate).
 * Approach after TailTiny's idle work (Asimmimx/tailtiny 6335e77, bfa95f3).
 * ========================================================================== */

#define DERP_IDLE_WAIT_MS     250   /* upper bound: keepalives/reconnects are slower */
#define DERP_AT_POLL_MS       20

static void derp_wake(microlink_t *ml) {
    int fd = ml->derp_wake_fd;
    if (fd >= 0) {
        uint64_t one = 1;
        (void)write(fd, &one, sizeof(one));
    }
}

static void derp_wake_init(microlink_t *ml) {
    if (ml->derp_wake_fd >= 0) return;
    esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t err = esp_vfs_eventfd_register(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE: already registered */
        ESP_LOGW(TAG, "eventfd unavailable (%s); DERP task will poll", esp_err_to_name(err));
        return;
    }
    int fd = eventfd(0, 0);
    if (fd < 0) {
        ESP_LOGW(TAG, "eventfd() failed (errno %d); DERP task will poll", errno);
        return;
    }
    ml->derp_wake_fd = fd;
}

static void derp_idle_wait(microlink_t *ml) {
    if (uxQueueMessagesWaiting(ml->derp_tx_queue) > 0) return;
    if (ml->derp.connected && mbedtls_ssl_get_bytes_avail(&ml->derp.ssl) > 0) return;
    /* headboard: connections to peers' home regions are read the same way */
    bool alt_at = false;
    for (int i = 0; i < ML_DERP_MAX_ALT; i++) {
        ml_derp_conn_t *c = &ml->derp_alt[i];
        if (!c->connected) continue;
        if (mbedtls_ssl_get_bytes_avail(&c->ssl) > 0) return;
        if (ml_at_socket_is_at_fd(c->sockfd)) alt_at = true;
    }

    int sock = ml->derp.sockfd;
    int wfd = ml->derp_wake_fd;
    if (sock >= 0 && wfd >= 0 && !ml_at_socket_is_at_fd(sock) && !alt_at) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        FD_SET(wfd, &rfds);
        int maxfd = sock > wfd ? sock : wfd;
        for (int i = 0; i < ML_DERP_MAX_ALT; i++) {
            ml_derp_conn_t *c = &ml->derp_alt[i];
            if (c->connected && c->sockfd >= 0) {
                FD_SET(c->sockfd, &rfds);
                if (c->sockfd > maxfd) maxfd = c->sockfd;
            }
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = DERP_IDLE_WAIT_MS * 1000 };
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n > 0 && FD_ISSET(wfd, &rfds)) {
            uint64_t v;
            (void)read(wfd, &v, sizeof(v));
        } else if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));   /* never spin on a select error */
        }
    } else {
        ml_derp_tx_item_t peek;
        (void)xQueuePeek(ml->derp_tx_queue, &peek, pdMS_TO_TICKS(DERP_AT_POLL_MS));
    }
}

/* Queue a packet for DERP TX with backpressure */
esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    if (!ml || !dest_key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t *pkt_data = malloc(len);
    if (!pkt_data) return ESP_ERR_NO_MEM;
    memcpy(pkt_data, data, len);

    ml_derp_tx_item_t item = {
        .data = pkt_data,
        .len = len,
        .frame_type = DERP_FRAME_SEND_PACKET,
    };
    memcpy(item.dest_pubkey, dest_key, 32);

    /* headboard: remember the peer's home region, so the I/O task sends through
     * that region (peer state is owned by wg_mgr; a stale read only picks the
     * previous region for one packet) */
    for (int i = 0; i < ml->peer_count && i < ML_MAX_PEERS; i++) {
        if (ml->peers[i].active && memcmp(ml->peers[i].public_key, dest_key, 32) == 0) {
            item.region = ml->peers[i].derp_region;
            break;
        }
    }

    /* WG handshake packets (type 1=init, 2=response) get priority — front of queue.
     * This ensures handshake responses aren't delayed behind DISCO pings. */
    bool is_wg_handshake = (len >= 4 && (data[0] == 0x01 || data[0] == 0x02));

    /* Try to send to queue */
    if ((is_wg_handshake ? xQueueSendToFront(ml->derp_tx_queue, &item, 0)
                         : xQueueSend(ml->derp_tx_queue, &item, 0)) == pdTRUE) {
        derp_wake(ml);
        return ESP_OK;
    }

    /* Queue full - backpressure: drop oldest, retry up to 3 times */
    for (int i = 0; i < 3; i++) {
        ml_derp_tx_item_t dropped;
        if (xQueueReceive(ml->derp_tx_queue, &dropped, 0) == pdTRUE) {
            free(dropped.data);  /* Drop oldest */
        }
        if (xQueueSend(ml->derp_tx_queue, &item, 0) == pdTRUE) {
            derp_wake(ml);
            return ESP_OK;
        }
    }

    /* Still full after 3 attempts, drop new packet */
    free(pkt_data);
    return ESP_ERR_TIMEOUT;
}

/* ============================================================================
 * headboard: connections to peers' home regions
 * ========================================================================== */

/* Pick the connection to send a packet for `region` through. Opens (or reuses)
 * an extra connection when the region differs from ours; falls back to the
 * home connection when that is not possible. Runs on the DERP I/O task only. */
static ml_derp_conn_t *derp_conn_for_region(microlink_t *ml, uint16_t region) {
    if (region == 0 || region == ml->derp_home_region || ml->derp_home_region == 0 ||
        region == ml->derp.region) {
        return &ml->derp;
    }
    uint64_t now = ml_get_time_ms();
    ml_derp_conn_t *slot = NULL;
    for (int i = 0; i < ML_DERP_MAX_ALT; i++) {
        ml_derp_conn_t *c = &ml->derp_alt[i];
        if (c->region == region) {
            if (c->connected) {
                c->last_used_ms = now;
                return c;
            }
            if (now < c->retry_after_ms) {
                return &ml->derp;  /* recently failed: don't block the I/O task again */
            }
            slot = c;
            break;
        }
    }
    if (!slot) {
        /* A free slot, or the least recently used one that is not in active use */
        for (int i = 0; i < ML_DERP_MAX_ALT; i++) {
            ml_derp_conn_t *c = &ml->derp_alt[i];
            if (!c->connected && now >= c->retry_after_ms) {
                slot = c;
                break;
            }
            if (c->connected && now - c->last_used_ms > DERP_ALT_KEEP_MS &&
                (!slot || c->last_used_ms < slot->last_used_ms)) {
                slot = c;
            }
        }
        if (!slot) {
            return &ml->derp;
        }
        if (slot->connected) {
            ESP_LOGI(TAG, "Closing DERP region %d to make room for region %d", slot->region, region);
            derp_conn_close(slot);
        }
    }

    /* A TLS session needs memory; don't starve the rest of the system */
    size_t free_tls = heap_caps_get_free_size(DERP_ALT_TLS_CAPS);
    size_t block_tls = heap_caps_get_largest_free_block(DERP_ALT_TLS_CAPS);
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_tls < DERP_ALT_MIN_FREE_TLS || block_tls < DERP_ALT_MIN_BLOCK_TLS ||
        free_int < DERP_ALT_MIN_FREE_INTERNAL) {
        ESP_LOGW(TAG, "Not enough memory for DERP region %d (TLS heap free %u, largest %u; "
                 "internal free %u); sending through home region %d", region, (unsigned)free_tls,
                 (unsigned)block_tls, (unsigned)free_int, ml->derp_home_region);
        slot->region = region;
        slot->retry_after_ms = now + DERP_ALT_NOMEM_RETRY_MS;
        return &ml->derp;
    }

    ESP_LOGI(TAG, "Peer's home DERP region %d differs from ours (%d), opening a connection",
             region, ml->derp_home_region);
    if (derp_conn_open(ml, slot, region, false) != ESP_OK) {
        ESP_LOGW(TAG, "DERP region %d connect failed, retrying in %d s",
                 region, DERP_ALT_RETRY_MS / 1000);
        slot->region = region;
        slot->retry_after_ms = ml_get_time_ms() + DERP_ALT_RETRY_MS;
        return &ml->derp;
    }
    return slot;
}

/* Read from the extra connections and close idle or broken ones */
static void derp_alt_service(microlink_t *ml, uint32_t *frames_rx) {
    uint64_t now = ml_get_time_ms();
    for (int i = 0; i < ML_DERP_MAX_ALT; i++) {
        ml_derp_conn_t *c = &ml->derp_alt[i];
        if (!c->connected) continue;
        if (now - c->last_used_ms > DERP_ALT_IDLE_MS) {
            ESP_LOGI(TAG, "DERP region %d idle, closing", c->region);
            derp_conn_close(c);
            c->region = 0;
            continue;
        }
        for (int burst = 0; burst < 4; burst++) {
            int ret = poll_derp_read(ml, c);
            if (ret > 0) {
                (*frames_rx)++;
                c->last_recv_ms = ml_get_time_ms();
            } else if (ret == 0) {
                break;
            } else {
                ESP_LOGW(TAG, "DERP region %d read error: %d, closing", c->region, ret);
                derp_conn_close(c);
                c->retry_after_ms = ml_get_time_ms() + 5000;
                break;
            }
        }
    }
}

/* ============================================================================
 * Unified DERP I/O Task
 * ========================================================================== */

void ml_derp_tx_task(void *arg) {
    microlink_t *ml = (microlink_t *)arg;
    ESP_LOGI(TAG, "DERP I/O task started (Core %d)", xPortGetCoreID());
    derp_wake_init(ml);

    uint32_t frames_rx = 0;
    uint32_t frames_tx = 0;
    uint64_t last_status_ms = 0;
    uint32_t loop_count = 0;
    uint64_t last_heartbeat_ms = 0;
    uint64_t connected_since_ms = 0;
    bool verbose_phase = false;  /* verbose logging for first 15s after connect */

    while (!(xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)) {
        loop_count++;
        uint64_t loop_start = ml_get_time_ms();

        /* Unconditional heartbeat - proves task is alive */
        if (loop_start - last_heartbeat_ms > 5000) {
            ESP_LOGD(TAG, "HEARTBEAT: loop=%lu conn=%d rx=%lu tx=%lu stack_free=%lu",
                     (unsigned long)loop_count, ml->derp.connected,
                     (unsigned long)frames_rx, (unsigned long)frames_tx,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = loop_start;
        }

        /* ---- Periodic status logging (always, even when disconnected) ---- */
        {
            uint64_t now_ms = loop_start;
            if (now_ms - last_status_ms > 10000) {
                ESP_LOGD(TAG, "DERP status: connected=%d fd=%d rx=%lu tx=%lu loops=%lu",
                         ml->derp.connected, ml->derp.sockfd,
                         (unsigned long)frames_rx, (unsigned long)frames_tx,
                         (unsigned long)loop_count);
                for (int i = 0; i < ML_DERP_MAX_ALT; i++) {
                    if (ml->derp_alt[i].connected) {
                        ESP_LOGI(TAG, "DERP status: region %d connected (peer's home), idle %lu s",
                                 ml->derp_alt[i].region,
                                 (unsigned long)((now_ms - ml->derp_alt[i].last_used_ms) / 1000));
                    }
                }
                last_status_ms = now_ms;
            }
        }

        /* ---- Handle DERP connect request from coord task ---- */
        {
            EventBits_t bits = xEventGroupGetBits(ml->events);
            /* Always consume a connect request. One that arrives while we
             * are connected (or reconnecting) is already satisfied; left
             * set, it would fire after the next read/write error and run a
             * connect over the failed, not yet freed session. A pending
             * RECONNECT does the full disconnect + connect itself. */
            if (bits & ML_EVT_DERP_CONNECT_REQ) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
            }
            if ((bits & ML_EVT_DERP_CONNECT_REQ) && !ml->derp.connected &&
                !(bits & ML_EVT_DERP_RECONNECT)) {
                /* Retry up to 3 times with 2s backoff */
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP connect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    } else {
                        ESP_LOGI(TAG, "DERP connect requested, connecting from I/O task");
                    }
                    /* Each attempt can cost a DNS timeout plus a 20 s TLS
                     * handshake; don't start one that stop will discard. */
                    if (ml_shutdown_requested(ml)) break;
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP connect attempt %d failed", attempt + 1);
                }
            }
            if (bits & ML_EVT_DERP_RECONNECT) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_RECONNECT);
                ESP_LOGW(TAG, "DERP reconnect requested (was %s)",
                         ml->derp.connected ? "connected" : "disconnected");
                ml_derp_disconnect(ml);
                verbose_phase = false;
                /* Auto-reconnect after disconnect */
                vTaskDelay(pdMS_TO_TICKS(1000));
                for (int attempt = 0; attempt < 3 && !ml->derp.connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP reconnect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (ml_shutdown_requested(ml)) break;
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP reconnect attempt %d failed", attempt + 1);
                }
            }
        }

        /* Disable verbose logging after 15s */
        if (verbose_phase && ml_get_time_ms() - connected_since_ms > 15000) {
            verbose_phase = false;
        }

        if (!ml->derp.connected) {
            /* Not connected - just wait and check again */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ---- Phase 1: Drain TX queue FIRST (prioritize outgoing) ---- */
        {
            for (int tx_count = 0; tx_count < 8; tx_count++) {
                ml_derp_tx_item_t item;
                if (xQueueReceive(ml->derp_tx_queue, &item, 0) != pdTRUE) {
                    break;
                }
                if (!ml->derp.connected) {
                    free(item.data);
                    continue;
                }
                int ret;
                /* headboard: through the peer's home region when it differs from ours */
                ml_derp_conn_t *c = (item.frame_type == DERP_FRAME_SEND_PACKET)
                                        ? derp_conn_for_region(ml, item.region) : &ml->derp;
                if (item.frame_type == DERP_FRAME_SEND_PACKET) {
                    ESP_LOGD(TAG, "DERP TX: SendPacket %d bytes, dest=%02x%02x%02x%02x, hdr=%02x, region=%d",
                             (int)item.len, item.dest_pubkey[0], item.dest_pubkey[1],
                             item.dest_pubkey[2], item.dest_pubkey[3],
                             item.data[0], c == &ml->derp ? ml->derp_home_region : c->region);
                    ret = derp_send_packet(c, item.dest_pubkey, item.data, item.len);
                } else {
                    ret = derp_write_frame(c, item.frame_type, item.data, item.len);
                }
                if (ret < 0 && c != &ml->derp) {
                    ESP_LOGW(TAG, "DERP region %d write failed, closing", c->region);
                    derp_conn_close(c);
                    c->retry_after_ms = ml_get_time_ms() + 5000;
                } else if (ret < 0) {
                    ESP_LOGW(TAG, "DERP write failed");
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                } else {
                    frames_tx++;
                }
                free(item.data);
            }
        }

        if (!ml->derp.connected) continue;

        /* ---- Phase 2: Poll for incoming DERP frames ---- */
        {
            int burst;
            for (burst = 0; burst < 4; burst++) {
                int ret = poll_derp_read(ml, &ml->derp);
                if (ret > 0) {
                    frames_rx++;
                    ml->derp.last_recv_ms = ml_get_time_ms();
                } else if (ret == 0) {
                    break;  /* No more data / timeout */
                } else {
                    ESP_LOGW(TAG, "DERP read error: %d", ret);
                    ml->derp.connected = false;
                    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                    break;
                }
            }
        }

        /* headboard: connections to peers' home regions */
        if (ml->derp.connected) {
            derp_alt_service(ml, &frames_rx);
        }

        /* Block until there is work (incoming data, queued TX) or a timeout */
        derp_idle_wait(ml);
    }

    /* Release the TLS session, the socket and any packets still queued for
     * TX. Nobody else may: the connection is owned by this task. */
    ML_HEAP_TRACE("derp_tx: exit begin");
    ml_derp_disconnect(ml);
    ML_HEAP_TRACE("derp_tx: TLS + socket freed");

    ESP_LOGI(TAG, "DERP I/O task exiting");
    ml_task_exit(ml, ML_TASK_BIT_DERP_TX);
}

/* ============================================================================
 * DERP Connection Management (called from coord task)
 * ========================================================================== */

/* headboard: open a DERP connection on `c` to `region` (our home region when
 * `home`). Split out of ml_derp_connect() so that extra connections to peers'
 * home regions use the same code. */
static esp_err_t derp_conn_open(microlink_t *ml, ml_derp_conn_t *c, uint16_t region, bool home) {
    /* A failed session is only marked (connected=false) by the read/write
     * error paths. Free it before the TLS contexts and sockfd below are
     * re-initialised, or its TLS buffers and lwIP socket leak. */
    if (c->sockfd >= 0) {
        ESP_LOGW(TAG, "DERP connect over an open session (fd=%d); closing it first",
                 c->sockfd);
        if (home) {
            ml_derp_disconnect(ml);
        } else {
            derp_conn_close(c);
        }
    }

    /* Determine DERP host/port from DERPMap with node failover.
     * Always start from node 0 (the first/preferred node in the DERPMap).
     * Only rotate to a different node after a SUCCESSFUL connection drops,
     * NOT on connection failure (to avoid bouncing between nodes).
     * headboard: a peer's home region must be in the DERPMap (no default host) */
    const char *derp_host = home ? ML_DERP_HOST : NULL;
    int derp_port = ML_DERP_PORT;
    /* Track the region actually used, for logging (differs from HomeDERP on fallback) */
    uint16_t derp_region_used = region ? region : ML_DERP_REGION;
    bool node_selected = false;

    if (ml->derp_region_count > 0 && region > 0) {
        for (int i = 0; i < ml->derp_region_count; i++) {
            if (ml->derp_regions[i].region_id == region) {
                /* Always use the first non-stun-only node (preferred node).
                 * This ensures we connect to the same node as most peers. */
                for (int attempt = 0; attempt < ml->derp_regions[i].node_count; attempt++) {
                    if (!ml->derp_regions[i].nodes[attempt].stun_only &&
                        ml->derp_regions[i].nodes[attempt].hostname[0]) {
                        derp_host = ml->derp_regions[i].nodes[attempt].hostname;
                        if (ml->derp_regions[i].nodes[attempt].derp_port > 0) {
                            derp_port = ml->derp_regions[i].nodes[attempt].derp_port;
                        }
                        node_selected = true;
                        break;
                    }
                }
                break;
            }
        }
    }

    /* Fallback for when HomeDERP is not present in the DERPMap.
     * With headscale-based controllers (e.g. ZTL), HomeDERP may still point
     * at an official Tailscale region (e.g. 9) while the DERPMap only
     * contains their own region (e.g. 900). Falling back to the default
     * ML_DERP_HOST (official) in that case is a dead end — neither auth nor
     * TLS succeeds against the official DERP — so instead pick the first
     * usable node (not avoid, not stun-only, has a hostname) from the
     * DERPMap the controller handed out. In the normal official-Tailscale
     * case, HomeDERP is in the DERPMap so node_selected is set and this
     * branch is never taken; behavior is unchanged. */
    if (!node_selected && !home) {
        ESP_LOGW(TAG, "DERP region %d is not in the DERPMap", region);
        return ESP_FAIL;
    }
    if (!node_selected && ml->derp_region_count > 0) {
        for (int i = 0; i < ml->derp_region_count && !node_selected; i++) {
            ml_derp_region_t *r = &ml->derp_regions[i];
            if (r->avoid) continue;
            for (int j = 0; j < r->node_count; j++) {
                if (!r->nodes[j].stun_only && r->nodes[j].hostname[0]) {
                    derp_host = r->nodes[j].hostname;
                    derp_port = (r->nodes[j].derp_port > 0) ? r->nodes[j].derp_port
                                                            : ML_DERP_PORT;
                    derp_region_used = r->region_id;
                    node_selected = true;
                    ESP_LOGW(TAG, "Home DERP region %d not in DERPMap, falling back to region %d (%s)",
                             ml->derp_home_region, derp_region_used, derp_host);
                    break;
                }
            }
        }
    }

    int64_t t_derp_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Connecting to DERP %s:%d (region %d%s)",
             derp_host, derp_port, derp_region_used, home ? "" : ", peer's home region");

    /* DNS resolve — accept IPv4 or IPv6 (carrier may be IPv6-only) */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", derp_port);

    if (ml_getaddrinfo(derp_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", derp_host);
        return ESP_FAIL;
    }

    int64_t t_derp_dns = esp_timer_get_time();
    ESP_LOGD(TAG, "[TIMING] DERP DNS: %ld ms", (long)((t_derp_dns - t_derp_start) / 1000));

    /* TCP connect — use address family from DNS result */
    int sock = ml_socket(res->ai_family, SOCK_STREAM, 0);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }

    /* Set connect timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        ml_close_sock(sock);
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }
    ml_freeaddrinfo(res);

    int64_t t_derp_tcp = esp_timer_get_time();
    ESP_LOGD(TAG, "[TIMING] DERP TCP connect: %ld ms", (long)((t_derp_tcp - t_derp_dns) / 1000));

    /* TLS setup */
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->ssl_conf);
#if ML_TLS_LEGACY_RNG
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
#endif

    /* Always check the return values of the mbedTLS setup functions and
     * abort immediately on failure instead of proceeding to the handshake.
     * On failure (typically ALLOC_FAILED), mbedtls_ssl_setup resets
     * ssl->conf to NULL in its error label, so ignoring the error and
     * proceeding to the handshake trips the sanity check with
     * MBEDTLS_ERR_SSL_BAD_INPUT_DATA — misreported as "Bad input
     * parameters" when the real cause is out-of-memory. */
    int cfg_ret;
#if ML_TLS_LEGACY_RNG
    cfg_ret = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
                           &c->entropy, NULL, 0);
    if (cfg_ret != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed: -0x%04x", -cfg_ret);
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }
#endif

    cfg_ret = mbedtls_ssl_config_defaults(&c->ssl_conf,
                                 MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_STREAM,
                                 MBEDTLS_SSL_PRESET_DEFAULT);
    if (cfg_ret != 0) {
        ESP_LOGE(TAG, "ssl_config_defaults failed: -0x%04x", -cfg_ret);
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }
#if CONFIG_ML_DERP_VERIFY_CERT
    /* Verify the relay certificate against ESP-IDF's CA bundle; the host name
     * is checked via mbedtls_ssl_set_hostname() below. */
    mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (esp_crt_bundle_attach(&c->ssl_conf) != ESP_OK) {
        ESP_LOGE(TAG, "TLS setup failed: could not attach the CA bundle");
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }
#else
    {
        static bool s_warned;
        if (!s_warned) {
            s_warned = true;
            ESP_LOGW(TAG, "DERP TLS certificates are not verified (%s)",
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
                     "CONFIG_ML_DERP_VERIFY_CERT=n"
#else
                     "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n"
#endif
                     );
        }
    }
    mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
#endif
#if ML_TLS_LEGACY_RNG
    mbedtls_ssl_conf_rng(&c->ssl_conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);
#endif
    /* Pin the DERP connection to TLS 1.2. When the DERP relay uses a
     * Let's Encrypt ECDSA certificate, the ESP-IDF mbedTLS TLS 1.3 path
     * cannot interpret the certificate's signature algorithm OID and fails
     * with "X509 - Signature algorithm (oid) is unsupported" (even with
     * authmode=VERIFY_NONE, TLS 1.3 cannot skip certificate processing).
     * DERP also accepts TLS 1.2, so pin to 1.2 to bypass the 1.3
     * certificate path. */
    mbedtls_ssl_conf_max_tls_version(&c->ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);

    cfg_ret = mbedtls_ssl_setup(&c->ssl, &c->ssl_conf);
    if (cfg_ret != 0) {
        /* -0x7F00 (ALLOC_FAILED) means internal RAM exhaustion. To allocate
         * the ~20KB TLS I/O buffers from PSRAM instead, set
         * CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y. */
        ESP_LOGE(TAG, "mbedtls_ssl_setup failed: -0x%04x", -cfg_ret);
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }
    cfg_ret = mbedtls_ssl_set_hostname(&c->ssl, derp_host);
    if (cfg_ret != 0) {
        ESP_LOGE(TAG, "ssl_set_hostname failed: -0x%04x", -cfg_ret);
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }
    /* Store socket fd BEFORE setting bio.
     * Use custom BIO callbacks that route through ml_read_sock/ml_write_sock,
     * which transparently support both lwIP and AT socket backends.
     * Timeout is handled via SO_RCVTIMEO. */
    c->sockfd = sock;
    /* Pass a blocking recv as f_recv and set f_recv_timeout to NULL
     * (same scheme as the control plane; avoids the TLS 1.3 Bad input
     * data failure). */
    mbedtls_ssl_set_bio(&c->ssl, &c->sockfd,
                         ml_derp_bio_send, ml_derp_bio_recv, NULL);

    /* TLS handshake - socket has 10s SO_RCVTIMEO from connect phase; the
     * overall deadline stops a relay that accepts TCP and then stalls from
     * hanging this task forever. */
    int ret;
    const int64_t hs_deadline_us = esp_timer_get_time() + 20LL * 1000 * 1000;
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if ((ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
            esp_timer_get_time() < hs_deadline_us && !ml_shutdown_requested(ml)) {
            continue;
        }
        char err_buf[128];
#if defined(MBEDTLS_ERROR_C)
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
#else
        /* CONFIG_MBEDTLS_ERROR_STRINGS=n: mbedTLS 4 has no strerror stub */
        snprintf(err_buf, sizeof(err_buf), "-0x%04x", (unsigned)-ret);
#endif
        ESP_LOGE(TAG, "TLS handshake failed: %s", err_buf);
#if CONFIG_ML_DERP_VERIFY_CERT
        uint32_t vflags = mbedtls_ssl_get_verify_result(&c->ssl);
        if (vflags != 0 && vflags != (uint32_t)-1) {
            char why[160];
            mbedtls_x509_crt_verify_info(why, sizeof(why), "", vflags);
            ESP_LOGE(TAG, "DERP certificate for %s not accepted: %s", derp_host, why);
        }
#endif
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }

    int64_t t_derp_tls = esp_timer_get_time();
    ESP_LOGD(TAG, "[TIMING] DERP TLS handshake: %ld ms", (long)((t_derp_tls - t_derp_tcp) / 1000));
    ESP_LOGD(TAG, "TLS connected to DERP");

    /* HTTP Upgrade: GET /derp with Upgrade: DERP header */
    char upgrade_req[256];
    snprintf(upgrade_req, sizeof(upgrade_req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: Upgrade\r\n"
             "Upgrade: DERP\r\n"
             "\r\n",
             derp_host);

    ret = mbedtls_ssl_write(&c->ssl, (const uint8_t *)upgrade_req, strlen(upgrade_req));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send HTTP upgrade");
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }

    /* Read HTTP response byte-by-byte until \r\n\r\n to avoid over-reading
     * into the DERP binary frame stream (matching v1 approach) */
    {
        uint8_t resp_buf[512];
        int resp_len = 0;
        bool found_end = false;
        uint64_t http_start = ml_get_time_ms();

        while (resp_len < (int)sizeof(resp_buf) - 1) {
            if (ml_get_time_ms() - http_start > DERP_CONNECT_TIMEOUT_MS) {
                ESP_LOGE(TAG, "HTTP upgrade response timeout");
                derp_tls_abort(c, sock);
                return ESP_FAIL;
            }

            ret = mbedtls_ssl_read(&c->ssl, resp_buf + resp_len, 1);
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGE(TAG, "HTTP upgrade read failed: -0x%04x", -ret);
                derp_tls_abort(c, sock);
                return ESP_FAIL;
            }
            if (ret == 0) {
                ESP_LOGE(TAG, "Connection closed during HTTP upgrade");
                derp_tls_abort(c, sock);
                return ESP_FAIL;
            }
            resp_len++;

            /* Check for \r\n\r\n */
            if (resp_len >= 4 &&
                resp_buf[resp_len - 4] == '\r' && resp_buf[resp_len - 3] == '\n' &&
                resp_buf[resp_len - 2] == '\r' && resp_buf[resp_len - 1] == '\n') {
                found_end = true;
                break;
            }
        }

        resp_buf[resp_len] = '\0';

        if (!found_end || strncmp((char *)resp_buf, "HTTP/1.1 101", 12) != 0) {
            ESP_LOGE(TAG, "DERP upgrade rejected: %.100s", resp_buf);
            derp_tls_abort(c, sock);
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "HTTP 101 Switching Protocols received");
    }

    /* Match v1 exactly: O_NONBLOCK + short SO_RCVTIMEO + SO_SNDTIMEO.
     * O_NONBLOCK ensures read()/write() never block indefinitely.
     * SO_RCVTIMEO provides 100ms polling rhythm for reads.
     * SO_SNDTIMEO prevents writes from blocking too long. */
    {
        int flags = ml_fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            ml_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
        struct timeval io_tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100ms */
        ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
        ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    }

    /* ========================================================
     * DERP Handshake: ServerKey -> ClientInfo -> ServerInfo
     * ======================================================== */

    /* Step 1: Read ServerKey frame header using reliable read helper */
    uint8_t frame_type;
    uint32_t frame_len;
    esp_err_t err = derp_recv_frame_header(c, &frame_type, &frame_len, DERP_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ServerKey frame header (err=%d)", err);
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }

    if (frame_type != DERP_FRAME_SERVER_KEY || frame_len < 40) {
        ESP_LOGE(TAG, "Expected ServerKey frame (0x01), got 0x%02x len=%lu",
                 frame_type, (unsigned long)frame_len);
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }

    /* Read and verify 8-byte magic */
    uint8_t magic[8];
    static const uint8_t DERP_MAGIC[8] = {0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91};
    if (derp_tls_read_all(c, magic, 8, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read ServerKey magic");
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }

    if (memcmp(magic, DERP_MAGIC, 8) != 0) {
        ESP_LOGE(TAG, "Invalid DERP magic: %02x%02x%02x%02x%02x%02x%02x%02x",
                 magic[0], magic[1], magic[2], magic[3],
                 magic[4], magic[5], magic[6], magic[7]);
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "DERP magic verified");

    /* Read 32-byte server public key */
    uint8_t derp_server_key[32];
    if (derp_tls_read_all(c, derp_server_key, 32, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read server key");
        derp_tls_abort(c, sock);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "DERP server key received (first 8): %02x%02x%02x%02x%02x%02x%02x%02x",
             derp_server_key[0], derp_server_key[1], derp_server_key[2], derp_server_key[3],
             derp_server_key[4], derp_server_key[5], derp_server_key[6], derp_server_key[7]);

    /* Skip remaining bytes if frame_len > 40 */
    if (frame_len > 40) {
        uint8_t skip_buf[64];
        size_t remaining = frame_len - 40;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(skip_buf) ? sizeof(skip_buf) : remaining;
            if (derp_tls_read_all(c, skip_buf, chunk, DERP_CONNECT_TIMEOUT_MS) < 0) break;
            remaining -= chunk;
        }
    }

    /* Step 2: Send ClientInfo frame (type 0x02)
     * Payload: [our_nodekey(32)][nonce(24)][nacl_box(JSON)] */
    {
        const char *client_info_json = "{\"Version\":2,\"CanAckPings\":true,\"IsProber\":false}";
        size_t json_len = strlen(client_info_json);

        /* Generate random nonce */
        uint8_t nonce[NACL_BOX_NONCEBYTES];
        esp_fill_random(nonce, NACL_BOX_NONCEBYTES);

        /* Encrypt JSON with NaCl box: our WG private key -> DERP server public key */
        size_t ciphertext_len = json_len + NACL_BOX_MACBYTES;
        uint8_t *ciphertext = malloc(ciphertext_len);
        if (!ciphertext) {
            derp_tls_abort(c, sock);
            return ESP_FAIL;
        }

        if (nacl_box(ciphertext,
                     (const uint8_t *)client_info_json, json_len,
                     nonce,
                     derp_server_key,       /* recipient: DERP server */
                     ml->wg_private_key     /* sender: our WG node key */
                     ) != 0) {
            ESP_LOGE(TAG, "NaCl box encrypt failed");
            free(ciphertext);
            derp_tls_abort(c, sock);
            return ESP_FAIL;
        }

        /* Build ClientInfo frame payload: nodekey(32) + nonce(24) + ciphertext */
        size_t ci_payload_len = 32 + NACL_BOX_NONCEBYTES + ciphertext_len;
        uint8_t *ci_payload = malloc(ci_payload_len);
        if (!ci_payload) {
            free(ciphertext);
            derp_tls_abort(c, sock);
            return ESP_FAIL;
        }

        memcpy(ci_payload, ml->wg_public_key, 32);
        memcpy(ci_payload + 32, nonce, NACL_BOX_NONCEBYTES);
        memcpy(ci_payload + 32 + NACL_BOX_NONCEBYTES, ciphertext, ciphertext_len);
        free(ciphertext);

        ESP_LOGD(TAG, "DERP ClientInfo node_key=%02x%02x%02x%02x%02x%02x%02x%02x",
                 ml->wg_public_key[0], ml->wg_public_key[1],
                 ml->wg_public_key[2], ml->wg_public_key[3],
                 ml->wg_public_key[4], ml->wg_public_key[5],
                 ml->wg_public_key[6], ml->wg_public_key[7]);

        /* Send ClientInfo frame */
        if (derp_write_frame(c, DERP_FRAME_CLIENT_INFO, ci_payload, ci_payload_len) < 0) {
            ESP_LOGE(TAG, "Failed to send ClientInfo");
            free(ci_payload);
            derp_tls_abort(c, sock);
            return ESP_FAIL;
        }
        free(ci_payload);

        ESP_LOGD(TAG, "ClientInfo sent");
    }

    /* Step 3: Read ServerInfo frame (type 0x03) */
    {
        uint8_t si_type;
        uint32_t si_len;
        err = derp_recv_frame_header(c, &si_type, &si_len, DERP_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK && si_type == DERP_FRAME_SERVER_INFO && si_len > 0) {
            /* Read and discard ServerInfo payload */
            uint8_t *si_buf = malloc(si_len);
            if (si_buf) {
                derp_tls_read_all(c, si_buf, si_len, DERP_CONNECT_TIMEOUT_MS);
                free(si_buf);
            }
            ESP_LOGD(TAG, "ServerInfo received (discarded)");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "No ServerInfo frame (continuing anyway)");
        }
    }

    /* Send NotePreferred (type 0x07): this is our preferred DERP
     * (headboard: only on the home connection) */
    if (home) {
        uint8_t preferred = 0x01;
        derp_write_frame(c, DERP_FRAME_NOTE_PREFERRED, &preferred, 1);
    }

    /* Switch socket to short timeout for data phase.
     * Long timeout was needed for TLS handshake, but polling must be fast.
     * conf_read_timeout is deliberately not called: recv uses the f_recv
     * (blocking) scheme, so only SO_RCVTIMEO takes effect (with
     * f_recv_timeout unused, conf_read_timeout would be a no-op; same
     * policy as the TLS 1.3 Bad input data workaround). */
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200ms */
        ml_setsockopt(c->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    c->connected = true;
    c->last_recv_ms = ml_get_time_ms();
    c->last_used_ms = c->last_recv_ms;
    c->region = derp_region_used;
    if (home) {
        xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECTED);
    }

    int64_t t_derp_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP total: %ld ms (DNS=%ld, TCP=%ld, TLS=%ld, proto=%ld)",
             (long)((t_derp_done - t_derp_start) / 1000),
             (long)((t_derp_dns - t_derp_start) / 1000),
             (long)((t_derp_tcp - t_derp_dns) / 1000),
             (long)((t_derp_tls - t_derp_tcp) / 1000),
             (long)((t_derp_done - t_derp_tls) / 1000));
    ESP_LOGI(TAG, "DERP handshake complete, connected (region %d)", derp_region_used);
    return ESP_OK;
}

esp_err_t ml_derp_connect(microlink_t *ml) {
    return derp_conn_open(ml, &ml->derp, ml->derp_home_region, true);
}

/* headboard: free the TLS state and socket of one connection */
static void derp_conn_close(ml_derp_conn_t *c) {
    c->connected = false;
    if (c->sockfd >= 0) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->ssl_conf);
#if ML_TLS_LEGACY_RNG
        mbedtls_ctr_drbg_free(&c->ctr_drbg);
        mbedtls_entropy_free(&c->entropy);
#endif
        ml_close_sock(c->sockfd);
        c->sockfd = -1;
    }
}

void ml_derp_disconnect(microlink_t *ml) {
    ml->derp.connected = false;
    xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);

    derp_conn_close(&ml->derp);
    /* headboard: also close the connections to peers' home regions
     * (they are reopened on demand) */
    for (int i = 0; i < ML_DERP_MAX_ALT; i++) {
        derp_conn_close(&ml->derp_alt[i]);
        ml->derp_alt[i].region = 0;
    }

    /* Drain TX queue */
    ml_derp_tx_item_t item;
    while (xQueueReceive(ml->derp_tx_queue, &item, 0) == pdTRUE) {
        free(item.data);
    }

    ESP_LOGI(TAG, "DERP disconnected");
}
