#ifndef SERVER_WEBSOCKET_H
#define SERVER_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int websocket_handle_client(int client_fd);
int websocket_calculate_accept_key(const char *client_key, char *out, size_t out_size);
void websocket_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4]);

#ifdef __cplusplus
}
#endif

#endif // SERVER_WEBSOCKET_H
