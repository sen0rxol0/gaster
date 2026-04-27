SRC      = $(shell pwd)
LIBS_DIR = $(SRC)/libs_root
CC      ?= cc
CFLAGS  += -I$(LIBS_DIR)/include

# -Weverything is Clang-only; gate it so GCC builds don't break
ifeq ($(shell $(CC) --version 2>&1 | grep -c clang),1)
CFLAGS += -Weverything \
          -Wno-unused-macros \
          -Wno-padded \
          -Wno-poison-system-directories \
          -Wno-declaration-after-statement \
          -Wno-used-but-marked-unused
else
CFLAGS += -Wall -Wextra -Wpedantic
endif

CFLAGS += -DGASTERAIN_VERSION=\"1.0\"

LIBS += $(LIBS_DIR)/lib/libgeneral.a \
        $(LIBS_DIR)/lib/libfragmentzip.a \
        -lcurl -lpthread

ifeq ($(TARGET_OS),)
TARGET_OS = $(shell uname -s)
endif

ifeq ($(TARGET_OS),Darwin)
ifeq (,$(findstring version-min=,$(CFLAGS)))
CFLAGS += -mmacosx-version-min=10.13
endif
LDFLAGS += -Wl,-dead_strip
LIBS    += -framework CoreFoundation -framework IOKit
# libplist and libirecovery live in the host app's Frameworks bundle.
# Link dynamically so the linker records the load command; the host app
# already owns the canonical dylibs — do NOT statically link them.
#
# On macOS the sysroot contains no actual libplist/libirecovery archive
# (build.sh removes them after installing headers).  We therefore must NOT
# pass -L$(LIBS_DIR)/lib here, because ld would search that directory first
# and fail with "library not found for -lplist-2.0".  Instead we rely on
# the -rpath entries below so dyld finds the host Frameworks/ copies at
# runtime; for link-time resolution we search the SDK and system lib dirs
# by omitting any explicit -L override for these two libraries.
LIBS    += -lplist-2.0 -lirecovery-1.0
# Embed an LC_RPATH so dyld resolves the dylibs from Contents/Frameworks/
# at runtime when the binary sits at Contents/MacOS/gastera1n.
LDFLAGS += -Wl,-rpath,@executable_path/../../Frameworks
# Also cover the flat bundle layout (gastera1n alongside Frameworks/).
LDFLAGS += -Wl,-rpath,@executable_path/../Frameworks
else
# Linux — libplist and libirecovery are macOS-specific (DFU/recovery mode
# via IOKit + CoreFoundation is not available on Linux).  Omit them entirely
# to avoid linker errors on systems where the host packages are absent.
CFLAGS  += -fdata-sections -ffunction-sections
LDFLAGS += -static -no-pie -Wl,--gc-sections
# Point the linker at our static sysroot so libgeneral/libfragmentzip are found.
LDFLAGS += -L$(LIBS_DIR)/lib
endif

CFLAGS += -Os -g
CFLAGS += -DBUILD_STYLE="RELEASE" \
          -DBUILD_DATE="\"$(shell LANG=C date)\"" \
          -DBUILD_WHOAMI=\"$(shell whoami)\"

LIBS += -lc -lz

SRCC = main.c lzfse.c gastera1n.c ideviceenterramdisk.c kernel64patcher.c kerneldiff.c

# Generated header files
PAYLOAD_HEADERS = \
    payload_A9.h \
    payload_notA9.h \
    payload_notA9_armv7.h \
    payload_handle_checkm8_request.h \
    payload_handle_checkm8_request_armv7.h

.PHONY: all headers payloads clean

all: gastera1n

# Generate headers from binary payloads
$(PAYLOAD_HEADERS): headers

headers:
	xxd -iC payload_A9.bin                        payload_A9.h
	xxd -iC payload_notA9.bin                     payload_notA9.h
	xxd -iC payload_notA9_armv7.bin               payload_notA9_armv7.h
	xxd -iC payload_handle_checkm8_request.bin    payload_handle_checkm8_request.h
	xxd -iC payload_handle_checkm8_request_armv7.bin \
	                                              payload_handle_checkm8_request_armv7.h

# gastera1n is a real file target, NOT phony, so make can track staleness.
# Headers are generated first; if compilation fails the headers remain on
# disk so the next invocation can attempt a rebuild without re-running xxd.
# They are only cleaned up on an explicit `make clean`.
gastera1n: $(PAYLOAD_HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCC) $(LIBS) -o $@

payloads:
	as -arch arm64  payload_A9.S  -o payload_A9.o
	gobjcopy -O binary -j .text payload_A9.o payload_A9.bin
	$(RM) payload_A9.o

	as -arch arm64  payload_notA9.S  -o payload_notA9.o
	gobjcopy -O binary -j .text payload_notA9.o payload_notA9.bin
	$(RM) payload_notA9.o

	as -arch armv7  payload_notA9_armv7.S  -o payload_notA9_armv7.o
	gobjcopy -O binary -j .text payload_notA9_armv7.o payload_notA9_armv7.bin
	$(RM) payload_notA9_armv7.o

	as -arch arm64  payload_handle_checkm8_request.S  -o payload_handle_checkm8_request.o
	gobjcopy -O binary -j .text payload_handle_checkm8_request.o payload_handle_checkm8_request.bin
	$(RM) payload_handle_checkm8_request.o

	as -arch armv7  payload_handle_checkm8_request_armv7.S  -o payload_handle_checkm8_request_armv7.o
	gobjcopy -O binary -j .text payload_handle_checkm8_request_armv7.o payload_handle_checkm8_request_armv7.bin
	$(RM) payload_handle_checkm8_request_armv7.o

clean:
	$(RM) gastera1n $(PAYLOAD_HEADERS)
