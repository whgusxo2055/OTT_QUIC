#include "server/quic_stream.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_in_order(void) {
    quic_stream_manager_t mgr;
    quic_stream_manager_init(&mgr);

    uint8_t out[32];
    size_t out_len = 0;
    uint32_t out_off = 0;

    const uint8_t data[] = {'A', 'B', 'C'};
    assert(quic_stream_on_data(&mgr, 1, 0, data, sizeof(data), out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_off == 0 && out_len == 3);
    assert(memcmp(out, "ABC", 3) == 0);

    quic_stream_manager_destroy(&mgr);
}

static void test_out_of_order(void) {
    quic_stream_manager_t mgr;
    quic_stream_manager_init(&mgr);

    uint8_t out[64];
    size_t out_len = 0;
    uint32_t out_off = 0;

    assert(quic_stream_on_data(&mgr, 2, 3, (const uint8_t *)"DEF", 3, out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_len == 0);
    assert(quic_stream_on_data(&mgr, 2, 0, (const uint8_t *)"ABC", 3, out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_off == 0 && out_len == 6);
    assert(memcmp(out, "ABCDEF", 6) == 0);

    quic_stream_manager_destroy(&mgr);
}

static void test_overlap_and_duplicate(void) {
    quic_stream_manager_t mgr;
    quic_stream_manager_init(&mgr);

    uint8_t out[64];
    size_t out_len = 0;
    uint32_t out_off = 0;

    assert(quic_stream_on_data(&mgr, 3, 0, (const uint8_t *)"Hello", 5, out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_off == 0 && out_len == 5);
    assert(memcmp(out, "Hello", 5) == 0);

    assert(quic_stream_on_data(&mgr, 3, 3, (const uint8_t *)"loWorld", 7, out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_off == 5 && out_len == 5);
    assert(memcmp(out, "World", 5) == 0);

    /* duplicate segment should not emit new data */
    assert(quic_stream_on_data(&mgr, 3, 0, (const uint8_t *)"Hello", 5, out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_len == 0);

    quic_stream_manager_destroy(&mgr);
}

static void test_reset_and_capacity(void) {
    quic_stream_manager_t mgr;
    quic_stream_manager_init(&mgr);

    uint8_t out[64];
    size_t out_len = 0;
    uint32_t out_off = 0;

    assert(quic_stream_on_data(&mgr, 4, 0, (const uint8_t *)"Old", 3, out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_off == 0 && out_len == 3);

    assert(quic_stream_reset(&mgr, 4) == 0);
    assert(quic_stream_on_data(&mgr, 4, 0, (const uint8_t *)"NewData", 7, out, sizeof(out), &out_off, &out_len) == 0);
    assert(out_off == 0 && out_len == 7);
    assert(memcmp(out, "NewData", 7) == 0);

    /* create streams up to capacity */
    for (int i = 5; i < 5 + QUIC_STREAM_MAX_STREAMS - 1; ++i) {
        assert(quic_stream_on_data(&mgr, (uint32_t)i, 0, (const uint8_t *)"X", 1, out, sizeof(out), &out_off, &out_len) == 0);
    }
    /* exceeding capacity should fail */
    assert(quic_stream_on_data(&mgr, 99, 0, (const uint8_t *)"Y", 1, out, sizeof(out), &out_off, &out_len) != 0);

    quic_stream_manager_destroy(&mgr);
}

int main(void) {
    test_in_order();
    test_out_of_order();
    test_overlap_and_duplicate();
    test_reset_and_capacity();
    puts("quic_stream_test passed");
    return 0;
}
