/*
 * stream2_generic_receiver.c - Unified Stream V2 tools (buffer, buffer-decode, dump, bifurcator).
 *
 * Usage (mode is the first argument, or after --mode):
 *   stream2_generic_receiver buffer <host>
 *   stream2_generic_receiver --mode dump <host>
 *   stream2_generic_receiver buffer-decode <host>
 *   stream2_generic_receiver bifurcator <source_host> <publish_interface> [publish_port]
 *
 * Same behavior and environment variables as the former stand-alone programs.
 * Use --help for a short summary.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#include "stream2_common.h"
#include "stream2_image_buffer.h"
#include "stream2_stats.h"
#include "tiff_writer.h"
#include "stream2.h"
#include "compression/src/compression.h"
#include "tinycbor/src/cbor.h"
#include <zmq.h>

#if defined(__linux__)
#include <sys/resource.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define STREAM2_HAS_CPU_AFFINITY 1
#define STREAM2_HAS_REALTIME_SCHED 1
#define STREAM2_HAS_IFADDRS 1
#elif defined(__APPLE__)
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define STREAM2_HAS_CPU_AFFINITY 0
#define STREAM2_HAS_REALTIME_SCHED 0
#define STREAM2_HAS_IFADDRS 1
#elif !defined(_WIN32)
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define STREAM2_HAS_CPU_AFFINITY 0
#define STREAM2_HAS_REALTIME_SCHED 0
#define STREAM2_HAS_IFADDRS 1
#else
#define STREAM2_HAS_CPU_AFFINITY 0
#define STREAM2_HAS_REALTIME_SCHED 0
#define STREAM2_HAS_IFADDRS 0
#endif

static void wire_handle_msg(struct stream2_msg* msg,
                       size_t msg_size,
                       struct stream2_stats* s,
                       struct stream2_buffer_ctx* buf,
                       struct stream2_msg_owner** owner_slot,
                       zmq_msg_t* src_msg) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        stream2_stats_add_image(s, msg_size);
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                stream2_buffer_image(&d->data, im->image_id, im->series_id,
                                     d->channel, buf, owner_slot, src_msg);
            }
        }
    } else {
        stream2_stats_add_bytes(s, msg_size);
    }
}

static enum stream2_result wire_parse_msg(const uint8_t* msg_data,
                                     size_t msg_size,
                                     struct stream2_stats* s,
                                     struct stream2_buffer_ctx* buf,
                                     struct stream2_msg_owner** owner_slot,
                                     zmq_msg_t* src_msg) {
    enum stream2_result r;

    struct stream2_msg* msg;
    if ((r = stream2_parse_msg(msg_data, msg_size, &msg))) {
        fprintf(stderr, "error: error %i parsing message\n", (int)r);
        return r;
    }

    wire_handle_msg(msg, msg_size, s, buf, owner_slot, src_msg);

    stream2_free_msg(msg);

    return STREAM2_OK;
}

static void report_compression(const struct stream2_buffer_ctx* buf) {
    uint64_t compressed_bytes = 0;
    uint64_t decompressed_bytes = 0;
    uint64_t compressed_images = 0;

    for (size_t i = 0; i < buf->len; i++) {
        const struct stream2_buffered_image* bi = &buf->items[i];
        if (bi->compression_alg == NULL) {
            compressed_bytes += bi->data_size;
            decompressed_bytes += bi->data_size;
            continue;
        }
        CompressionAlgorithm algorithm;
        if (strcmp(bi->compression_alg, "bslz4") == 0) {
            algorithm = COMPRESSION_BSLZ4;
        } else if (strcmp(bi->compression_alg, "lz4") == 0) {
            algorithm = COMPRESSION_LZ4;
        } else {
            fprintf(stderr, "unknown compression '%s' for image %" PRIu64 "\n",
                    bi->compression_alg, bi->image_id);
            continue;
        }
        const size_t out_len = compression_decompress_buffer(
                algorithm, NULL, 0, (const char*)bi->data, bi->data_size,
                bi->compression_elem_size);
        if (out_len == COMPRESSION_ERROR) {
            fprintf(stderr, "decompress size error for image %" PRIu64 "\n",
                    bi->image_id);
            continue;
        }
        void* tmp = malloc(out_len);
        if (!tmp) {
            fprintf(stderr, "OOM decompressing image %" PRIu64 "\n",
                    bi->image_id);
            continue;
        }
        const size_t decoded = compression_decompress_buffer(
                algorithm, (char*)tmp, out_len, (const char*)bi->data,
                bi->data_size, bi->compression_elem_size);
        if (decoded != out_len) {
            fprintf(stderr, "decode mismatch for image %" PRIu64 "\n",
                    bi->image_id);
            free(tmp);
            continue;
        }
        free(tmp);
        compressed_images++;
        compressed_bytes += bi->data_size;
        decompressed_bytes += out_len;
    }

    printf("\nCompression summary:\n");
    printf("  images buffered: %zu\n", buf->len);
    printf("  compressed images: %" PRIu64 "\n", compressed_images);
    printf("  compressed bytes: %.3f GB\n", compressed_bytes / 1e9);
    printf("  decompressed bytes: %.3f GB\n", decompressed_bytes / 1e9);
    if (compressed_bytes > 0) {
        double ratio = (double)decompressed_bytes / (double)compressed_bytes;
        printf("  avg compression ratio: %.3f\n", ratio);
    } else {
        printf("  avg compression ratio: n/a\n");
    }
}

static int run_buffer(const char* host) {
    char address[100];
    sprintf(address, "tcp://%s:31001", host);
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
    uint64_t buffer_limit = stream2_parse_wire_buffer_limit_gb(40);
    struct stream2_buffer_ctx buf;
    stream2_buffer_init(&buf, buffer_limit);
    for (;;) {
        if (g_stop)
            break;
        struct stream2_msg_owner* owner_slot = NULL;
        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EAGAIN) {
                stream2_stats_report(&s, &buf, 0);
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
        if ((r = wire_parse_msg(msg_data, msg_size, &s, &buf, &owner_slot, &msg)))
            break;
        stream2_stats_report(&s, &buf, 0);
    }
    stream2_stats_report(&s, &buf, 1);
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&buf);
    return g_stop ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_buffer_decode(const char* host) {
    char address[100];
    sprintf(address, "tcp://%s:31001", host);
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
    uint64_t buffer_limit = stream2_parse_wire_buffer_limit_gb(40);
    struct stream2_buffer_ctx buf;
    stream2_buffer_init(&buf, buffer_limit);
    for (;;) {
        if (g_stop)
            break;
        struct stream2_msg_owner* owner_slot = NULL;
        int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc == -1) {
            if (errno == EAGAIN) {
                stream2_stats_report(&s, &buf, 0);
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
        if ((r = wire_parse_msg(msg_data, msg_size, &s, &buf, &owner_slot, &msg)))
            break;
        stream2_stats_report(&s, &buf, 0);
    }
    stream2_stats_report(&s, &buf, 1);
    report_compression(&buf);
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&buf);
    return EXIT_SUCCESS;
}

static enum stream2_result decode_bytes(const struct stream2_bytes* bytes,
                                        const unsigned char** decoded,
                                        size_t* decoded_len,
                                        void** decompress_buffer) {
    const struct stream2_compression compression = bytes->compression;

    if (compression.algorithm == NULL) {
        *decoded = (const unsigned char*)bytes->ptr;
        *decoded_len = bytes->len;
        *decompress_buffer = NULL;
        return STREAM2_OK;
    }

    CompressionAlgorithm algorithm;
    if (strcmp(compression.algorithm, "bslz4") == 0) {
        algorithm = COMPRESSION_BSLZ4;
    } else if (strcmp(compression.algorithm, "lz4") == 0) {
        algorithm = COMPRESSION_LZ4;
    } else {
        return STREAM2_ERROR_NOT_IMPLEMENTED;
    }

    const size_t len = compression_decompress_buffer(
            algorithm, NULL, 0, (const char*)bytes->ptr, bytes->len,
            compression.elem_size);
    if (len == COMPRESSION_ERROR)
        return STREAM2_ERROR_DECODE;

    void* buffer = malloc(len);
    if (!buffer)
        return STREAM2_ERROR_OUT_OF_MEMORY;

    if (compression_decompress_buffer(algorithm, (char*)buffer, len,
                                      (const char*)bytes->ptr, bytes->len,
                                      compression.elem_size) != len)
    {
        free(buffer);
        return STREAM2_ERROR_DECODE;
    }

    *decoded = (const unsigned char*)buffer;
    *decoded_len = len;
    *decompress_buffer = buffer;
    return STREAM2_OK;
}

static enum stream2_result decode_typed_array(
        const struct stream2_typed_array* array,
        const unsigned char** data,
        size_t* len,
        size_t* elem_size,
        void** decompress_buffer) {
    enum stream2_result r;

    uint64_t elem_size64;
    if ((r = stream2_typed_array_elem_size(array, &elem_size64)))
        return r;

    if (elem_size64 > SIZE_MAX)
        return STREAM2_ERROR_NOT_IMPLEMENTED;

    *elem_size = elem_size64;

    if ((r = decode_bytes(&array->data, data, len, decompress_buffer)))
        return r;

    *len /= *elem_size;

    return STREAM2_OK;
}

static int print_typed_array_type(const struct stream2_typed_array* array) {
    switch (array->tag) {
        case STREAM2_TYPED_ARRAY_UINT8:
            return printf("uint8 (tag %" PRIu64 ")", array->tag);
        case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN:
            return printf("uint16 (tag %" PRIu64 ")", array->tag);
        case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN:
            return printf("uint32 (tag %" PRIu64 ")", array->tag);
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN:
            return printf("float32 (tag %" PRIu64 ")", array->tag);
        default:
            return printf("tag %" PRIu64, array->tag);
    }
}

static void print_multidim_array(
        const struct stream2_multidim_array* multidim) {
    enum stream2_result r;
    const unsigned char* data;
    size_t len;
    size_t elem_size;
    void* buffer;
    if ((r = decode_typed_array(&multidim->array, &data, &len, &elem_size,
                                &buffer)))
    {
        printf("error %i\n", (int)r);
        return;
    }
    printf("dim [%" PRIu64 " %" PRIu64 "] type ", multidim->dim[0],
           multidim->dim[1]);
    print_typed_array_type(&multidim->array);
    printf("\n");

    uint64_t ce = multidim->dim[1];
    int c_width;
    switch (multidim->array.tag) {
        default:
        case STREAM2_TYPED_ARRAY_UINT8:
            ce = multidim->dim[1] * elem_size;
            c_width = 2;
            break;
        case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN:
            c_width = 4;
            break;
        case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN:
            c_width = 8;
            break;
        case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN:
            c_width = 5;
            break;
    }
    const int COLS = 80;
    const uint64_t c_mid = ((COLS - 3) / (c_width + 1) & ~1) / 2;
    const uint64_t r_mid = 20 / 2;
    for (uint64_t ri = 0, re = multidim->dim[0]; ri < re; ri++, printf("\n")) {
        if (ri == r_mid && re > r_mid * 2) {
            ri = re - r_mid - 1;
            for (uint64_t ci = 0; ci < ce; ci++) {
                if (ci == c_mid && ce > c_mid * 2) {
                    ci = ce - c_mid - 1;
                    printf(":: ");
                    continue;
                }
                printf(":%*s: ", c_width - 2, "");
            }
            continue;
        }
        for (uint64_t ci = 0; ci < ce; ci++) {
            if (ci == c_mid && ce > c_mid * 2) {
                ci = ce - c_mid - 1;
                printf(".. ");
                continue;
            }
            switch (multidim->array.tag) {
                default:
                case STREAM2_TYPED_ARRAY_UINT8:
                    printf("%02" PRIx8 " ", data[ri * ce + ci]);
                    break;
                case STREAM2_TYPED_ARRAY_UINT16_LITTLE_ENDIAN: {
                    uint16_t v;
                    memcpy(&v, data + (ri * ce + ci) * elem_size, sizeof(v));
                    printf("%04" PRIx16 " ", v);
                    break;
                }
                case STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN: {
                    uint32_t v;
                    memcpy(&v, data + (ri * ce + ci) * elem_size, sizeof(v));
                    printf("%08" PRIx32 " ", v);
                    break;
                }
                case STREAM2_TYPED_ARRAY_FLOAT32_LITTLE_ENDIAN: {
                    float v;
                    memcpy(&v, data + (ri * ce + ci) * elem_size, sizeof(v));
                    if (v >= 0.f && v < 10.f)
                        printf("%.3f ", v != 0.f ? v : 0.f);
                    else
                        printf("##### ");
                    break;
                }
            }
        }
    }
    free(buffer);
}

static void print_user_data(struct stream2_user_data* user_data) {
    if (user_data->ptr != NULL) {
        CborParser parser;
        CborValue it;
        CborError e;
        if ((e = cbor_parser_init(user_data->ptr, user_data->len, 0, &parser,
                                  &it)) ||
            (e = cbor_value_to_pretty(stdout, &it)))
        {
            printf("error: %s\n", cbor_error_string(e));
            return;
        }
    }
    printf("\n");
}

static void handle_start_msg(struct stream2_start_msg* msg) {
    printf("\nSTART MESSAGE: series_id %" PRIu64 " series_unique_id %s\n",
           msg->series_id, msg->series_unique_id);
    printf("arm_date: %s\n", msg->arm_date ? msg->arm_date : "");
    printf("beam_center_x: %f\n", msg->beam_center_x);
    printf("beam_center_y: %f\n", msg->beam_center_y);
    printf("channels: [");
    for (size_t i = 0; i < msg->channels.len; i++) {
        if (i > 0)
            printf(" ");
        printf("\"%s\"", msg->channels.ptr[i]);
    }
    printf("]\n");
    printf("count_time: %f\n", msg->count_time);
    printf("countrate_correction_enabled: %s\n",
           msg->countrate_correction_enabled ? "true" : "false");
    {
        enum stream2_result r;
        const unsigned char* array;
        size_t len;
        size_t elem_size;
        void* buffer;
        if (msg->countrate_correction_lookup_table.tag == UINT64_MAX) {
            printf("countrate_correction_lookup_table:\n");
        } else if (msg->countrate_correction_lookup_table.tag !=
                   STREAM2_TYPED_ARRAY_UINT32_LITTLE_ENDIAN)
        {
            printf("countrate_correction_lookup_table: error: unexpected tag "
                   "%" PRIu64 "\n",
                   msg->countrate_correction_lookup_table.tag);
        } else if ((r = decode_typed_array(
                            &msg->countrate_correction_lookup_table, &array,
                            &len, &elem_size, &buffer)))
        {
            printf("countrate_correction_lookup_table: error %i\n", (int)r);
        } else {
            uint32_t cutoff = 0;
            if (len >= 2)
                memcpy(&cutoff, array + (len - 2) * elem_size, sizeof(cutoff));
            printf("countrate_correction_lookup_table: %zu entries cutoff "
                   "%" PRIu32 "\n",
                   len, cutoff);
            free(buffer);
        }
    }
    printf("detector_description: \"%s\"\n",
           msg->detector_description ? msg->detector_description : "");
    printf("detector_serial_number: \"%s\"\n",
           msg->detector_serial_number ? msg->detector_serial_number : "");
    printf("detector_translation: [%f %f %f]\n", msg->detector_translation[0],
           msg->detector_translation[1], msg->detector_translation[2]);
    for (size_t i = 0; i < msg->flatfield.len; i++) {
        struct stream2_flatfield* flatfield = &msg->flatfield.ptr[i];
        printf("flatfield: \"%s\" ", flatfield->channel);
        print_multidim_array(&flatfield->flatfield);
    }
    printf("flatfield_enabled: %s\n",
           msg->flatfield_enabled ? "true" : "false");
    printf("frame_time: %f\n", msg->frame_time);
    printf("goniometer: chi: start %f increment %f\n",
           msg->goniometer.chi.start, msg->goniometer.chi.increment);
    printf("goniometer: kappa: start %f increment %f\n",
           msg->goniometer.kappa.start, msg->goniometer.kappa.increment);
    printf("goniometer: omega: start %f increment %f\n",
           msg->goniometer.omega.start, msg->goniometer.omega.increment);
    printf("goniometer: phi: start %f increment %f\n",
           msg->goniometer.phi.start, msg->goniometer.phi.increment);
    printf("goniometer: two_theta: start %f increment %f\n",
           msg->goniometer.two_theta.start,
           msg->goniometer.two_theta.increment);
    printf("image_dtype: \"%s\"\n", msg->image_dtype ? msg->image_dtype : "");
    printf("image_size_x: %" PRIu64 "\n", msg->image_size_x);
    printf("image_size_y: %" PRIu64 "\n", msg->image_size_y);
    printf("incident_energy: %f\n", msg->incident_energy);
    printf("incident_wavelength: %f\n", msg->incident_wavelength);
    printf("number_of_images: %" PRIu64 "\n", msg->number_of_images);
    for (size_t i = 0; i < msg->pixel_mask.len; i++) {
        struct stream2_pixel_mask* pixel_mask = &msg->pixel_mask.ptr[i];
        printf("pixel_mask: \"%s\" ", pixel_mask->channel);
        print_multidim_array(&pixel_mask->pixel_mask);
    }
    printf("pixel_mask_enabled: %s\n",
           msg->pixel_mask_enabled ? "true" : "false");
    printf("pixel_size_x: %f\n", msg->pixel_size_x);
    printf("pixel_size_y: %f\n", msg->pixel_size_y);
    printf("saturation_value: %" PRIu64 "\n", msg->saturation_value);
    printf("sensor_material: \"%s\"\n",
           msg->sensor_material ? msg->sensor_material : "");
    printf("sensor_thickness: %f\n", msg->sensor_thickness);
    for (size_t i = 0; i < msg->threshold_energy.len; i++) {
        printf("threshold_energy: \"%s\" %f\n",
               msg->threshold_energy.ptr[i].channel,
               msg->threshold_energy.ptr[i].energy);
    }
    printf("user_data: ");
    print_user_data(&msg->user_data);
    printf("virtual_pixel_interpolation_enabled: %s\n",
           msg->virtual_pixel_interpolation_enabled ? "true" : "false");
}

static void handle_image_msg(struct stream2_image_msg* msg) {
    printf("\nIMAGE MESSAGE: series_id %" PRIu64 " series_unique_id %s\n",
           msg->series_id, msg->series_unique_id);
    printf("image_id: %" PRIu64 "\n", msg->image_id);
    printf("real_time: %" PRIu64 "/%" PRIu64 "\n", msg->real_time[0],
           msg->real_time[1]);
    printf("series_date: %s\n", msg->series_date ? msg->series_date : "");
    printf("start_time: %" PRIu64 "/%" PRIu64 "\n", msg->start_time[0],
           msg->start_time[1]);
    printf("stop_time: %" PRIu64 "/%" PRIu64 "\n", msg->stop_time[0],
           msg->stop_time[1]);
    printf("user_data: ");
    print_user_data(&msg->user_data);
    for (size_t i = 0; i < msg->data.len; i++) {
        struct stream2_image_data* data = &msg->data.ptr[i];
        printf("data: \"%s\" ", data->channel);
        print_multidim_array(&data->data);
    }
}

static void handle_end_msg(struct stream2_end_msg* msg) {
    printf("\nEND MESSAGE: series_id %" PRIu64 " series_unique_id %s\n",
           msg->series_id, msg->series_unique_id);
}

static void dump_handle_msg(struct stream2_msg* msg) {
    switch (msg->type) {
        case STREAM2_MSG_START:
            handle_start_msg((struct stream2_start_msg*)msg);
            break;
        case STREAM2_MSG_IMAGE:
            handle_image_msg((struct stream2_image_msg*)msg);
            break;
        case STREAM2_MSG_END:
            handle_end_msg((struct stream2_end_msg*)msg);
            break;
    }
}

static enum stream2_result dump_parse_msg(const uint8_t* msg_data, size_t msg_size) {
    enum stream2_result r;

    struct stream2_msg* msg;
    if ((r = stream2_parse_msg(msg_data, msg_size, &msg))) {
        fprintf(stderr, "error: error %i parsing message\n", (int)r);
        return r;
    }

    dump_handle_msg(msg);

    stream2_free_msg(msg);

    return STREAM2_OK;
}

static int run_dump(const char* host) {
    char address[100];
    sprintf(address, "tcp://%s:31001", host);
    void* ctx = zmq_ctx_new();
    void* socket = zmq_socket(ctx, ZMQ_PULL);
    zmq_connect(socket, address);
    zmq_msg_t msg;
    zmq_msg_init(&msg);
    for (;;) {
        zmq_msg_recv(&msg, socket, 0);
        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);
        size_t msg_size = zmq_msg_size(&msg);
        enum stream2_result r;
        if ((r = dump_parse_msg(msg_data, msg_size)))
            break;
    }
    zmq_msg_close(&msg);
    zmq_close(socket);
    zmq_ctx_term(ctx);
    return EXIT_FAILURE;
}

/* Extended stats for bifurcator */
struct bifurcator_stats {
    struct stream2_stats base;
    uint64_t msgs_forwarded;
    uint64_t bytes_forwarded;
    uint64_t forward_errors;
    uint64_t zero_copy_forwards;
};

static void bifurcator_stats_init(struct bifurcator_stats* s) {
    stream2_stats_init(&s->base);
    s->msgs_forwarded = 0;
    s->bytes_forwarded = 0;
    s->forward_errors = 0;
    s->zero_copy_forwards = 0;
}

static void bifurcator_report(struct bifurcator_stats* s,
                              const struct stream2_buffer_ctx* buf,
                              int force) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = stream2_time_diff_sec(&now, &s->base.last_report);
    if (!force && elapsed < 3.0)
        return;

    if (elapsed <= 0.0)
        elapsed = 1e-9;

    double gbps_in = (s->base.bytes_window * 8.0) / (elapsed * 1e9);
    double gb_total = s->base.bytes_total / 1e9;
    double gb_buffer = buf ? (double)buf->total_bytes / 1e9 : 0.0;
    double gb_cap = buf ? (double)buf->bytes_limit / 1e9 : 0.0;
    size_t buffered = buf ? buf->len : 0;
    double gb_fwd = s->bytes_forwarded / 1e9;

    printf("\rimages: %" PRIu64 "  in: %.2f GB @ %.2f Gbps  "
           "fwd: %" PRIu64 " msgs (all types) / %.2f GB  "
           "buf: %zu / %.1f GB (cap %.0f GB)",
           s->base.images_total, gb_total, gbps_in, s->msgs_forwarded, gb_fwd,
           buffered, gb_buffer, gb_cap);

    if (s->forward_errors > 0)
        printf("  [ERR: %" PRIu64 "]", s->forward_errors);

    if (force)
        printf("\n");
    fflush(stdout);

    s->base.images_window = 0;
    s->base.bytes_window = 0;
    s->base.last_report = now;
}

static void bif_handle_msg(struct stream2_msg* msg,
                       size_t msg_size,
                       struct bifurcator_stats* s,
                       struct stream2_buffer_ctx* buf,
                       struct stream2_msg_owner** owner_slot,
                       zmq_msg_t* src_msg) {
    if (msg->type == STREAM2_MSG_IMAGE) {
        stream2_stats_add_image(&s->base, msg_size);
        if (buf) {
            struct stream2_image_msg* im = (struct stream2_image_msg*)msg;
            for (size_t i = 0; i < im->data.len; i++) {
                struct stream2_image_data* d = &im->data.ptr[i];
                stream2_buffer_image(&d->data, im->image_id, im->series_id,
                                     d->channel, buf, owner_slot, src_msg);
            }
        }
    } else {
        stream2_stats_add_bytes(&s->base, msg_size);
    }
}

static int parse_env_int(const char* name, int default_val) {
    const char* val = getenv(name);
    if (val && *val) {
        char* endp;
        long v = strtol(val, &endp, 10);
        if (endp && *endp == '\0' && v > 0)
            return (int)v;
    }
    return default_val;
}

#if STREAM2_HAS_IFADDRS
/* Resolve interface name to IPv4 address */
static int resolve_interface_ip(const char* ifname, char* ip_out, size_t ip_out_size) {
    struct ifaddrs* ifaddrs_list = NULL;
    struct ifaddrs* ifa = NULL;
    int found = 0;

    if (getifaddrs(&ifaddrs_list) == -1) {
        return 0;
    }

    for (ifa = ifaddrs_list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        if (strcmp(ifa->ifa_name, ifname) != 0)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
            const char* ip_str = inet_ntoa(sin->sin_addr);
            if (ip_str) {
                strncpy(ip_out, ip_str, ip_out_size - 1);
                ip_out[ip_out_size - 1] = '\0';
                found = 1;
                break;
            }
        }
    }

    freeifaddrs(ifaddrs_list);
    return found;
}
#endif

#if STREAM2_HAS_CPU_AFFINITY
static void set_cpu_affinity(int cpu) {
    if (cpu < 0)
        return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        printf("  CPU affinity: pinned to core %d\n", cpu);
    } else {
        fprintf(stderr, "  Warning: could not set CPU affinity to core %d\n",
                cpu);
    }
}
#endif

#if STREAM2_HAS_REALTIME_SCHED
static void set_realtime_priority(void) {
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);

    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
        printf("  Scheduler: SCHED_FIFO (realtime)\n");
    } else {
        /* Try SCHED_RR as fallback */
        param.sched_priority = sched_get_priority_max(SCHED_RR);
        if (sched_setscheduler(0, SCHED_RR, &param) == 0) {
            printf("  Scheduler: SCHED_RR (realtime)\n");
        }
        /* Silently continue with default scheduler if neither works */
    }
}
#endif

static int run_bifurcator(const char* progname, const char* source_host_arg,
                          const char* publish_interface_arg, int publish_port_arg) {
    (void)progname;
    const char* source_host = source_host_arg;
    const char* publish_interface = publish_interface_arg;
    int publish_port = publish_port_arg;

    if (publish_port <= 0 || publish_port > 65535) {
        fprintf(stderr, "error: invalid port %d\n", publish_port);
        return EXIT_FAILURE;
    }

    /* Parse configuration from environment */
    int rcvbuf_mb = parse_env_int("STREAM2_RCVBUF_MB", 256);
    int sndbuf_mb = parse_env_int("STREAM2_SNDBUF_MB", 256);
    int io_threads = parse_env_int("STREAM2_IO_THREADS", 2);
    int busy_poll_us = parse_env_int("STREAM2_BUSY_POLL_US", 0);
    int cpu_affinity = parse_env_int("STREAM2_CPU_AFFINITY", -1);
    int realtime = parse_env_int("STREAM2_REALTIME", 0);

    char source_addr[128];
    char publish_addr[128];
    char publish_ip[64] = {0};
    
    /* Check if publish_interface is an interface name (not an IP) */
#if STREAM2_HAS_IFADDRS
    if (strchr(publish_interface, '.') == NULL && 
        strchr(publish_interface, ':') == NULL) {
        /* Looks like an interface name, try to resolve it */
        if (resolve_interface_ip(publish_interface, publish_ip, sizeof(publish_ip))) {
            printf("  Resolved interface %s -> IP %s\n", publish_interface, publish_ip);
            snprintf(publish_addr, sizeof(publish_addr), "tcp://%s:%d",
                     publish_ip, publish_port);
        } else {
            fprintf(stderr, "warning: could not resolve interface %s to IP, "
                    "using as-is\n", publish_interface);
            snprintf(publish_addr, sizeof(publish_addr), "tcp://%s:%d",
                     publish_interface, publish_port);
        }
    } else
#endif
    {
        /* Assume it's already an IP address */
        snprintf(publish_addr, sizeof(publish_addr), "tcp://%s:%d",
                 publish_interface, publish_port);
    }
    
    snprintf(source_addr, sizeof(source_addr), "tcp://%s:31001", source_host);

    printf("Stream Bifurcator (High-Performance)\n");
    printf("  Source:  %s\n", source_addr);
    printf("  Publish: %s\n", publish_addr);
    printf("Configuration:\n");
    printf("  Receive buffer: %d MB\n", rcvbuf_mb);
    printf("  Send buffer:    %d MB\n", sndbuf_mb);
    printf("  I/O threads:    %d\n", io_threads);
    if (busy_poll_us > 0)
        printf("  Busy polling:   %d us\n", busy_poll_us);

#if STREAM2_HAS_CPU_AFFINITY
    /* Set CPU affinity if requested (Linux only) */
    if (cpu_affinity >= 0)
        set_cpu_affinity(cpu_affinity);
#else
    if (cpu_affinity >= 0)
        fprintf(stderr, "  Note: CPU affinity not supported on this platform\n");
#endif

#if STREAM2_HAS_REALTIME_SCHED
    /* Set realtime priority if requested (Linux only) */
    if (realtime)
        set_realtime_priority();
#else
    if (realtime)
        fprintf(stderr, "  Note: Realtime scheduling not supported on this platform\n");
#endif

    printf("\n");

    /* Create ZMQ context with multiple I/O threads for better throughput */
    void* ctx = zmq_ctx_new();
    zmq_ctx_set(ctx, ZMQ_IO_THREADS, io_threads);

    /* Receiver socket (PULL) */
    void* receiver = zmq_socket(ctx, ZMQ_PULL);

    int hwm = 100000; /* High water mark for high-speed streams */
    zmq_setsockopt(receiver, ZMQ_RCVHWM, &hwm, sizeof(hwm));

    int rcvbuf = rcvbuf_mb * 1024 * 1024;
    zmq_setsockopt(receiver, ZMQ_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int rcv_timeout_ms = 100; /* Shorter timeout for responsiveness */
    zmq_setsockopt(receiver, ZMQ_RCVTIMEO, &rcv_timeout_ms,
                   sizeof(rcv_timeout_ms));

    /* Enable TCP keepalive for long-running connections */
    int tcp_keepalive = 1;
    zmq_setsockopt(receiver, ZMQ_TCP_KEEPALIVE, &tcp_keepalive,
                   sizeof(tcp_keepalive));
    int tcp_keepalive_idle = 30;
    zmq_setsockopt(receiver, ZMQ_TCP_KEEPALIVE_IDLE, &tcp_keepalive_idle,
                   sizeof(tcp_keepalive_idle));

    if (zmq_connect(receiver, source_addr) != 0) {
        fprintf(stderr, "error: cannot connect to %s: %s\n", source_addr,
                zmq_strerror(errno));
        zmq_close(receiver);
        zmq_ctx_term(ctx);
        return EXIT_FAILURE;
    }

    /* Publisher socket (PUSH) for re-broadcasting - matches Stream V2 protocol */
    void* publisher = zmq_socket(ctx, ZMQ_PUSH);

    int sndhwm = 100000;
    zmq_setsockopt(publisher, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));

    int sndbuf = sndbuf_mb * 1024 * 1024;
    zmq_setsockopt(publisher, ZMQ_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* PUSH sockets will block if no PULL sockets are connected.
     * Use a timeout to avoid hanging indefinitely. */
    int snd_timeout_ms = 1000; /* 1 second timeout */
    zmq_setsockopt(publisher, ZMQ_SNDTIMEO, &snd_timeout_ms,
                   sizeof(snd_timeout_ms));

    /* Enable TCP_NODELAY for lower latency (if available) */
#ifdef ZMQ_TCP_NODELAY
    int tcp_nodelay = 1;
    zmq_setsockopt(publisher, ZMQ_TCP_NODELAY, &tcp_nodelay,
                   sizeof(tcp_nodelay));
#endif

    /* Enable TCP keepalive */
    zmq_setsockopt(publisher, ZMQ_TCP_KEEPALIVE, &tcp_keepalive,
                   sizeof(tcp_keepalive));

    if (zmq_bind(publisher, publish_addr) != 0) {
        fprintf(stderr, "error: cannot bind to %s: %s\n", publish_addr,
                zmq_strerror(errno));
        zmq_close(receiver);
        zmq_close(publisher);
        zmq_ctx_term(ctx);
        return EXIT_FAILURE;
    }
    
    /* Get the actual bound endpoint (ZMQ may bind to * or 0.0.0.0) */
    {
        char endpoint[256];
        size_t endpoint_len = sizeof(endpoint);
        if (zmq_getsockopt(publisher, ZMQ_LAST_ENDPOINT, endpoint, &endpoint_len) == 0) {
            printf("  Bound to: %s\n", endpoint);
        }
    }
    
    printf("\nReady to forward Stream V2 messages.\n");
    printf("Downstream clients should use ZMQ_PULL sockets and connect to: %s\n\n",
           publish_addr);

    zmq_msg_t msg;
    zmq_msg_init(&msg);

    stream2_install_signal_handler();

    struct bifurcator_stats stats;
    bifurcator_stats_init(&stats);

    uint64_t buffer_limit = stream2_parse_wire_buffer_limit_gb(40);
    struct stream2_buffer_ctx buf;
    stream2_buffer_init(&buf, buffer_limit);

    printf("Waiting for data... (Ctrl+C to stop)\n");

    /* Main loop with optional busy polling */
    struct timespec poll_start, poll_now;
    int use_busy_poll = (busy_poll_us > 0);

    for (;;) {
        if (g_stop)
            break;

        struct stream2_msg_owner* owner_slot = NULL;

        int rc;

        if (use_busy_poll) {
            /* Busy poll loop for lowest latency */
            clock_gettime(CLOCK_MONOTONIC, &poll_start);
            for (;;) {
                rc = zmq_msg_recv(&msg, receiver, ZMQ_DONTWAIT);
                if (rc >= 0)
                    break; /* Got message */
                if (errno != EAGAIN) {
                    if (errno == EINTR && g_stop)
                        goto done;
                    if (errno == EINTR)
                        continue;
                    perror("zmq_msg_recv");
                    goto done;
                }
                /* Check if we've exceeded busy poll timeout */
                clock_gettime(CLOCK_MONOTONIC, &poll_now);
                long long elapsed_us =
                        stream2_time_diff_ns(&poll_now, &poll_start) / 1000;
                if (elapsed_us >= busy_poll_us) {
                    /* Fall back to blocking receive */
                    rc = zmq_msg_recv(&msg, receiver, 0);
                    break;
                }
                /* Yield to other threads/processes occasionally */
#if defined(_WIN32)
                SwitchToThread();
#else
                sched_yield();
#endif
            }
        } else {
            rc = zmq_msg_recv(&msg, receiver, 0);
        }

        if (rc == -1) {
            if (errno == EAGAIN) {
                bifurcator_report(&stats, &buf, 0);
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

        size_t msg_size = zmq_msg_size(&msg);

        /*
         * Zero-copy forward: Use zmq_msg_send with ZMQ_SNDMORE=0
         * to transfer message ownership. We need to copy for buffering
         * anyway, but we can avoid one copy for the forward path.
         *
         * Strategy: Extract data pointer before any moves, then parse/buffer,
         * then forward. If buffer took ownership, we need to copy from the
         * owner's message data.
         */
        const uint8_t* msg_data = (const uint8_t*)zmq_msg_data(&msg);

        /* Parse and buffer first (needs the data) */
        enum stream2_result r;
        struct stream2_msg* parsed;
        if ((r = stream2_parse_msg(msg_data, msg_size, &parsed))) {
            fprintf(stderr, "error: error %i parsing message\n", (int)r);
            /* Still forward even if we can't parse */
        } else {
            bif_handle_msg(parsed, msg_size, &stats, &buf, &owner_slot, &msg);
            stream2_free_msg(parsed);
        }

        /*
         * Forward the message. If the buffer took ownership via zero-copy,
         * we need to make a copy from the owner's message. Otherwise we can
         * potentially forward with zero-copy.
         */
        int fwd_rc;
        if (owner_slot != NULL) {
            /* Buffer took ownership (one or more channels used zero-copy),
             * need to copy for forward. Get data from owner's message since
             * original msg was moved. */
            zmq_msg_t fwd_msg;
            zmq_msg_init_size(&fwd_msg, msg_size);
            const uint8_t* owner_data = (const uint8_t*)zmq_msg_data(&owner_slot->msg);
            memcpy(zmq_msg_data(&fwd_msg), owner_data, msg_size);
            fwd_rc = zmq_msg_send(&fwd_msg, publisher, 0);
            if (fwd_rc == -1) {
                zmq_msg_close(&fwd_msg);
                /* EAGAIN on PUSH socket means no PULL sockets connected or buffer full */
                if (errno == EAGAIN) {
                    static int warned_no_receivers = 0;
                    if (!warned_no_receivers) {
                        fprintf(stderr,
                                "\nWarning: PUSH socket cannot send - "
                                "no PULL sockets connected or send buffer full!\n"
                                "Make sure downstream clients (e.g., DectrisStream2Receiver_linux) "
                                "are connected with ZMQ_PULL sockets.\n");
                        warned_no_receivers = 1;
                    }
                }
            }
            /* Reinitialize msg for next receive (original was moved to owner_slot) */
            zmq_msg_init(&msg);
        } else {
            /* Try zero-copy forward by moving the message */
            zmq_msg_t fwd_msg;
            zmq_msg_init(&fwd_msg);
            zmq_msg_move(&fwd_msg, &msg);
            fwd_rc = zmq_msg_send(&fwd_msg, publisher, 0);
            if (fwd_rc == -1) {
                zmq_msg_close(&fwd_msg);
                /* EAGAIN on PUSH socket means no PULL sockets connected or buffer full */
                if (errno == EAGAIN) {
                    static int warned_no_receivers = 0;
                    if (!warned_no_receivers) {
                        fprintf(stderr,
                                "\nWarning: PUSH socket cannot send - "
                                "no PULL sockets connected or send buffer full!\n"
                                "Make sure downstream clients (e.g., DectrisStream2Receiver_linux) "
                                "are connected with ZMQ_PULL sockets.\n");
                        warned_no_receivers = 1;
                    }
                }
            } else {
                stats.zero_copy_forwards++;
            }
            /* Reinitialize msg for next receive */
            zmq_msg_init(&msg);
        }

        if (fwd_rc == -1) {
            if (errno != EAGAIN) {
                stats.forward_errors++;
                fprintf(stderr, "error: zmq_msg_send failed: %s\n",
                        zmq_strerror(errno));
            }
        } else {
            stats.msgs_forwarded++;
            stats.bytes_forwarded += msg_size;
        }

        bifurcator_report(&stats, &buf, 0);
    }

done:
    bifurcator_report(&stats, &buf, 1);

    printf("\nSummary:\n");
    printf("  Total received:  %" PRIu64 " images, %.3f GB\n",
           stats.base.images_total, stats.base.bytes_total / 1e9);
    printf("  Total forwarded: %" PRIu64 " msgs (start+images+end), %.3f GB\n",
           stats.msgs_forwarded, stats.bytes_forwarded / 1e9);
    printf("  Zero-copy fwds:  %" PRIu64 "\n", stats.zero_copy_forwards);
    printf("  Forward errors:  %" PRIu64 "\n", stats.forward_errors);
    printf("  Buffered:        %zu images, %.3f GB\n", buf.len,
           (double)buf.total_bytes / 1e9);

    /* Save buffered images to TIFF files before cleanup */
    if (buf.len > 0) {
        int tiff_threads = parse_env_int("STREAM2_TIFF_THREADS", 10);
        printf("\nSaving %zu buffered images to TIFF files...\n", buf.len);
        tiff_writer_flush_buffer_mt(&buf, tiff_threads);
        printf("Done saving TIFF files.\n");
    }

    zmq_msg_close(&msg);
    zmq_close(receiver);
    zmq_close(publisher);
    zmq_ctx_term(ctx);
    stream2_buffer_free(&buf);

    return EXIT_SUCCESS;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "usage: %s [ --mode <name> ] <arguments...>\n"
            "       (mode may be the first argument instead of --mode)\n"
            "modes:\n"
            "  buffer <host>                    buffer wire payloads on tcp://host:31001, print stats\n"
            "  buffer-decode <host>             same, plus compression summary at exit\n"
            "  dump <host>                      decode and print messages to stdout\n"
            "  bifurcator <src> <pub_iface> [port]  relay stream (default publish port 31002)\n",
            prog);
}

int main(int argc, char** argv) {
    int mi = 1;
    const char* mode = NULL;

    if (argc >= 3 && strcmp(argv[1], "--mode") == 0) {
        mode = argv[2];
        mi = 3;
    } else if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    } else if (argc >= 2) {
        mode = argv[1];
        mi = 2;
    } else {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(mode, "buffer") == 0) {
        if (argc != mi + 1) {
            fprintf(stderr, "usage: %s [ --mode ] buffer <host>\n", argv[0]);
            return EXIT_FAILURE;
        }
        return run_buffer(argv[mi]);
    }
    if (strcmp(mode, "buffer-decode") == 0) {
        if (argc != mi + 1) {
            fprintf(stderr, "usage: %s [ --mode ] buffer-decode <host>\n", argv[0]);
            return EXIT_FAILURE;
        }
        return run_buffer_decode(argv[mi]);
    }
    if (strcmp(mode, "dump") == 0) {
        if (argc != mi + 1) {
            fprintf(stderr, "usage: %s [ --mode ] dump <host>\n", argv[0]);
            return EXIT_FAILURE;
        }
        return run_dump(argv[mi]);
    }
    if (strcmp(mode, "bifurcator") == 0) {
        if (argc < mi + 2 || argc > mi + 3) {
            fprintf(stderr,
                    "usage: %s [ --mode ] bifurcator <source_host> <publish_interface> "
                    "[publish_port]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
        int port = (argc > mi + 2) ? atoi(argv[mi + 2]) : 31002;
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "error: invalid port %d\n", port);
            return EXIT_FAILURE;
        }
        return run_bifurcator(argv[0], argv[mi], argv[mi + 1], port);
    }
    fprintf(stderr, "error: unknown mode \"%s\"\n", mode);
    print_usage(argv[0]);
    return EXIT_FAILURE;
}
