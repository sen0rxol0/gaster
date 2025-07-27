SRC = $(shell pwd)
LIBS_DIR = $(SRC)/libs_root
STRIP = strip
CC ?= cc
CFLAGS += -I$(LIBS_DIR)/include
# CFLAGS += -I$(LIBS_DIR)/include -I$(SRC)/include -I$(SRC)
# CFLAGS += -Os -Weverything -DGASTERAIN_VERSION=\"1.0\"
CFLAGS += -Wall -Wextra -Wno-unused-parameter -DGASTERAIN_VERSION=\"1.0.0\"
# CFLAGS += -Wno-unused-variable -std=c99 -pedantic-errors -D_C99_SOURCE -D_POSIX_C_SOURCE=200112L -D_DARWIN_C_SOURCE
LIBS += $(LIBS_DIR)/lib/libimobiledevice-1.0.a $(LIBS_DIR)/lib/libirecovery-1.0.a $(LIBS_DIR)/lib/libusbmuxd-2.0.a
LIBS += $(LIBS_DIR)/lib/libimobiledevice-glue-1.0.a $(LIBS_DIR)/lib/libplist-2.0.a -pthread
ifeq ($(TARGET_OS),)
TARGET_OS = $(shell uname -s)
endif
ifeq ($(TARGET_OS),Darwin)
#CFLAGS += -Wno-nullability-extension
ifeq (,$(findstring version-min=, $(CFLAGS)))
CFLAGS += -mmacosx-version-min=10.8
endif
LDFLAGS += -Wl,-dead_strip
LIBS += -framework CoreFoundation -framework IOKit
else
#linux
CFLAGS += -fdata-sections -ffunction-sections
LDFLAGS += -static -no-pie -Wl,--gc-sections
endif

#CFLAGS += -Os -g
#LIBS += -lc

BUILD_STYLE = RELEASE
BUILD_DATE := $(shell LANG=C date)
BUILD_WHOAMI := $(shell whoami)

CFLAGS += -DBUILD_STYLE="\"$(BUILD_STYLE)\"" -DBUILD_DATE="\"$(BUILD_DATE)\"" -DBUILD_WHOAMI=\"$(BUILD_WHOAMI)\"

CSRC=main.c lzfse.c gastera1n.c ideviceenterrecovery.c idevicedfu.c ideviceenterramdisk.c kernel64patcher.c

# export SRC DEP CC CFLAGS LDFLAGS LIBS TARGET_OS DEV_BUILD BUILD_DATE BUILD_TAG BUILD_WHOAMI BUILD_STYLE

all: gastera1n

headers:
	xxd -iC payload_A9.bin payload_A9.h
	xxd -iC payload_notA9.bin payload_notA9.h
	xxd -iC payload_notA9_armv7.bin payload_notA9_armv7.h
	xxd -iC payload_handle_checkm8_request.bin payload_handle_checkm8_request.h
	xxd -iC payload_handle_checkm8_request_armv7.bin payload_handle_checkm8_request_armv7.h

gastera1n: headers
	$(CC) $(CFLAGS) $(LDFLAGS) $(CSRC) $(LIBS) -o gastera1n
	# xcrun -sdk macosx clang -mmacosx-version-min=10.15 -Os -Weverything $(INCL) $(MACOSX_LIBS) -framework CoreFoundation -framework IOKit $(SRC) -o gastera1n
	$(RM) payload_A9.h payload_notA9.h payload_notA9_armv7.h payload_handle_checkm8_request.h payload_handle_checkm8_request_armv7.h

.PHONY: all gastera1n headers

# MACOSX_LIBS = /usr/local/Cellar/libplist/2.7.0/lib/libplist-2.0.a \
# /usr/local/Cellar/libirecovery/1.2.1/lib/libirecovery-1.0.a \
# /usr/local/Cellar/libimobiledevice-glue/1.3.2/lib/libimobiledevice-glue-1.0.dylib \
# /usr/local/Cellar/libimobiledevice/1.3.0/lib/libimobiledevice-1.0.dylib \
# /usr/local/Cellar/curl/8.6.0/lib/libcurl.a \
# /usr/local/lib/libfragmentzip.dylib
#
# # /usr/local/Cellar/libusbmuxd/2.0.2/lib/libusbmuxd-2.0.a \
# # -I/usr/local/Cellar/libusbmuxd/2.0.2/include \
#
# INCL = -I/usr/local/Cellar/libirecovery/1.2.1/include \
# -I/usr/local/Cellar/libimobiledevice/1.3.0/include \
# -I/usr/local/Cellar/libimobiledevice-glue/1.3.2/include \
# -I/usr/local/Cellar/libplist/2.7.0/include \
# -I/usr/local/Cellar/curl/8.6.0/include \
# -I/usr/local/include
#
# SRC=main.c lzfse.c gastera1n.c ideviceenterrecovery.c idevicedfu.c ideviceenterramdisk.c kernel64patcher.c
#
# .PHONY: macos libusb payload clean
#
# headers:
# 	xxd -iC payload_A9.bin payload_A9.h
# 	xxd -iC payload_notA9.bin payload_notA9.h
# 	xxd -iC payload_notA9_armv7.bin payload_notA9_armv7.h
# 	xxd -iC payload_handle_checkm8_request.bin payload_handle_checkm8_request.h
# 	xxd -iC payload_handle_checkm8_request_armv7.bin payload_handle_checkm8_request_armv7.h
#
# macos: headers
# 	xcrun -sdk macosx clang -mmacosx-version-min=10.15 -Os -Weverything $(INCL) $(MACOSX_LIBS) -framework CoreFoundation -framework IOKit $(SRC) -o gastera1n
# 	$(RM) payload_A9.h payload_notA9.h payload_notA9_armv7.h payload_handle_checkm8_request.h payload_handle_checkm8_request_armv7.h
#
# libusb: headers
# 	$(CC) -Wall -Wextra -Wpedantic -DHAVE_LIBUSB gastera1n.c lzfse.c plugin.c -o gastera1n -lusb-1.0 -lcrypto -Os
# 	$(RM) payload_A9.h payload_notA9.h payload_notA9_armv7.h payload_handle_checkm8_request.h payload_handle_checkm8_request_armv7.h
#
# payload:
# 	as -arch arm64 payload_A9.S -o payload_A9.o
# 	gobjcopy -O binary -j .text payload_A9.o payload_A9.bin
# 	$(RM) payload_A9.o
# 	as -arch arm64 payload_notA9.S -o payload_notA9.o
# 	gobjcopy -O binary -j .text payload_notA9.o payload_notA9.bin
# 	$(RM) payload_notA9.o
# 	as -arch armv7 payload_notA9_armv7.S -o payload_notA9_armv7.o
# 	gobjcopy -O binary -j .text payload_notA9_armv7.o payload_notA9_armv7.bin
# 	$(RM) payload_notA9_armv7.o
# 	as -arch arm64 payload_handle_checkm8_request.S -o payload_handle_checkm8_request.o
# 	gobjcopy -O binary -j .text payload_handle_checkm8_request.o payload_handle_checkm8_request.bin
# 	$(RM) payload_handle_checkm8_request.o
# 	as -arch armv7 payload_handle_checkm8_request_armv7.S -o payload_handle_checkm8_request_armv7.o
# 	gobjcopy -O binary -j .text payload_handle_checkm8_request_armv7.o payload_handle_checkm8_request_armv7.bin
# 	$(RM) payload_handle_checkm8_request_armv7.o
#
# clean:
# 	$(RM) gastera1n
