# Linked only through the PS5 SDK. Run `make ps5` to generate web assets first.
.DEFAULT_GOAL := all

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK must point to PS5 Payload SDK)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS += -Isrc/core -Isrc/web -Isrc/ps5 -Isrc/drivers/common -Ibuild -DUC_PS5
LDLIBS += -lScePad -lSceUserService -lSceAppInstUtil -ldl

PAYLOAD := dist/unicontroller.elf
BT_DIR := src/drivers/bluetooth
BT_NAMES := host le hci_usb smp_crypto usb_desc evstream
BT_SOURCES := $(addprefix $(BT_DIR)/,$(addsuffix .c,$(BT_NAMES)))
DRIVER_SHARED := src/drivers/protocols/profiles.c src/drivers/protocols/generic.c \
	src/drivers/common/util.c src/drivers/common/crc32.c


SOURCES := \
	src/logging.c \
	vendor/rxi-log/log.c \
	src/drivers/usb.c \
	src/drivers/usb/transport.c \
	src/drivers/usb/protocol.c \
	src/drivers/bt.c \
	src/drivers/config/json.c \
	src/drivers/config/overrides.c \
	$(BT_SOURCES) \
	$(DRIVER_SHARED) \
	src/core/uc_core.c \
	src/core/bt_autostart.c \
	src/ps5/main.c \
	src/ps5/backend.c \
	src/ps5/launcher.c \
	src/web/runtime.c \
	build/assets.c
OBJECTS := $(patsubst %.c,build/ps5/%.o,$(SOURCES))
CONTROLLER_DATA := build/controller_registry.inc
HEADERS := $(shell find src -type f -name '*.h' | sort) vendor/rxi-log/log.h

.PHONY: all
all: $(PAYLOAD)

$(PAYLOAD): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(OBJECTS) $(LDLIBS)

build/ps5/%.o: %.c $(HEADERS) $(CONTROLLER_DATA) build/launcher_icon.inc
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/controller_registry.inc: src/drivers/protocols/controllers.json tools/embed_controllers.py
	python3 tools/embed_controllers.py


build/launcher_icon.inc: assets/branding/icon0.png tools/embed_icon.py
	python3 tools/embed_icon.py
