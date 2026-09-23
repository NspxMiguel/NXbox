# SPDX-License-Identifier: GPL-3.0-or-later
DEVKITPRO ?= /opt/devkitpro
CC := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
ELF2NRO := $(DEVKITPRO)/tools/bin/elf2nro
BUILD := build
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := $(ARCH) -O2 -Wall -Wextra -Werror -ffunction-sections -D__SWITCH__ -I$(DEVKITPRO)/libnx/include
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs -L$(DEVKITPRO)/libnx/lib

.PHONY: all clean
all: $(BUILD)/boot.nro

$(BUILD):
	mkdir -p $@

$(BUILD)/boot.elf: main.c | $(BUILD)
	$(CC) $(CFLAGS) $< $(LDFLAGS) -lnx -o $@

$(BUILD)/boot.nro: $(BUILD)/boot.elf
	$(ELF2NRO) $< $@

clean:
	rm -rf $(BUILD)
