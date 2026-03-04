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
LIBS := -lthorvg -lstdc++ -lm -lpthread

CC_ARM := arm-linux-gnueabihf-gcc

.PHONY: build build-arm build-host dist clean

build-host: $(BUILD_DIR)/$(BINARY_NAME)
	@echo "Built $(BUILD_DIR)/$(BINARY_NAME) for host"

$(BUILD_DIR)/$(BINARY_NAME): main.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

build: build-arm

build-arm:
	mkdir -p $(BUILD_DIR)
	$(CC_ARM) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/$(BINARY_NAME) main.c $(LIBS)

dist: build
	arm-linux-gnueabihf-strip $(BUILD_DIR)/$(BINARY_NAME)

clean:
	rm -rf $(BUILD_DIR)
