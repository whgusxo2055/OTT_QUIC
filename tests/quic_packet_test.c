#include "server/quic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    quic_packet_t packet = {
        .flags = QUIC_FLAG_INITIAL,
        .connection_id = 0x1122334455667788ULL,
        .packet_number = 42,
        .stream_id = 1,
        .offset = 0,
        .length = sizeof(payload),
        .payload = payload,
    };

    uint8_t buffer[QUIC_MAX_PACKET_SIZE];
    size_t serialized = 0;
    assert(quic_packet_serialize(&packet, buffer, sizeof(buffer), &serialized) == 0);
    assert(serialized == QUIC_HEADER_SIZE + sizeof(payload));

    quic_packet_t parsed;
    assert(quic_packet_deserialize(&parsed, buffer, serialized) == 0);
    assert(parsed.flags == packet.flags);
    assert(parsed.connection_id == packet.connection_id);
    assert(parsed.packet_number == packet.packet_number);
    assert(parsed.stream_id == packet.stream_id);
    assert(parsed.offset == packet.offset);
    assert(parsed.length == packet.length);
    assert(memcmp(parsed.payload, payload, packet.length) == 0);

    printf("quic_packet_test passed\n");
    return 0;
}
