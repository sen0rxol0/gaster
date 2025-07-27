SRC = $(shell pwd)
LIBS_DIR = $(SRC)/libs_root
STRIP = strip
CC ?= cc
CFLAGS += -I$(LIBS_DIR)/include
# CFLAGS += -I$(LIBS_DIR)/include -I$(SRC)/include -I$(SRC)
CFLAGS += -Os -Weverything -DGASTERAIN_VERSION=\"1.0\"
# CFLAGS += -Wall -Wextra -Wno-unused-parameter -DGASTERAIN_VERSION=\"1.0.0\"
# CFLAGS += -Wno-unused-variable -std=c99 -pedantic-errors -D_C99_SOURCE -D_POSIX_C_SOURCE=200112L -D_DARWIN_C_SOURCE
LIBS += $(LIBS_DIR)/lib/libmbedtls.a $(LIBS_DIR)/lib/libmbedcrypto.a $(LIBS_DIR)/lib/libmbedx509.a $(LIBS_DIR)/lib/libreadline.a
LIBS += $(LIBS_DIR)/lib/libusbmuxd-2.0.a $(LIBS_DIR)/lib/libplist-2.0.a $(LIBS_DIR)/lib/libirecovery-1.0.a
LIBS += $(LIBS_DIR)/lib/libimobiledevice-glue-1.0.a $(LIBS_DIR)/lib/libimobiledevice-1.0.a
LIBS += $(LIBS_DIR)/lib/libgeneral.a $(LIBS_DIR)/lib/libfragmentzip.a -lcurl -lpthread
ifeq ($(TARGET_OS),)
TARGET_OS = $(shell uname -s)
endif
ifeq ($(TARGET_OS),Darwin)
#CFLAGS += -Wno-nullability-extension
ifeq (,$(findstring version-min=, $(CFLAGS)))
CFLAGS += -mmacosx-version-min=10.13
endif
LDFLAGS += -Wl,-dead_strip
LIBS += -framework CoreFoundation -framework IOKit
else
#linux
CFLAGS += -fdata-sections -ffunction-sections
LDFLAGS += -static -no-pie -Wl,--gc-sections
endif

CFLAGS += -Os -g
LIBS += -lc -lz

BUILD_STYLE = RELEASE
BUILD_DATE := $(shell LANG=C date)
BUILD_WHOAMI := $(shell whoami)

CFLAGS += -DBUILD_STYLE="\"$(BUILD_STYLE)\"" -DBUILD_DATE="\"$(BUILD_DATE)\"" -DBUILD_WHOAMI=\"$(BUILD_WHOAMI)\"

CSRC=main.c lzfse.c gastera1n.c ideviceenterrecovery.c idevicedfu.c ideviceenterramdisk.c kernel64patcher.c

all: gastera1n

headers:
	xxd -iC payload_A9.bin payload_A9.h
	xxd -iC payload_notA9.bin payload_notA9.h
	xxd -iC payload_notA9_armv7.bin payload_notA9_armv7.h
	xxd -iC payload_handle_checkm8_request.bin payload_handle_checkm8_request.h
	xxd -iC payload_handle_checkm8_request_armv7.bin payload_handle_checkm8_request_armv7.h

gastera1n: headers
	$(CC) $(CFLAGS) $(LDFLAGS) $(CSRC) $(LIBS) -o gastera1n
	$(RM) payload_A9.h payload_notA9.h payload_notA9_armv7.h payload_handle_checkm8_request.h payload_handle_checkm8_request_armv7.h

# libusb: headers
# 	$(CC) -Wall -Wextra -Wpedantic -DHAVE_LIBUSB gastera1n.c lzfse.c plugin.c -o gastera1n -lusb-1.0 -lcrypto -Os
# 	$(RM) payload_A9.h payload_notA9.h payload_notA9_armv7.h payload_handle_checkm8_request.h payload_handle_checkm8_request_armv7.h

payloads:
	as -arch arm64 payload_A9.S -o payload_A9.o
	gobjcopy -O binary -j .text payload_A9.o payload_A9.bin
	$(RM) payload_A9.o
	as -arch arm64 payload_notA9.S -o payload_notA9.o
	gobjcopy -O binary -j .text payload_notA9.o payload_notA9.bin
	$(RM) payload_notA9.o
	as -arch armv7 payload_notA9_armv7.S -o payload_notA9_armv7.o
	gobjcopy -O binary -j .text payload_notA9_armv7.o payload_notA9_armv7.bin
	$(RM) payload_notA9_armv7.o
	as -arch arm64 payload_handle_checkm8_request.S -o payload_handle_checkm8_request.o
	gobjcopy -O binary -j .text payload_handle_checkm8_request.o payload_handle_checkm8_request.bin
	$(RM) payload_handle_checkm8_request.o
	as -arch armv7 payload_handle_checkm8_request_armv7.S -o payload_handle_checkm8_request_armv7.o
	gobjcopy -O binary -j .text payload_handle_checkm8_request_armv7.o payload_handle_checkm8_request_armv7.bin
	$(RM) payload_handle_checkm8_request_armv7.o

.PHONY: all headers gastera1n
