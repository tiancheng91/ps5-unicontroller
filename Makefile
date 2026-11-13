# Host development and regression checks. PS5 linking is in ps5.mk.
.DEFAULT_GOAL := all

CC ?= cc
PYTHON ?= python3
CLANG_FORMAT ?= clang-format-18
PREVIEW_PORT ?= 7456
PREVIEW_BIND ?= 127.0.0.1
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Isrc/core -Isrc/web -Isrc/ps5 \
	-Isrc/drivers/common -Ibuild

CONTROLLER_DATA := build/controller_registry.inc

BUILD := build
HOST_BINARY := $(BUILD)/unicontroller-host

LOG_SOURCES := src/logging.c vendor/rxi-log/log.c
LOG_HEADERS := src/logging.h vendor/rxi-log/log.h

BT_DIR := src/drivers/bluetooth
BT_NAMES := host le smp_crypto usb_desc evstream
BT_SOURCES := $(addprefix $(BT_DIR)/,$(addsuffix .c,$(BT_NAMES)))
DRIVER_SHARED := src/drivers/protocols/profiles.c src/drivers/protocols/generic.c \
	src/drivers/common/util.c src/drivers/common/crc32.c $(LOG_SOURCES)
BT_MANAGER := src/drivers/bt.c

USB_SOURCES := src/drivers/usb.c src/drivers/usb/transport.c src/drivers/usb/protocol.c \
	src/drivers/usb/diagnostics.c src/drivers/usb/generic_hid.c

CONFIG_SOURCES := src/drivers/config/json.c src/drivers/config/overrides.c

CORE_SOURCES := src/core/uc_core.c src/core/bt_autostart.c
HOST_SOURCES := \
	$(CORE_SOURCES) \
	$(BT_MANAGER) \
	$(BT_SOURCES) \
	$(DRIVER_SHARED) \
	$(USB_SOURCES) \
	$(CONFIG_SOURCES) \
	src/ps5/launcher.c \
	src/web/runtime.c \
	tests/web/host_main.c \
	tests/web/host_backend.c

C_SOURCES := $(shell find src tests -type f \( -name '*.c' -o -name '*.h' \) | sort)
C_HEADERS := $(filter %.h,$(C_SOURCES)) $(LOG_HEADERS)
WEB_SOURCES := $(shell find web -type f \( -name '*.html' -o -name '*.css' \
	-o -name '*.js' -o -name '*.png' -o -name '*.svg' \) | sort)
PYTHON_SOURCES := $(shell find tools tests -type f -name '*.py' | sort)

.PHONY: all assets host ps5 test test-core test-json test-backend test-web test-ports test-ui
.PHONY: test-bluetooth test-bt-manager test-bt-ui test-usb-protocol test-usb-manager test-usb-ui
.PHONY: format format-check clean preview controllers test-config

all: host

controllers: $(CONTROLLER_DATA)
build/controller_registry.inc: src/drivers/protocols/controllers.json tools/embed_controllers.py
	$(PYTHON) tools/embed_controllers.py


# Generated resources are never edited or formatted manually.
assets: $(BUILD)/assets.c

$(BUILD)/assets.c: $(WEB_SOURCES) tools/embed_assets.py src/web/assets.h
	$(PYTHON) tools/embed_assets.py

# Isolated demo preview; fixture data never ships in the payload.
preview:
	$(PYTHON) tools/preview_server.py --port $(PREVIEW_PORT) --bind $(PREVIEW_BIND)

host: $(HOST_BINARY)

$(HOST_BINARY): $(HOST_SOURCES) $(C_HEADERS) $(BUILD)/assets.c $(CONTROLLER_DATA)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(HOST_SOURCES) $(BUILD)/assets.c -o $@

ps5: assets controllers $(BUILD)/launcher_icon.inc
	$(MAKE) -f ps5.mk

# Core/platform tests run without sockets; web tests bind local listeners.
test: test-core test-json test-backend test-web test-ports test-ui test-bluetooth test-bt-manager test-bt-ui \
	test-usb-protocol test-usb-manager test-usb-ui test-config

test-core: $(BUILD)/test-core
	./$(BUILD)/test-core

$(BUILD)/test-core: $(CORE_SOURCES) tests/core/test_core.c src/core/uc_core.h $(LOG_SOURCES) $(LOG_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE_SOURCES) tests/core/test_core.c $(LOG_SOURCES) -o $@

test-json: $(BUILD)/test-json
	./$(BUILD)/test-json

$(BUILD)/test-json: tests/web/test_json.c src/web/json.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/web/test_json.c -o $@

test-backend: $(BUILD)/test-backend
	./$(BUILD)/test-backend

$(BUILD)/test-backend: tests/ps5/test_backend.c src/ps5/backend.c src/ps5/uc_backend.h \
	src/core/bt_autostart.c src/core/bt_autostart.h $(LOG_SOURCES) $(LOG_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ps5/test_backend.c src/core/bt_autostart.c $(LOG_SOURCES) -o $@

test-web: host
	$(PYTHON) tests/web/test_runtime.py $(HOST_BINARY)

test-ports: host
	$(PYTHON) tests/web/test_ports.py $(HOST_BINARY)

# Install formatter versions from requirements-dev.txt and docs/code-style.md.
format:
	$(CLANG_FORMAT) -i $(C_SOURCES)
	$(PYTHON) -m black $(PYTHON_SOURCES)
	npx --yes prettier@3.6.2 --write 'web/**/*.{html,css,js}' 'tests/**/*.{js,cjs}' 'tools/**/*.js' '**/*.md' \
		'.github/**/*.yml' '*.json' 'src/**/*.json' 'examples/**/*.json' \
		'.clang-format' '.editorconfig' --ignore-unknown

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(C_SOURCES)
	$(PYTHON) -m black --check $(PYTHON_SOURCES)
	npx --yes prettier@3.6.2 --check 'web/**/*.{html,css,js}' 'tests/**/*.{js,cjs}' 'tools/**/*.js' '**/*.md' \
		'.github/**/*.yml' '*.json' 'src/**/*.json' 'examples/**/*.json' \
		'.clang-format' '.editorconfig' --ignore-unknown
	$(PYTHON) tools/check_make_style.py Makefile ps5.mk

clean:
	rm -rf $(BUILD) dist

test-ui:
	node tests/web/test_ui.js
	node tests/web/test_models.js

test-bluetooth: controllers
	@mkdir -p $(BUILD)
	CC=$(CC) bash tests/bluetooth/run.sh > $(BUILD)/bluetooth-tests.log 2>&1 || \
		{ tail -80 $(BUILD)/bluetooth-tests.log; exit 1; }
	@tail -8 $(BUILD)/bluetooth-tests.log

test-bt-manager: $(BUILD)/test-bt-manager
	./$(BUILD)/test-bt-manager

$(BUILD)/test-bt-manager: src/drivers/bt.c src/drivers/bt.h tests/bt_manager/test_manager.c \
	$(LOG_SOURCES) $(LOG_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/drivers/bt.c tests/bt_manager/test_manager.c $(LOG_SOURCES) -o $@

test-bt-ui:
	node tests/web/test_bt_ui.js

test-usb-protocol: $(BUILD)/test-usb-protocol $(BUILD)/test-usb-native
	./$(BUILD)/test-usb-protocol
	./$(BUILD)/test-usb-native

$(BUILD)/test-usb-protocol: src/drivers/usb/transport.c src/drivers/usb/protocol.c \
	src/drivers/usb/diagnostics.c src/drivers/usb/generic_hid.c \
	tests/usb_protocol/test_usb_protocol.c $(C_HEADERS) $(DRIVER_SHARED) $(CONTROLLER_DATA) \
	$(CONFIG_SOURCES)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/drivers/usb/transport.c src/drivers/usb/protocol.c \
		src/drivers/usb/diagnostics.c src/drivers/usb/generic_hid.c \
		tests/usb_protocol/test_usb_protocol.c $(DRIVER_SHARED) $(CONFIG_SOURCES) -o $@

$(BUILD)/test-usb-native: tests/usb_native/test_native.c src/drivers/usb/transport.c \
	src/drivers/usb/diagnostics.c \
	tests/usb_native/include/dev/usb/usb.h tests/usb_native/include/dev/usb/usb_ioctl.h \
	$(C_HEADERS) $(LOG_SOURCES)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/usb_native/include tests/usb_native/test_native.c \
		src/drivers/usb/diagnostics.c \
		$(LOG_SOURCES) -o $@

test-usb-manager: $(BUILD)/test-usb-manager
	./$(BUILD)/test-usb-manager

$(BUILD)/test-usb-manager: src/drivers/usb.c src/drivers/usb/diagnostics.c \
	tests/usb_manager/test_manager.c $(C_HEADERS) $(LOG_SOURCES)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/drivers/usb.c src/drivers/usb/diagnostics.c \
		tests/usb_manager/test_manager.c $(LOG_SOURCES) -o $@

test-usb-ui:
	node tests/web/test_usb_ui.js

test-config:
	$(PYTHON) -m unittest discover -s tests/config -v


.PHONY: test-overrides
test: test-overrides
test-overrides: $(BUILD)/test-overrides
	./$(BUILD)/test-overrides

$(BUILD)/test-overrides: tests/config/test_overrides.c $(CONFIG_SOURCES) $(DRIVER_SHARED) \
	$(C_HEADERS) $(CONTROLLER_DATA)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/config/test_overrides.c $(CONFIG_SOURCES) \
		$(DRIVER_SHARED) -o $@


$(BUILD)/launcher_icon.inc: assets/branding/icon0.png tools/embed_icon.py
	$(PYTHON) tools/embed_icon.py


.PHONY: test-launcher
test: test-launcher
test-launcher: $(BUILD)/test-launcher
	./$(BUILD)/test-launcher

$(BUILD)/test-launcher: tests/launcher/test_launcher.c src/ps5/launcher.c \
	src/ps5/launcher.h $(BUILD)/launcher_icon.inc $(LOG_SOURCES) $(LOG_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/launcher/test_launcher.c $(LOG_SOURCES) -o $@


.PHONY: test-bt-autostart
test: test-bt-autostart
test-bt-autostart: $(BUILD)/test-bt-autostart
	./$(BUILD)/test-bt-autostart

$(BUILD)/test-bt-autostart: src/core/bt_autostart.c src/core/bt_autostart.h \
	tests/core/test_bt_autostart.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/core/bt_autostart.c tests/core/test_bt_autostart.c -o $@


.PHONY: test-browser
test-browser:
	npm run test:browser


.PHONY: test-logging
test: test-logging
test-logging: $(BUILD)/test-logging
	./$(BUILD)/test-logging

$(BUILD)/test-logging: tests/logging/test_logging.c $(LOG_SOURCES) $(LOG_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/logging/test_logging.c $(LOG_SOURCES) -o $@
