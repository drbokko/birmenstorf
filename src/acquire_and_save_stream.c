/*
 * acquire_and_save_stream.c — receive a fixed number of IMAGE messages, write TIFFs, optional flatfield TIFF
 *
 * Stops after --nimages stream IMAGE messages (legacy: -n / --images). Images are buffered as on the wire while a
 * background pthread FIFO-decodes into a second stack (see stream2_buffer_append_decoded_from_wire).
 * stderr shows recv IMAGE count vs decoded fifo depth (decode backlog may trail during heavy compression).
 * Flushes per-frame TIFFs like DectrisStream2Receiver_linux. Optional --generate-flatfield writes per-frame TIFFs plus
 * a mean flatfield; --generate-flatfield-only writes only the flatfield. Channel for the
 * mean is auto-selected (image, data, unnamed/empty, or most common non-mask).
 * Optional --flatfield-file PATH sets the flatfield TIFF path.
 */
#ifdef __linux__
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "stream2_buffer_decode_stack.h"
#include "tiff_writer.h"
#include "stream2.h"
#include "compression/src/compression.h"
#include <pthread.h>
#include <zmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8
#endif
#endif

#define AVG_TALLY_MAX 32

struct iface_stats {
    uint64_t rx_packets;
    uint64_t rx_errs;
    uint64_t rx_drop;
    uint64_t rx_frame;
    uint64_t tx_errs;
    uint64_t tx_drop;
};

#ifndef _WIN32
static int parse_iface_line(const char* line,
                            const char* iface,
                            struct iface_stats* st) {
    char name[64] = {0};
    unsigned long long rx_bytes, rx_packets, rx_errs, rx_drop, rx_fifo,
            rx_frame, rx_compressed, rx_multicast;
    unsigned long long tx_bytes, tx_packets, tx_errs, tx_drop, tx_fifo,
            tx_colls, tx_carrier, tx_compressed;
    int n = sscanf(line,
                   " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu "
                   "%llu %llu %llu %llu %llu %llu",
                   name, &rx_bytes, &rx_packets, &rx_errs, &rx_drop, &rx_fifo,
                   &rx_frame, &rx_compressed, &rx_multicast, &tx_bytes,
                   &tx_packets, &tx_errs, &tx_drop, &tx_fifo, &tx_colls,
                   &tx_carrier);
    if (n != 16)
        return -1;
    (void)rx_bytes;
    (void)rx_fifo;
    (void)rx_compressed;
    (void)rx_multicast;
    (void)tx_bytes;
    (void)tx_packets;
    (void)tx_errs;
    (void)tx_drop;
    (void)tx_fifo;
    (void)tx_colls;
    (void)tx_carrier;
    (void)tx_compressed;
    if (strcmp(name, iface) != 0)
        return 1;
    st->rx_packets = rx_packets;
    st->rx_errs = rx_errs;
    st->rx_drop = rx_drop;
    st->rx_frame = rx_frame;
    st->tx_drop = tx_drop;
    st->tx_errs = tx_errs;
    return 0;
}

static int read_iface_stats(const char* iface, struct iface_stats* st) {
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f)
        return -1;
    char line[512];
    int found = 0;
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        int r = parse_iface_line(line, iface, st);
        if (r == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static int pick_default_iface(char* dst, size_t dst_size) {
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f)
        return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[64] = {0};
        if (sscanf(line, " %63[^:]:", name) == 1) {
            if (strcmp(name, "lo") == 0)
                continue;
            strncpy(dst, name, dst_size - 1);
            dst[dst_size - 1] = '\0';
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int select_iface_for_host(const char* host, char* dst, size_t dst_size) {
    struct in_addr target;
    if (inet_pton(AF_INET, host, &target) != 1)
        return -1;

    uint32_t ip = ntohl(target.s_addr);
    uint32_t mask = 0xFFFFFF00u;
    if ((ip & 0x80000000u) == 0) {
        mask = 0xFF000000u;
    } else if ((ip & 0xC0000000u) == 0x80000000u) {
        mask = 0xFFFF0000u;
    }

    struct ifaddrs* ifs = NULL;
    if (getifaddrs(&ifs) != 0)
        return -1;
    int found = -1;
    for (struct ifaddrs* it = ifs; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
            continue;
        if (it->ifa_flags & IFF_LOOPBACK)
            continue;
        struct sockaddr_in* sin = (struct sockaddr_in*)it->ifa_addr;
        uint32_t iface_ip = ntohl(sin->sin_addr.s_addr);
        if ((iface_ip & mask) == (ip & mask)) {
            strncpy(dst, it->ifa_name, dst_size - 1);
            dst[dst_size - 1] = '\0';
            found = 0;
            break;
        }
    }
    freeifaddrs(ifs);
    return found;
}
#endif /* !_WIN32 */

struct frame_limit {
    uint64_t target_frames;
    uint64_t frames_received;
    int stop;
};

static double pixel_as_double(const uint8_t* base,
                              size_t idx,
                              enum stream2_typed_array_tag tag) {
    switch (tag) {
        case STREAM2_TYPED_ARRAY_UINT8:
            return (double)base[idx];
        case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN: {
            uint16_t v;
            memcpy(&v, base + idx * sizeof(uint16_t), sizeof(v));
            return (double)v;
        }
        case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN: {
            uint32_t v;
            memcpy(&v, base + idx * sizeof(uint32_t), sizeof(v));
            return (double)v;
        }
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN: {
            float f;
            memcpy(&f, base + idx * sizeof(float), sizeof(f));
            return (double)f;
        }
        default:
            return 0.0;
    }
}

/* Decompress if needed; always returns a malloc copy. Caller frees *out. */
static int image_to_raw_heap(const struct stream2_buffered_image* bi,
                             unsigned char** out,
                             size_t* out_len,
                             enum stream2_typed_array_tag* out_tag,
                             size_t* out_elem_size) {
    const void* src = bi->data;
    size_t nbytes = bi->data_size;
    void* tmp_dec = NULL;

    if (bi->compression_alg) {
        CompressionAlgorithm algorithm;
        if (strcmp(bi->compression_alg, "bslz4") == 0) {
            algorithm = COMPRESSION_BSLZ4;
        } else if (strcmp(bi->compression_alg, "lz4") == 0) {
            algorithm = COMPRESSION_LZ4;
        } else {
            fprintf(stderr, "unknown compression '%s' for average\n",
                    bi->compression_alg);
            return -1;
        }
        const size_t need = compression_decompress_buffer(
                algorithm, NULL, 0, (const char*)bi->data, bi->data_size,
                bi->compression_elem_size);
        if (need == COMPRESSION_ERROR)
            return -1;
        tmp_dec = malloc(need);
        if (!tmp_dec)
            return -1;
        const size_t got = compression_decompress_buffer(
                algorithm, (char*)tmp_dec, need, (const char*)bi->data,
                bi->data_size, bi->compression_elem_size);
        if (got != need) {
            free(tmp_dec);
            return -1;
        }
        src = tmp_dec;
        nbytes = need;
    }

    unsigned char* copy = malloc(nbytes);
    if (!copy) {
        free(tmp_dec);
        return -1;
    }
    memcpy(copy, src, nbytes);
    free(tmp_dec);

    *out = copy;
    *out_len = nbytes;
    *out_elem_size = bi->elem_size;
    *out_tag = bi->compression_alg && bi->compression_elem_size == 4
                       ? STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN
                       : bi->tag;
    return 0;
}

static enum stream2_typed_array_tag effective_tag(const struct stream2_buffered_image* bi) {
    if (bi->compression_alg && bi->compression_elem_size == 4)
        return STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN;
    return bi->tag;
}

static int same_image_layout(const struct stream2_buffered_image* a,
                             const struct stream2_buffered_image* b) {
    if (a->width != b->width || a->height != b->height)
        return 0;
    if (a->elem_size != b->elem_size)
        return 0;
    const char* ca = a->compression_alg;
    const char* cb = b->compression_alg;
    if (!ca && !cb)
        return effective_tag(a) == effective_tag(b);
    if (!ca || !cb)
        return 0;
    if (strcmp(ca, cb) != 0)
        return 0;
    return a->compression_elem_size == b->compression_elem_size;
}

struct avg_channel_spec {
    int use_null_or_blank; /* match missing / empty channel name from stream */
    char name[128];        /* if !use_null_or_blank, strcasecmp match */
};

static int channel_name_blank(const char* ch) {
    return ch == NULL || ch[0] == '\0';
}

static int channel_matches_avg(const struct stream2_buffered_image* bi,
                               const struct avg_channel_spec* spec) {
    if (spec->use_null_or_blank)
        return channel_name_blank(bi->channel);
    if (channel_name_blank(bi->channel))
        return 0;
    return strcasecmp(bi->channel, spec->name) == 0;
}

static int channel_is_probably_mask(const char* ch) {
    if (channel_name_blank(ch))
        return 0;
    if (strcasecmp(ch, "mask") == 0)
        return 1;
    if (strcasecmp(ch, "pixel_mask") == 0)
        return 1;
    if (strcasecmp(ch, "binary_mask") == 0)
        return 1;
    return 0;
}

static void print_buffer_channel_summary(struct stream2_buffer_ctx* buf) {
    fprintf(stderr, "average: channel names seen in buffer:");
    int any = 0;
    for (size_t i = 0; i < buf->len; i++) {
        const char* ch = buf->items[i].channel;
        int dup = 0;
        for (size_t j = 0; j < i; j++) {
            const char* prev = buf->items[j].channel;
            if (channel_name_blank(ch) && channel_name_blank(prev)) {
                dup = 1;
                break;
            }
            if (ch && prev && strcmp(ch, prev) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        any = 1;
        if (channel_name_blank(ch))
            fprintf(stderr, " (unnamed/empty)");
        else
            fprintf(stderr, " \"%s\"", ch);
    }
    if (!any)
        fprintf(stderr, " (none)");
    fprintf(stderr, "\n");
}

/* Returns 0 on success. On failure prints hint and returns -1. */
static int auto_pick_average_channel(struct stream2_buffer_ctx* buf,
                                     struct avg_channel_spec* out) {
    memset(out, 0, sizeof(*out));
    size_t i;

    for (i = 0; i < buf->len; i++) {
        if (!channel_name_blank(buf->items[i].channel) &&
            strcasecmp(buf->items[i].channel, "image") == 0) {
            strncpy(out->name, buf->items[i].channel, sizeof(out->name) - 1);
            fprintf(stderr,
                    "average: auto-selected channel \"%s\" (preference: image)\n",
                    out->name);
            return 0;
        }
    }
    for (i = 0; i < buf->len; i++) {
        if (!channel_name_blank(buf->items[i].channel) &&
            strcasecmp(buf->items[i].channel, "data") == 0) {
            strncpy(out->name, buf->items[i].channel, sizeof(out->name) - 1);
            fprintf(stderr,
                    "average: auto-selected channel \"%s\" (preference: data)\n",
                    out->name);
            return 0;
        }
    }
    for (i = 0; i < buf->len; i++) {
        if (channel_name_blank(buf->items[i].channel)) {
            out->use_null_or_blank = 1;
            fprintf(stderr,
                    "average: auto-selected unnamed/empty channel (no CBOR name)\n");
            return 0;
        }
    }

    struct {
        char name[128];
        size_t count;
    } tally[AVG_TALLY_MAX];
    size_t nt = 0;
    for (i = 0; i < buf->len; i++) {
        const char* ch = buf->items[i].channel;
        if (channel_name_blank(ch) || channel_is_probably_mask(ch))
            continue;
        size_t k;
        for (k = 0; k < nt; k++) {
            if (strcasecmp(tally[k].name, ch) == 0) {
                tally[k].count++;
                break;
            }
        }
        if (k == nt && nt < AVG_TALLY_MAX) {
            strncpy(tally[k].name, ch, sizeof(tally[k].name) - 1);
            tally[k].name[sizeof(tally[k].name) - 1] = '\0';
            tally[k].count = 1;
            nt++;
        }
    }
    size_t best_c = 0;
    size_t best_i = SIZE_MAX;
    for (i = 0; i < nt; i++) {
        if (tally[i].count > best_c) {
            best_c = tally[i].count;
            best_i = i;
        }
    }
    if (best_i != SIZE_MAX) {
        strncpy(out->name, tally[best_i].name, sizeof(out->name) - 1);
        fprintf(stderr,
                "average: auto-selected channel \"%s\" (most frequent non-mask)\n",
                out->name);
        return 0;
    }

    fprintf(stderr,
            "average: could not auto-select channel for flatfield\n");
    print_buffer_channel_summary(buf);
    return -1;
}

/* Create directory path (and parents); ignores EEXIST. Returns 0 on success. */
static int mkdir_p_inclusive(const char* dir_path) {
    char tmp[512];
    size_t len = strlen(dir_path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, dir_path, len + 1);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';
    for (size_t i = 1; tmp[i]; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (tmp[0] != '\0' && mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int ensure_parent_directory_of_file(const char* filepath) {
    char tmp[512];
    if (strlen(filepath) >= sizeof(tmp))
        return -1;
    strcpy(tmp, filepath);
    char* slash = strrchr(tmp, '/');
    if (!slash)
        return 0;
    if (slash == tmp)
        return 0;
    *slash = '\0';
    if (tmp[0] == '\0')
        return 0;
    return mkdir_p_inclusive(tmp);
}

static const char* avg_spec_label(const struct avg_channel_spec* spec) {
    static char buf[160];
    if (spec->use_null_or_blank)
        return "unnamed/empty";
    snprintf(buf, sizeof(buf), "\"%s\"", spec->name[0] ? spec->name : "?");
    return buf;
}

static int write_average_tiff_for_image_channel(struct stream2_buffer_ctx* buf,
                                                uint64_t series_id,
                                                const struct avg_channel_spec* spec,
                                                const char* flatfield_path_override,
                                                char* flat_path_out,
                                                size_t flat_path_out_sz) {
    if (flat_path_out && flat_path_out_sz)
        flat_path_out[0] = '\0';
    size_t nframes = 0;
    uint64_t w = 0, h = 0;
    enum stream2_typed_array_tag ref_tag = 0;
    size_t ref_elem = 0;
    struct stream2_buffered_image* ref_bi = NULL;

    for (size_t i = 0; i < buf->len; i++) {
        struct stream2_buffered_image* bi = &buf->items[i];
        if (!channel_matches_avg(bi, spec))
            continue;
        if (nframes == 0) {
            w = bi->width;
            h = bi->height;
            ref_tag = effective_tag(bi);
            ref_elem = bi->elem_size;
            ref_bi = bi;
        } else if (!same_image_layout(ref_bi, bi)) {
            fprintf(stderr,
                    "average: inconsistent image geometry or type "
                    "(frame %" PRIu64 ")\n",
                    bi->image_id);
            return -1;
        }
        nframes++;
    }

    if (nframes == 0) {
        fprintf(stderr,
                "average: no frames for selected channel (%s); skip average TIFF\n",
                avg_spec_label(spec));
        print_buffer_channel_summary(buf);
        return 0; /* not an error; flat_path_out stays empty */
    }

    const size_t pixels = (size_t)w * (size_t)h;
    double* acc = calloc(pixels, sizeof(double));
    float* out_f = malloc(pixels * sizeof(float));
    if (!acc || !out_f) {
        free(acc);
        free(out_f);
        return -1;
    }

    for (size_t i = 0; i < buf->len; i++) {
        struct stream2_buffered_image* bi = &buf->items[i];
        if (!channel_matches_avg(bi, spec))
            continue;
        unsigned char* raw = NULL;
        size_t raw_len = 0;
        enum stream2_typed_array_tag tag = 0;
        size_t esz = 0;
        if (image_to_raw_heap(bi, &raw, &raw_len, &tag, &esz) != 0) {
            free(acc);
            free(out_f);
            return -1;
        }
        if (tag != ref_tag || esz != ref_elem || raw_len < pixels * esz) {
            fprintf(stderr, "average: decoded payload mismatch frame %" PRIu64 "\n",
                    bi->image_id);
            free(raw);
            free(acc);
            free(out_f);
            return -1;
        }
        for (size_t p = 0; p < pixels; p++)
            acc[p] += pixel_as_double(raw, p, ref_tag);
        free(raw);
    }

    const double inv = 1.0 / (double)nframes;
    for (size_t p = 0; p < pixels; p++)
        out_f[p] = (float)(acc[p] * inv);

    struct stream2_buffered_image avg = {0};
    avg.channel = strdup("average");
    if (!avg.channel) {
        free(acc);
        free(out_f);
        return -1;
    }
    avg.image_id = 0;
    avg.series_id = series_id;
    avg.width = w;
    avg.height = h;
    avg.tag = STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN;
    avg.elem_size = sizeof(float);
    avg.data = out_f;
    avg.data_size = pixels * sizeof(float);
    avg.owner = NULL;
    avg.compression_alg = NULL;

    char default_path[512];
    const char* write_path;

    if (flatfield_path_override && flatfield_path_override[0] != '\0') {
        if (ensure_parent_directory_of_file(flatfield_path_override) != 0) {
            fprintf(stderr, "average: could not create parent dirs for %s: %s\n",
                    flatfield_path_override, strerror(errno));
            free(avg.channel);
            free(out_f);
            free(acc);
            return -1;
        }
        write_path = flatfield_path_override;
    } else {
        tiff_writer_format_path(default_path, sizeof(default_path), avg.channel,
                                avg.image_id, avg.series_id);
        write_path = default_path;
    }

    int wr = tiff_writer_write(write_path, &avg);
    free(avg.channel);
    free(out_f);
    free(acc);
    if (wr != 0) {
        fprintf(stderr, "average: failed to write %s\n", write_path);
        return -1;
    }
    if (flat_path_out && flat_path_out_sz) {
        strncpy(flat_path_out, write_path, flat_path_out_sz - 1);
        flat_path_out[flat_path_out_sz - 1] = '\0';
    }
    fprintf(stderr, "wrote average TIFF (%zu frames, channel %s): %s\n",
            nframes, avg_spec_label(spec), write_path);
    return 0;
}

/* Pipelined wire buffer + FIFO decode (see main recv loop). */
static struct stream2_buffer_ctx* acquire_pipeline_wire;
static pthread_mutex_t acquire_pipeline_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t acquire_pipeline_cv = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t acquire_pipeline_receive_done;
static volatile sig_atomic_t acquire_pipeline_decode_failed;
static size_t acquire_pipeline_decode_next;
static pthread_mutex_t acquire_progress_mu = PTHREAD_MUTEX_INITIALIZER;
static struct timespec acquire_progress_last;

static void acquire_progress_report(struct stream2_stats* s,
                                    const struct stream2_buffer_ctx* wire,
                                    const struct stream2_buffer_ctx* decoded,
                                    int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&acquire_progress_mu);
    double since_line = stream2_time_diff_sec(&now, &acquire_progress_last);
    if (!force && since_line < 0.2) {
        pthread_mutex_unlock(&acquire_progress_mu);
        return;
    }
    acquire_progress_last = now;
    pthread_mutex_unlock(&acquire_progress_mu);

    size_t wlen = 0;
    size_t dlen = 0;
    pthread_mutex_lock(&acquire_pipeline_mu);
    if (wire)
        wlen = wire->len;
    if (decoded)
        dlen = decoded->len;
    pthread_mutex_unlock(&acquire_pipeline_mu);

    double rate_elapsed = stream2_time_diff_sec(&now, &s->last_report);
    if (rate_elapsed <= 0.0)
        rate_elapsed = 1e-9;
    double gbps = (s->bytes_window * 8.0) / (rate_elapsed * 1e9);
    double rx_gb = s->bytes_total / 1e9;
    double wire_gib =
            wire ? wire->total_bytes / (1024.0 * 1024.0 * 1024.0) : 0.0;
    double wire_cap_gib =
            wire ? wire->bytes_limit / (1024.0 * 1024.0 * 1024.0) : 0.0;
    double dec_gib = decoded ? decoded->total_bytes /
                                       (1024.0 * 1024.0 * 1024.0)
                             : 0.0;
    double dec_cap_gib = decoded ? decoded->bytes_limit /
                                           (1024.0 * 1024.0 * 1024.0)
                                 : 0.0;
    size_t backlog = wlen > dlen ? wlen - dlen : 0;

    fprintf(stderr,
            "\rrecv: %" PRIu64
            " IMAGE msg(s) | fifo decoded: %zu | wire subimgs: %zu | "
            "decode backlog: %zu | net: %.2f Gb/s | rx cum: %.3f GB | "
            "wire %.2f/%.0f GiB | decoded %.2f/%.0f GiB",
            s->images_total, dlen, wlen, backlog, gbps, rx_gb, wire_gib,
            wire_cap_gib, dec_gib, dec_cap_gib);
    if (force)
        fprintf(stderr, "\n");
    fflush(stderr);

    s->bytes_window = 0;
    s->last_report = now;
}

typedef struct {
    struct stream2_buffer_ctx* wire;
    struct stream2_buffer_ctx* decoded;
    struct stream2_stats* stats;
} acquire_decode_pipeline_ctx;

static void* acquire_decode_pipeline_fn(void* arg) {
    acquire_decode_pipeline_ctx* c = (acquire_decode_pipeline_ctx*)arg;
    for (;;) {
        pthread_mutex_lock(&acquire_pipeline_mu);
        while (!acquire_pipeline_decode_failed && !g_stop &&
               acquire_pipeline_decode_next >= c->wire->len &&
               !acquire_pipeline_receive_done) {
            pthread_cond_wait(&acquire_pipeline_cv, &acquire_pipeline_mu);
        }
        if (acquire_pipeline_decode_next >= c->wire->len &&
            acquire_pipeline_receive_done) {
            pthread_mutex_unlock(&acquire_pipeline_mu);
            break;
        }
        if (acquire_pipeline_decode_failed || g_stop) {
            pthread_mutex_unlock(&acquire_pipeline_mu);
            break;
        }
        struct stream2_buffered_image snap =
                c->wire->items[acquire_pipeline_decode_next];
        acquire_pipeline_decode_next++;
        pthread_mutex_unlock(&acquire_pipeline_mu);

        if (stream2_buffer_append_decoded_from_wire(&snap, c->decoded) != 0) {
            acquire_pipeline_decode_failed = 1;
            pthread_mutex_lock(&acquire_pipeline_mu);
            pthread_cond_broadcast(&acquire_pipeline_cv);
            pthread_mutex_unlock(&acquire_pipeline_mu);
            break;
        }
        acquire_progress_report(c->stats, c->wire, c->decoded, 0);
    }
    return NULL;
}

static void handle_msg(struct stream2_msg* msg,
                       size_t msg_size,
                       struct stream2_stats* s,
                       struct stream2_buffer_ctx* buf,
                       struct stream2_msg_owner** owner_slot,
                       zmq_msg_t* src_msg,
                       struct frame_limit* fl) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        stream2_stats_add_image(s, msg_size);
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            if (buf == acquire_pipeline_wire)
                pthread_mutex_lock(&acquire_pipeline_mu);
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                stream2_buffer_image(&d->data, im->image_id, im->series_id,
                                     d->channel, buf, owner_slot, src_msg);
            }
            if (buf == acquire_pipeline_wire) {
                pthread_cond_broadcast(&acquire_pipeline_cv);
                pthread_mutex_unlock(&acquire_pipeline_mu);
            }
            if (fl && fl->target_frames > 0) {
                fl->frames_received++;
                if (fl->frames_received >= fl->target_frames)
                    fl->stop = 1;
            }
        }
    } else {
        stream2_stats_add_bytes(s, msg_size);
    }
}

static enum stream2_result parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stream2_stats* s,
                                     struct stream2_buffer_ctx* buf,
                                     struct stream2_msg_owner** owner_slot,
                                     zmq_msg_t* src_msg,
                                     struct frame_limit* fl) {
    enum stream2_result r;

    struct stream2_msg* msg;
    if ((r = stream2_parse_msg(msg_data, msg_size, &msg))) {
        fprintf(stderr, "error: error %i parsing message\n", (int)r);
        return r;
    }

    handle_msg(msg, msg_size, s, buf, owner_slot, src_msg, fl);

    stream2_free_msg(msg);

    return STREAM2_OK;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "usage: %s HOST --nimages COUNT [options]\n",
            prog);
    fprintf(stderr,
            "  HOST                 DCU host (connects tcp://HOST:31001)\n");
    fprintf(stderr,
            "  --nimages COUNT      stop after this many IMAGE messages\n");
    fprintf(stderr,
            "  (legacy: -n or --images, same meaning as --nimages)\n");
    fprintf(stderr,
            "  --threads M          TIFF writer threads (default 10); decode is pipelined FIFO\n");
    fprintf(stderr,
            "  --generate-flatfield       save per-frame TIFFs and a mean "
            "flatfield (stream2_average_*.tiff)\n");
    fprintf(stderr,
            "  --generate-flatfield-only   flatfield only; skip per-frame "
            "TIFFs (mutually exclusive with --generate-flatfield)\n");
    fprintf(stderr,
            "  --flatfield-file PATH   flatfield TIFF path (with "
            "--generate-flatfield*)\n");
    fprintf(stderr,
            "  --output PATH        TIFF output root for per-frame TIFFs "
            "(default /dev/shm)\n");
}

static void print_storage_paths(uint64_t series_id,
                                int per_frame_saved,
                                int flatfield_ok,
                                const char* flat_path) {
    const char* base = tiff_writer_get_output_base();
    char series_dir[512];
    snprintf(series_dir, sizeof(series_dir), "%s/serie_%06" PRIu64, base,
             series_id);
    if (per_frame_saved)
        printf("Per-frame TIFF data: %s\n", series_dir);
    else
        printf("Per-frame TIFF data: (not saved; --generate-flatfield-only)\n");
    if (flatfield_ok && flat_path && flat_path[0] != '\0')
        printf("Flatfield TIFF:      %s\n", flat_path);
    else
        printf("Flatfield TIFF:      (not written)\n");
}

int main(int argc, char** argv) {
    int num_threads = 10;
    const char* host = NULL;
    uint64_t target_frames = 0;
    int generate_flatfield = 0;
    int generate_flatfield_only = 0;
    const char* flatfield_file = NULL;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    host = argv[1];

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--nimages") == 0 || strcmp(argv[i], "-n") == 0 ||
            strcmp(argv[i], "--images") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s requires a count\n", argv[i]);
                return EXIT_FAILURE;
            }
            char* endp = NULL;
            unsigned long long n = strtoull(argv[i + 1], &endp, 10);
            if (!endp || *endp != '\0' || n == 0) {
                fprintf(stderr, "error: --nimages count must be a positive integer\n");
                return EXIT_FAILURE;
            }
            target_frames = n;
            i++;
        } else if (strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --threads requires a number\n");
                return EXIT_FAILURE;
            }
            char* endp = NULL;
            long t = strtol(argv[i + 1], &endp, 10);
            if (!endp || *endp != '\0' || t <= 0 || t > 255) {
                fprintf(stderr, "error: invalid thread count '%s'\n",
                        argv[i + 1]);
                return EXIT_FAILURE;
            }
            num_threads = (int)t;
            i++;
        } else if (strcmp(argv[i], "--generate-flatfield") == 0) {
            generate_flatfield = 1;
        } else if (strcmp(argv[i], "--generate-flatfield-only") == 0) {
            generate_flatfield_only = 1;
        } else if (strcmp(argv[i], "--flatfield-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "error: --flatfield-file requires a path\n");
                return EXIT_FAILURE;
            }
            flatfield_file = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --output requires a path\n");
                return EXIT_FAILURE;
            }
            tiff_writer_set_output_path(argv[i + 1]);
            i++;
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (target_frames == 0) {
        fprintf(stderr, "error: --nimages <count> is required (legacy: -n or --images)\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (generate_flatfield && generate_flatfield_only) {
        fprintf(stderr,
                "error: use only one of --generate-flatfield and "
                "--generate-flatfield-only\n");
        return EXIT_FAILURE;
    }

    const int want_flatfield = generate_flatfield || generate_flatfield_only;
    const int save_per_frame = !generate_flatfield_only;

    if (flatfield_file && !want_flatfield) {
        fprintf(stderr,
                "error: --flatfield-file requires --generate-flatfield or "
                "--generate-flatfield-only\n");
        return EXIT_FAILURE;
    }

    char address[100];
    sprintf(address, "tcp://%s:31001", host);

#ifndef _WIN32
    char iface[64] = {0};
    const char* env_iface = getenv("STREAM2_NET_IFACE");
    if (env_iface && *env_iface) {
        strncpy(iface, env_iface, sizeof(iface) - 1);
    } else if (select_iface_for_host(host, iface, sizeof(iface)) != 0 &&
               pick_default_iface(iface, sizeof(iface)) != 0) {
        fprintf(stderr, "warn: could not auto-detect network interface\n");
    }

    struct iface_stats net_start = {0};
    struct iface_stats net_end = {0};
    int have_iface_stats = iface[0] != '\0' &&
            read_iface_stats(iface, &net_start) == 0;
    if (have_iface_stats) {
        fprintf(stderr,
                "net iface %s start: rx_drop=%" PRIu64 " rx_err=%" PRIu64
                " rx_frame=%" PRIu64 " tx_drop=%" PRIu64 " tx_err=%" PRIu64
                "\n",
                iface, net_start.rx_drop, net_start.rx_errs, net_start.rx_frame,
                net_start.tx_drop, net_start.tx_errs);
    }
#endif

    void* ctx = zmq_ctx_new();
    void* socket = zmq_socket(ctx, ZMQ_PULL);

    int hwm = 10000;
    zmq_setsockopt(socket, ZMQ_RCVHWM, &hwm, sizeof(hwm));
    int rcvbuf = 16 * 1024 * 1024;
    zmq_setsockopt(socket, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    zmq_connect(socket, address);
    int rcv_timeout_ms = 500;
    zmq_setsockopt(socket, ZMQ_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    stream2_install_signal_handler();

    struct stream2_stats s = {0};
    stream2_stats_init(&s);

    uint64_t wire_limit = stream2_parse_wire_buffer_limit_gb(40);
    uint64_t decoded_limit = stream2_parse_buffer_limit_gb(40);
    struct stream2_buffer_ctx wire_buf;
    struct stream2_buffer_ctx decoded_buf;
    stream2_buffer_init(&wire_buf, wire_limit);
    stream2_buffer_init(&decoded_buf, decoded_limit);

    clock_gettime(CLOCK_MONOTONIC, &acquire_progress_last);
    acquire_pipeline_receive_done = 0;
    acquire_pipeline_decode_failed = 0;
    acquire_pipeline_decode_next = 0;
    acquire_pipeline_wire = &wire_buf;

    acquire_decode_pipeline_ctx dctx = {.wire = &wire_buf,
                                          .decoded = &decoded_buf,
                                          .stats = &s};
    pthread_t decode_tid;
    if (pthread_create(&decode_tid, NULL, acquire_decode_pipeline_fn, &dctx)
        != 0) {
        fprintf(stderr, "error: pthread_create for decode pipeline failed\n");
        acquire_pipeline_wire = NULL;
        stream2_buffer_free(&wire_buf);
        stream2_buffer_free(&decoded_buf);
        zmq_msg_close(&msg);
        zmq_close(socket);
        zmq_ctx_term(ctx);
        return EXIT_FAILURE;
    }

    struct frame_limit fl = {.target_frames = target_frames,
                             .frames_received = 0,
                             .stop = 0};

    for (;;) {
        if (acquire_pipeline_decode_failed)
            break;
        if (g_stop)
            break;
        if (fl.stop)
            break;

        struct stream2_msg_owner* owner_slot = NULL;

        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EAGAIN) {
                acquire_progress_report(&s, &wire_buf, &decoded_buf, 0);
                if (g_stop)
                    break;
                continue;
            }
            if (errno == EINTR && g_stop)
                break;
            if (errno == EINTR)
                continue;
            perror("zmq_msg_recv");
            break;
        }

        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);

        enum stream2_result r;
        if ((r = parse_msg(msg_data, msg_size, &s, &wire_buf, &owner_slot,
                           &msg, &fl)))
            break;

        if (acquire_pipeline_decode_failed)
            break;

        acquire_progress_report(&s, &wire_buf, &decoded_buf, 0);
    }

    acquire_pipeline_receive_done = 1;
    pthread_cond_broadcast(&acquire_pipeline_cv);
    pthread_join(decode_tid, NULL);
    acquire_pipeline_wire = NULL;

    acquire_progress_report(&s, &wire_buf, &decoded_buf, 1);

    if (acquire_pipeline_decode_failed) {
        fprintf(stderr, "error: decode pipeline failed\n");
        stream2_buffer_free(&wire_buf);
        stream2_buffer_free(&decoded_buf);
        zmq_msg_close(&msg);
        zmq_close(socket);
        zmq_ctx_term(ctx);
        return EXIT_FAILURE;
    }
    if (!g_stop && wire_buf.len > 0 && decoded_buf.len != wire_buf.len) {
        fprintf(stderr,
                "error: decoded fifo incomplete (%zu decoded vs %zu wire)\n",
                decoded_buf.len, wire_buf.len);
        stream2_buffer_free(&wire_buf);
        stream2_buffer_free(&decoded_buf);
        zmq_msg_close(&msg);
        zmq_close(socket);
        zmq_ctx_term(ctx);
        return EXIT_FAILURE;
    }

    stream2_buffer_free(&wire_buf);

    if (save_per_frame && decoded_buf.len > 0)
        tiff_writer_flush_buffer_mt(&decoded_buf, num_threads);

    char flat_path[512] = {0};
    int flatfield_ok = 0;
    uint64_t series_id = 0;

    if (decoded_buf.len > 0) {
        series_id = decoded_buf.items[0].series_id;
        for (size_t i = 0; i < decoded_buf.len; i++) {
            if (decoded_buf.items[i].channel &&
                strcasecmp(decoded_buf.items[i].channel, "image") == 0) {
                series_id = decoded_buf.items[i].series_id;
                break;
            }
        }

        if (want_flatfield) {
            struct avg_channel_spec avg_spec;
            memset(&avg_spec, 0, sizeof(avg_spec));
            if (auto_pick_average_channel(&decoded_buf, &avg_spec) == 0) {
                for (size_t i = 0; i < decoded_buf.len; i++) {
                    if (channel_matches_avg(&decoded_buf.items[i], &avg_spec)) {
                        series_id = decoded_buf.items[i].series_id;
                        break;
                    }
                }
                int ar = write_average_tiff_for_image_channel(
                        &decoded_buf, series_id, &avg_spec, flatfield_file,
                        flat_path, sizeof(flat_path));
                if (ar != 0)
                    fprintf(stderr, "warn: flatfield step failed\n");
                else if (flat_path[0] != '\0')
                    flatfield_ok = 1;
            }
        }
        print_storage_paths(series_id, save_per_frame, flatfield_ok,
                             flat_path);
    } else {
        printf("Per-frame TIFF data: (no frames received)\n");
        printf("Flatfield TIFF:      (not written)\n");
    }

#ifndef _WIN32
    if (have_iface_stats) {
        if (read_iface_stats(iface, &net_end) == 0) {
            fprintf(stderr,
                    "net iface %s end:   rx_drop=%" PRIu64 " rx_err=%" PRIu64
                    " rx_frame=%" PRIu64 " tx_drop=%" PRIu64 " tx_err=%" PRIu64
                    "\n",
                    iface, net_end.rx_drop, net_end.rx_errs, net_end.rx_frame,
                    net_end.tx_drop, net_end.tx_errs);
            fprintf(stderr,
                    "net iface %s delta: rx_drop=%" PRIu64 " rx_err=%" PRIu64
                    " rx_frame=%" PRIu64 " tx_drop=%" PRIu64 " tx_err=%" PRIu64
                    "\n",
                    iface,
                    net_end.rx_drop - net_start.rx_drop,
                    net_end.rx_errs - net_start.rx_errs,
                    net_end.rx_frame - net_start.rx_frame,
                    net_end.tx_drop - net_start.tx_drop,
                    net_end.tx_errs - net_start.tx_errs);
        } else {
            fprintf(stderr,
                    "warn: failed to read final network stats for %s\n", iface);
        }
    }
#endif
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&decoded_buf);
    return EXIT_SUCCESS;
}
