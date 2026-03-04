BINARY_NAME := boot-animation
BUILD_DIR := bin
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
LDFLAGS := -ldflags "-w -s -X main.version=$(VERSION)"

# ThorVG paths (adjust for your build)
THORVG_SRC := $(HOME)/src/thorvg
THORVG_INC := $(THORVG_SRC)/inc $(THORVG_SRC)/src/bindings/capi

# For host build, use local thorvg build
THORVG_BUILD ?= $(THORVG_SRC)/builddir

CC_HOST := gcc
CC_ARM := arm-linux-gnueabihf-gcc
CFLAGS := -O2 -Wall $(addprefix -I,$(THORVG_INC))
LIBS := -lthorvg -lstdc++ -lm -lpthread

.PHONY: build build-arm build-host dist clean

build-host: $(BUILD_DIR)/$(BINARY_NAME)
	@echo "Built $(BUILD_DIR)/$(BINARY_NAME) for host"

$(BUILD_DIR)/$(BINARY_NAME): main.c
	mkdir -p $(BUILD_DIR)
	$(CC_HOST) $(CFLAGS) -L$(THORVG_BUILD)/src -o $@ $< $(LIBS)

build: build-arm

build-arm:
	mkdir -p $(BUILD_DIR)
	$(CC_ARM) $(CFLAGS) -L$(THORVG_BUILD)/src -o $(BUILD_DIR)/$(BINARY_NAME) main.c $(LIBS)

dist: build
	arm-linux-gnueabihf-strip $(BUILD_DIR)/$(BINARY_NAME)

clean:
	rm -rf $(BUILD_DIR)
