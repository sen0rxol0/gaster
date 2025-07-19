MACOSX_LIBS = /usr/local/Cellar/libplist/2.7.0/lib/libplist-2.0.a \
/usr/local/Cellar/libirecovery/1.2.1/lib/libirecovery-1.0.a \
/usr/local/Cellar/libimobiledevice-glue/1.3.2/lib/libimobiledevice-glue-1.0.dylib \
/usr/local/Cellar/libimobiledevice/1.3.0/lib/libimobiledevice-1.0.dylib \
/usr/local/Cellar/libusbmuxd/2.0.2/lib/libusbmuxd-2.0.a \
/usr/local/Cellar/curl/8.6.0/lib/libcurl.a \
/usr/local/lib/libfragmentzip.dylib

INCL = -I/usr/local/Cellar/libirecovery/1.2.1/include \
-I/usr/local/Cellar/libimobiledevice/1.3.0/include \
-I/usr/local/Cellar/libimobiledevice-glue/1.3.2/include \
-I/usr/local/Cellar/libusbmuxd/2.0.2/include \
-I/usr/local/Cellar/libplist/2.7.0/include \
-I/usr/local/Cellar/curl/8.6.0/include \
-I/usr/local/include

SRC=main.c lzfse.c gastera1n.c ideviceenterrecovery.c idevicedfu.c ideviceenterramdisk.c kernel64patcher.c

.PHONY: macos libusb payload clean

headers:
	xxd -iC payload_A9.bin payload_A9.h
	xxd -iC payload_notA9.bin payload_notA9.h
	xxd -iC payload_notA9_armv7.bin payload_notA9_armv7.h
	xxd -iC payload_handle_checkm8_request.bin payload_handle_checkm8_request.h
	xxd -iC payload_handle_checkm8_request_armv7.bin payload_handle_checkm8_request_armv7.h

macos: headers
	xcrun -sdk macosx clang -mmacosx-version-min=10.15 -Os -Weverything $(INCL) $(MACOSX_LIBS) -framework CoreFoundation -framework IOKit $(SRC) -o gastera1n
	$(RM) payload_A9.h payload_notA9.h payload_notA9_armv7.h payload_handle_checkm8_request.h payload_handle_checkm8_request_armv7.h

libusb: headers
	$(CC) -Wall -Wextra -Wpedantic -DHAVE_LIBUSB gastera1n.c lzfse.c plugin.c -o gastera1n -lusb-1.0 -lcrypto -Os
	$(RM) payload_A9.h payload_notA9.h payload_notA9_armv7.h payload_handle_checkm8_request.h payload_handle_checkm8_request_armv7.h

payload:
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

clean:
	$(RM) gastera1n
