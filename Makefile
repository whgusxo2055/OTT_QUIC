CC := gcc
CFLAGS := -Wall -Wextra -Werror -std=c11 -pedantic -pthread
CPPFLAGS := -Isrc
LDFLAGS :=
LDLIBS := -lsqlite3 -pthread

BUILD_DIR := build
TARGET := $(BUILD_DIR)/ott_server

SRC_FILES := $(shell find src -name '*.c')
OBJ_FILES := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC_FILES))
MAIN_OBJ := $(BUILD_DIR)/main.o
LIB_OBJ_FILES := $(filter-out $(MAIN_OBJ), $(OBJ_FILES))

TEST_BINS := \
	$(BUILD_DIR)/tests/db_test \
	$(BUILD_DIR)/tests/server_test \
	$(BUILD_DIR)/tests/websocket_utils_test \
	$(BUILD_DIR)/tests/quic_packet_test \
	$(BUILD_DIR)/tests/quic_engine_test

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJ_FILES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/db_test: tests/db_test.c $(LIB_OBJ_FILES)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/tests/server_test: tests/server_test.c $(LIB_OBJ_FILES)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/tests/websocket_utils_test: tests/websocket_utils_test.c $(LIB_OBJ_FILES)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/tests/quic_packet_test: tests/quic_packet_test.c $(LIB_OBJ_FILES)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/tests/quic_engine_test: tests/quic_engine_test.c $(LIB_OBJ_FILES)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	$(TARGET)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo \"Running $$t\"; \
		$$t; \
	done

clean:
	rm -rf $(BUILD_DIR)
