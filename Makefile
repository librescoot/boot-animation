BINARY_NAME := boot-animation
BUILD_DIR := bin

# ThorVG paths — override via env for cross-compilation / Yocto
THORVG_SRC ?= $(HOME)/src/thorvg
THORVG_INC ?= $(THORVG_SRC)/inc $(THORVG_SRC)/src/bindings/capi
THORVG_BUILD ?= $(THORVG_SRC)/builddir

CC ?= gcc
CFLAGS ?= -O2 -Wall
CFLAGS += $(addprefix -I,$(THORVG_INC))
LDFLAGS ?=
LDFLAGS += -L$(THORVG_BUILD)/src
LIBS := -lthorvg -lstdc++ -lm -lpthread -lasound -lz

CC_ARM := arm-linux-gnueabihf-gcc

.PHONY: build build-arm build-host tools dist clean

build-host: $(BUILD_DIR)/$(BINARY_NAME)
	@echo "Built $(BUILD_DIR)/$(BINARY_NAME) for host"

$(BUILD_DIR)/$(BINARY_NAME): main.c stream.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c $(LIBS)

build: build-arm

build-arm:
	mkdir -p $(BUILD_DIR)
	$(CC_ARM) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) main.c $(LIBS)

# Build-time packer. Always a host binary: it runs during the image build,
# not on the vehicle.
tools: $(BUILD_DIR)/lottie2stream

$(BUILD_DIR)/lottie2stream: tools/lottie2stream.c stream.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/lottie2stream.c $(LIBS)

dist: build
	arm-linux-gnueabihf-strip $(BUILD_DIR)/$(BINARY_NAME)

clean:
	rm -rf $(BUILD_DIR)
