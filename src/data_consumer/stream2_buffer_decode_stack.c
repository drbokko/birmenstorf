/*
 * stream2_buffer_decode_stack.c — decompress wire buffer into a decoded image stack
 */
#ifdef __linux__
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "stream2_buffer_decode_stack.h"
#include "compression/src/compression.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#endif

static void clear_decoded_partial(struct stream2_buffer_ctx* out, size_t n_valid) {
    for (size_t i = 0; i < n_valid; i++) {
        free(out->items[i].channel);
        free((void*)out->items[i].data);
        out->items[i].channel = NULL;
        out->items[i].data = NULL;
    }
    free(out->items);
    out->items = NULL;
    out->len = out->cap = 0;
    out->total_bytes = 0;
}

static int decode_wire_image_to_decoded(const struct stream2_buffered_image* src,
                                        struct stream2_buffered_image* dst) {
    memset(dst, 0, sizeof(*dst));
    dst->channel = src->channel ? strdup(src->channel) : NULL;
    if (src->channel && !dst->channel)
        return -1;
    dst->image_id = src->image_id;
    dst->series_id = src->series_id;
    dst->width = src->width;
    dst->height = src->height;
    dst->owner = NULL;
    dst->compression_alg = NULL;
    dst->compression_elem_size = 0;

    if (src->compression_alg) {
        CompressionAlgorithm algorithm;
        if (strcmp(src->compression_alg, "bslz4") == 0) {
            algorithm = COMPRESSION_BSLZ4;
        } else if (strcmp(src->compression_alg, "lz4") == 0) {
            algorithm = COMPRESSION_LZ4;
        } else {
            fprintf(stderr,
                    "decode stack: unknown compression '%s' for image %" PRIu64
                    "\n",
                    src->compression_alg, src->image_id);
            free(dst->channel);
            dst->channel = NULL;
            return -1;
        }
        const size_t out_len = compression_decompress_buffer(
                algorithm, NULL, 0, (const char*)src->data, src->data_size,
                src->compression_elem_size);
        if (out_len == COMPRESSION_ERROR) {
            fprintf(stderr,
                    "decode stack: decompress size query failed image %" PRIu64
                    "\n",
                    src->image_id);
            free(dst->channel);
            dst->channel = NULL;
            return -1;
        }
        void* tmp = malloc(out_len);
        if (!tmp) {
            free(dst->channel);
            dst->channel = NULL;
            return -1;
        }
        const size_t got = compression_decompress_buffer(
                algorithm, (char*)tmp, out_len, (const char*)src->data,
                src->data_size, src->compression_elem_size);
        if (got != out_len) {
            fprintf(stderr,
                    "decode stack: decode mismatch image %" PRIu64 "\n",
                    src->image_id);
            free(tmp);
            free(dst->channel);
            dst->channel = NULL;
            return -1;
        }
        dst->data = tmp;
        dst->data_size = out_len;
        dst->tag = src->compression_elem_size == 4
                           ? STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN
                           : src->tag;
        dst->elem_size = src->elem_size;
    } else {
        void* copy = malloc(src->data_size);
        if (!copy) {
            free(dst->channel);
            dst->channel = NULL;
            return -1;
        }
        memcpy(copy, src->data, src->data_size);
        dst->data = copy;
        dst->data_size = src->data_size;
        dst->tag = src->tag;
        dst->elem_size = src->elem_size;
    }
    return 0;
}

static size_t decoded_size_upper_bound(const struct stream2_buffered_image* s) {
    if (!s->compression_alg)
        return s->data_size;
    CompressionAlgorithm algorithm;
    if (strcmp(s->compression_alg, "bslz4") == 0) {
        algorithm = COMPRESSION_BSLZ4;
    } else if (strcmp(s->compression_alg, "lz4") == 0) {
        algorithm = COMPRESSION_LZ4;
    } else {
        return SIZE_MAX;
    }
    const size_t out_len = compression_decompress_buffer(
            algorithm, NULL, 0, (const char*)s->data, s->data_size,
            s->compression_elem_size);
    if (out_len == COMPRESSION_ERROR)
        return SIZE_MAX;
    return out_len;
}

static int ensure_decoded_capacity(struct stream2_buffer_ctx* buf) {
    if (buf->len < buf->cap)
        return 0;
    size_t new_cap = buf->cap ? buf->cap * 2 : 64;
    void* p = realloc(buf->items, new_cap * sizeof(*buf->items));
    if (!p)
        return -1;
    buf->items = p;
    buf->cap = new_cap;
    return 0;
}

int stream2_buffer_append_decoded_from_wire(const struct stream2_buffered_image* wire_bi,
                                              struct stream2_buffer_ctx* dst) {
    size_t ub = decoded_size_upper_bound(wire_bi);
    if (ub == SIZE_MAX)
        return -1;
    if (dst->total_bytes + (uint64_t)ub > dst->bytes_limit) {
        fprintf(stderr,
                "decode stack: append would exceed decoded limit (image %" PRIu64
                ")\n",
                wire_bi->image_id);
        return -1;
    }
    if (ensure_decoded_capacity(dst) != 0)
        return -1;
    struct stream2_buffered_image* slot = &dst->items[dst->len];
    if (decode_wire_image_to_decoded(wire_bi, slot) != 0)
        return -1;
    dst->total_bytes += slot->data_size;
    dst->len++;
    return 0;
}

int stream2_buffer_build_decoded_stack(const struct stream2_buffer_ctx* wire,
                                         struct stream2_buffer_ctx* out,
                                         uint64_t decoded_limit) {
    stream2_buffer_free(out);
    stream2_buffer_init(out, decoded_limit);

    if (wire->len == 0)
        return 0;

    uint64_t sum_bound = 0;
    for (size_t i = 0; i < wire->len; i++) {
        size_t ub = decoded_size_upper_bound(&wire->items[i]);
        if (ub == SIZE_MAX) {
            fprintf(stderr, "decode stack: cannot size image %" PRIu64 "\n",
                    wire->items[i].image_id);
            return -1;
        }
        sum_bound += (uint64_t)ub;
        if (sum_bound > decoded_limit) {
            fprintf(stderr,
                    "decode stack: decoded byte estimate exceeds limit "
                    "(%.2f GiB)\n",
                    decoded_limit / (1024.0 * 1024.0 * 1024.0));
            return -1;
        }
    }

    out->items = calloc(wire->len, sizeof(*out->items));
    if (!out->items)
        return -1;
    out->cap = wire->len;

    for (size_t i = 0; i < wire->len; i++) {
        if (decode_wire_image_to_decoded(&wire->items[i], &out->items[i]) != 0) {
            clear_decoded_partial(out, i);
            return -1;
        }
        out->len = i + 1;
        out->total_bytes += out->items[i].data_size;
    }
    return 0;
}

#ifndef _WIN32

struct decode_mt_ctx {
    const struct stream2_buffer_ctx* wire;
    struct stream2_buffered_image* dst_items;
    pthread_mutex_t mu;
    size_t next_idx;
    int err;
};

static void* decode_worker_mt(void* arg) {
    struct decode_mt_ctx* ctx = (struct decode_mt_ctx*)arg;
    for (;;) {
        pthread_mutex_lock(&ctx->mu);
        if (ctx->err) {
            pthread_mutex_unlock(&ctx->mu);
            break;
        }
        size_t i = ctx->next_idx++;
        pthread_mutex_unlock(&ctx->mu);
        if (i >= ctx->wire->len)
            break;
        if (decode_wire_image_to_decoded(&ctx->wire->items[i], &ctx->dst_items[i])
            != 0) {
            pthread_mutex_lock(&ctx->mu);
            ctx->err = 1;
            pthread_mutex_unlock(&ctx->mu);
            break;
        }
    }
    return NULL;
}
#endif /* !_WIN32 */

int stream2_buffer_build_decoded_stack_mt(const struct stream2_buffer_ctx* wire,
                                          struct stream2_buffer_ctx* out,
                                          uint64_t decoded_limit,
                                          int num_threads) {
    if (num_threads <= 1 || wire->len <= 1)
        return stream2_buffer_build_decoded_stack(wire, out, decoded_limit);

#ifndef _WIN32
    stream2_buffer_free(out);
    stream2_buffer_init(out, decoded_limit);

    if (wire->len == 0)
        return 0;

    uint64_t sum_bound = 0;
    for (size_t i = 0; i < wire->len; i++) {
        size_t ub = decoded_size_upper_bound(&wire->items[i]);
        if (ub == SIZE_MAX) {
            fprintf(stderr, "decode stack: cannot size image %" PRIu64 "\n",
                    wire->items[i].image_id);
            return -1;
        }
        sum_bound += (uint64_t)ub;
        if (sum_bound > decoded_limit) {
            fprintf(stderr,
                    "decode stack: decoded byte estimate exceeds limit "
                    "(%.2f GiB)\n",
                    decoded_limit / (1024.0 * 1024.0 * 1024.0));
            return -1;
        }
    }

    struct stream2_buffered_image* dst =
            calloc(wire->len, sizeof(*dst));
    if (!dst)
        return -1;

    int threads = num_threads;
    if (threads > (int)wire->len)
        threads = (int)wire->len;
    if (threads > 256)
        threads = 256;

    struct decode_mt_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.wire = wire;
    ctx.dst_items = dst;
    ctx.next_idx = 0;
    ctx.err = 0;
    pthread_mutex_init(&ctx.mu, NULL);

    pthread_t* tids = calloc((size_t)threads, sizeof(pthread_t));
    if (!tids) {
        pthread_mutex_destroy(&ctx.mu);
        free(dst);
        return -1;
    }

    for (int t = 0; t < threads; t++) {
        if (pthread_create(&tids[t], NULL, decode_worker_mt, &ctx) != 0) {
            pthread_mutex_lock(&ctx.mu);
            ctx.err = 1;
            pthread_mutex_unlock(&ctx.mu);
            for (int j = 0; j < t; j++)
                pthread_join(tids[j], NULL);
            free(tids);
            pthread_mutex_destroy(&ctx.mu);
            for (size_t i = 0; i < wire->len; i++) {
                free(dst[i].channel);
                free((void*)dst[i].data);
            }
            free(dst);
            fprintf(stderr, "decode stack: pthread_create failed\n");
            return -1;
        }
    }
    for (int t = 0; t < threads; t++)
        pthread_join(tids[t], NULL);
    free(tids);

    int failed = ctx.err;
    pthread_mutex_destroy(&ctx.mu);

    if (failed) {
        for (size_t i = 0; i < wire->len; i++) {
            free(dst[i].channel);
            free((void*)dst[i].data);
        }
        free(dst);
        return -1;
    }

    uint64_t total = 0;
    for (size_t i = 0; i < wire->len; i++)
        total += dst[i].data_size;
    if (total > decoded_limit) {
        fprintf(stderr, "decode stack: decoded total exceeds limit\n");
        for (size_t i = 0; i < wire->len; i++) {
            free(dst[i].channel);
            free((void*)dst[i].data);
        }
        free(dst);
        return -1;
    }

    out->items = dst;
    out->len = wire->len;
    out->cap = wire->len;
    out->total_bytes = total;
    return 0;
#else
    (void)num_threads;
    return stream2_buffer_build_decoded_stack(wire, out, decoded_limit);
#endif
}
