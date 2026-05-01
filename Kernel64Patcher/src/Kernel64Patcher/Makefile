CC ?= clang
CFLAGS ?= -O2
LDFLAGS ?=

VERSION := $(shell git rev-parse --short HEAD)-$(shell git rev-list --count HEAD)

TARGET := Kernel64Patcher
SRC := Kernel64Patcher.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) \
		-DVERSION=\"$(VERSION)\" \
		$(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
