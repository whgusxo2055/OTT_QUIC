#ifndef SERVER_QUIC_STREAM_H
#define SERVER_QUIC_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIC_STREAM_MAX_STREAMS 16

typedef struct quic_stream_segment {
    uint32_t offset;
    uint32_t length;
    uint8_t *data;
    struct quic_stream_segment *next;
} quic_stream_segment_t;

typedef struct {
    uint32_t stream_id;
    uint32_t next_offset;
    quic_stream_segment_t *segments;
    int in_use;
} quic_stream_state_t;

typedef struct {
    quic_stream_state_t streams[QUIC_STREAM_MAX_STREAMS];
} quic_stream_manager_t;

void quic_stream_manager_init(quic_stream_manager_t *mgr);
void quic_stream_manager_destroy(quic_stream_manager_t *mgr);
int quic_stream_reset(quic_stream_manager_t *mgr, uint32_t stream_id);

int quic_stream_on_data(quic_stream_manager_t *mgr,
                        uint32_t stream_id,
                        uint32_t offset,
                        const uint8_t *data,
                        uint32_t length,
                        uint8_t *out_buf,
                        size_t out_buf_size,
                        uint32_t *out_offset,
                        size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif // SERVER_QUIC_STREAM_H
