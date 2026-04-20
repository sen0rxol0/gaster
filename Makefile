SRC = $(shell pwd)
LIBS_DIR = $(SRC)/libs_root
STRIP = strip
CC ?= cc
CFLAGS += -I$(LIBS_DIR)/include
CFLAGS += -Weverything -DGASTERAIN_VERSION=\"1.0\"
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
CFLAGS += -DBUILD_STYLE="RELEASE" -DBUILD_DATE="\"$(shell LANG=C date)\"" -DBUILD_WHOAMI=\"$(shell whoami)\"
LIBS += -lc -lz

SRCC=main.c lzfse.c gastera1n.c ideviceenterramdisk.c kernel64patcher.c

all: gastera1n

headers:
	xxd -iC payload_A9.bin payload_A9.h
	xxd -iC payload_notA9.bin payload_notA9.h
	xxd -iC payload_notA9_armv7.bin payload_notA9_armv7.h
	xxd -iC payload_handle_checkm8_request.bin payload_handle_checkm8_request.h
	xxd -iC payload_handle_checkm8_request_armv7.bin payload_handle_checkm8_request_armv7.h

gastera1n: headers
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCC) $(LIBS) -o gastera1n
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
