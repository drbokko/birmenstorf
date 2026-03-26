/*
 * stream2_buffer_decode_stack.h — second-stage decode from wire buffer to stack
 */
#ifndef STREAM2_BUFFER_DECODE_STACK_H
#define STREAM2_BUFFER_DECODE_STACK_H

#include "stream2_image_buffer.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Build a malloc-backed stack of uncompressed images from a wire buffer filled
 * by stream2_buffer_image (payloads may be compressed as received).
 * Re-initializes *out (any prior contents should be freed with stream2_buffer_free).
 * Returns 0 on success, -1 on error (out is cleared on failure).
 */
int stream2_buffer_build_decoded_stack(const struct stream2_buffer_ctx* wire,
                                         struct stream2_buffer_ctx* out,
                                         uint64_t decoded_limit);

/*
 * Same as stream2_buffer_build_decoded_stack but decodes images in parallel
 * (POSIX threads when num_threads > 1 and not _WIN32). After decode, total
 * pixel bytes must not exceed decoded_limit or the call fails and frees *out.
 */
int stream2_buffer_build_decoded_stack_mt(const struct stream2_buffer_ctx* wire,
                                          struct stream2_buffer_ctx* out,
                                          uint64_t decoded_limit,
                                          int num_threads);

/*
 * Decode one wire image and append to dst (malloc-backed). Enforces dst->bytes_limit.
 * Used for pipelined receive/decode. Returns 0 on success, -1 on error.
 */
int stream2_buffer_append_decoded_from_wire(const struct stream2_buffered_image* wire_bi,
                                            struct stream2_buffer_ctx* dst);

#if defined(__cplusplus)
}
#endif
#endif /* STREAM2_BUFFER_DECODE_STACK_H */
