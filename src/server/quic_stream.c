#include "server/quic_stream.h"

#include <stdlib.h>
#include <string.h>

static quic_stream_state_t *find_or_create_stream(quic_stream_manager_t *mgr, uint32_t stream_id);
static void free_segments(quic_stream_segment_t *seg);

void quic_stream_manager_init(quic_stream_manager_t *mgr) {
    if (!mgr) {
        return;
    }
    memset(mgr, 0, sizeof(*mgr));
}

void quic_stream_manager_destroy(quic_stream_manager_t *mgr) {
    if (!mgr) {
        return;
    }
    for (int i = 0; i < QUIC_STREAM_MAX_STREAMS; ++i) {
        if (mgr->streams[i].in_use) {
            free_segments(mgr->streams[i].segments);
            mgr->streams[i].segments = NULL;
            mgr->streams[i].in_use = 0;
        }
    }
}

int quic_stream_reset(quic_stream_manager_t *mgr, uint32_t stream_id) {
    if (!mgr) {
        return -1;
    }
    for (int i = 0; i < QUIC_STREAM_MAX_STREAMS; ++i) {
        if (mgr->streams[i].in_use && mgr->streams[i].stream_id == stream_id) {
            free_segments(mgr->streams[i].segments);
            mgr->streams[i].segments = NULL;
            mgr->streams[i].next_offset = 0;
            return 0;
        }
    }
    return -1;
}

static int insert_segment(quic_stream_state_t *state, uint32_t offset, const uint8_t *data, uint32_t length) {
    quic_stream_segment_t *node = malloc(sizeof(*node));
    if (!node) {
        return -1;
    }
    node->data = malloc(length);
    if (!node->data) {
        free(node);
        return -1;
    }
    memcpy(node->data, data, length);
    node->offset = offset;
    node->length = length;
    node->next = NULL;

    quic_stream_segment_t **cursor = &state->segments;
    while (*cursor && (*cursor)->offset < offset) {
        cursor = &(*cursor)->next;
    }
    node->next = *cursor;
    *cursor = node;
    return 0;
}

static void consume_segments(quic_stream_state_t *state, uint8_t *out_buf, size_t out_buf_size, size_t *out_len) {
    size_t written = 0;
    quic_stream_segment_t *seg = state->segments;

    while (seg && seg->offset <= state->next_offset) {
        uint32_t start = state->next_offset > seg->offset ? state->next_offset - seg->offset : 0;
        if (start >= seg->length) {
            quic_stream_segment_t *tmp = seg;
            seg = seg->next;
            free(tmp->data);
            free(tmp);
            state->segments = seg;
            continue;
        }

        uint32_t remaining = seg->length - start;
        size_t to_copy = remaining;
        if (to_copy > (out_buf_size - written)) {
            to_copy = out_buf_size - written;
        }
        memcpy(out_buf + written, seg->data + start, to_copy);
        written += to_copy;
        state->next_offset += (uint32_t)to_copy;

        if (start + to_copy < seg->length) {
            uint32_t consumed = start + (uint32_t)to_copy;
            memmove(seg->data, seg->data + consumed, seg->length - consumed);
            seg->offset += consumed;
            seg->length -= consumed;
            break;
        }

        quic_stream_segment_t *tmp = seg;
        seg = seg->next;
        free(tmp->data);
        free(tmp);
        state->segments = seg;
    }

    if (out_len) {
        *out_len = written;
    }
}

int quic_stream_on_data(quic_stream_manager_t *mgr,
                        uint32_t stream_id,
                        uint32_t offset,
                        const uint8_t *data,
                        uint32_t length,
                        uint8_t *out_buf,
                        size_t out_buf_size,
                        uint32_t *out_offset,
                        size_t *out_len) {
    if (!mgr || !data || length == 0 || !out_buf || out_buf_size == 0) {
        return -1;
    }

    quic_stream_state_t *state = find_or_create_stream(mgr, stream_id);
    if (!state) {
        return -1;
    }

    if (out_offset) {
        *out_offset = state->next_offset;
    }

    if (insert_segment(state, offset, data, length) != 0) {
        return -1;
    }

    consume_segments(state, out_buf, out_buf_size, out_len);
    return 0;
}

static quic_stream_state_t *find_or_create_stream(quic_stream_manager_t *mgr, uint32_t stream_id) {
    for (int i = 0; i < QUIC_STREAM_MAX_STREAMS; ++i) {
        if (mgr->streams[i].in_use && mgr->streams[i].stream_id == stream_id) {
            return &mgr->streams[i];
        }
    }

    for (int i = 0; i < QUIC_STREAM_MAX_STREAMS; ++i) {
        if (!mgr->streams[i].in_use) {
            mgr->streams[i].in_use = 1;
            mgr->streams[i].stream_id = stream_id;
            mgr->streams[i].next_offset = 0;
            mgr->streams[i].segments = NULL;
            return &mgr->streams[i];
        }
    }
    return NULL;
}

static void free_segments(quic_stream_segment_t *seg) {
    while (seg) {
        quic_stream_segment_t *next = seg->next;
        free(seg->data);
        free(seg);
        seg = next;
    }
}
