#include "server/websocket.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char accept_key[64];
    int rc = websocket_calculate_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept_key, sizeof(accept_key));
    assert(rc == 0);
    assert(strcmp(accept_key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    uint8_t data[4] = {0x1, 0x2, 0x3, 0x4};
    const uint8_t mask[4] = {0x37, 0xFA, 0x21, 0x3D};
    websocket_apply_mask(data, sizeof(data), mask);
    websocket_apply_mask(data, sizeof(data), mask);
    assert(data[0] == 0x1 && data[1] == 0x2 && data[2] == 0x3 && data[3] == 0x4);

    puts("websocket_utils_test passed");
    return 0;
}
