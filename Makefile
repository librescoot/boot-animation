BINARY_NAME := boot-animation
BUILD_DIR := bin

# ThorVG paths — override via env for cross-compilation / Yocto
THORVG_SRC ?= $(HOME)/src/thorvg
THORVG_INC ?= $(THORVG_SRC)/inc $(THORVG_SRC)/src/bindings/capi
THORVG_BUILD ?= $(THORVG_SRC)/builddir

CC ?= gcc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall
LDFLAGS ?=
THORVG_CPPFLAGS := $(foreach directory,$(THORVG_INC),-isystem $(directory))
THORVG_LDFLAGS := -L$(THORVG_BUILD)/src
LIBS := -lthorvg -lstdc++ -lm -lpthread -lasound -lz
COMMON_SOURCES := render_utils.c signal_utils.c stream_format.c
PLAYER_SOURCES := audio_playback.c $(COMMON_SOURCES)
TEST_BINARY := $(BUILD_DIR)/test-render-utils
STREAM_CHECK_BINARY := $(BUILD_DIR)/check-stream

CC_ARM := arm-linux-gnueabihf-gcc

.PHONY: build build-arm build-host tools test test-packer dist clean FORCE

build-host: $(BUILD_DIR)/$(BINARY_NAME)
	@echo "Built $(BUILD_DIR)/$(BINARY_NAME) for host"

$(BUILD_DIR)/$(BINARY_NAME): main.c audio_playback.h render_utils.h signal_utils.h stream.h $(COMMON_SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(THORVG_CPPFLAGS) $(CFLAGS) -o $@ main.c $(PLAYER_SOURCES) $(LDFLAGS) $(THORVG_LDFLAGS) $(LIBS)

build: build-arm

build-arm:
	mkdir -p $(BUILD_DIR)
	$(CC_ARM) $(CPPFLAGS) $(THORVG_CPPFLAGS) $(CFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) main.c $(PLAYER_SOURCES) $(LDFLAGS) $(THORVG_LDFLAGS) $(LIBS)

# Build-time packer. Always a host binary: it runs during the image build,
# not on the vehicle.
tools: $(BUILD_DIR)/lottie2stream

$(BUILD_DIR)/lottie2stream: tools/lottie2stream.c stream.h stream_format.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(THORVG_CPPFLAGS) $(CFLAGS) -o $@ \
		tools/lottie2stream.c stream_format.c $(LDFLAGS) $(THORVG_LDFLAGS) $(LIBS)

test: $(TEST_BINARY)
	$(TEST_BINARY)

$(TEST_BINARY): tests/test_render_utils.c render_utils.h signal_utils.h stream.h $(COMMON_SOURCES) FORCE
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -D_POSIX_C_SOURCE=200809L $(CFLAGS) \
		-std=c11 -Wall -Wextra -Werror -I. -o $@ \
		tests/test_render_utils.c $(COMMON_SOURCES) $(LDFLAGS)

$(STREAM_CHECK_BINARY): tests/check_stream.c stream.h stream_format.c FORCE
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -I. \
		-o $@ tests/check_stream.c stream_format.c $(LDFLAGS) -lz

test-packer: $(BUILD_DIR)/lottie2stream $(STREAM_CHECK_BINARY)
	tests/test_lottie2stream.sh $(BUILD_DIR)/lottie2stream $(STREAM_CHECK_BINARY)

FORCE:

dist: build
	arm-linux-gnueabihf-strip $(BUILD_DIR)/$(BINARY_NAME)

clean:
	rm -rf $(BUILD_DIR)
