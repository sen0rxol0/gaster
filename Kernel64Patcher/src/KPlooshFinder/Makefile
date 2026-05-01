PLATFORM_SRC = $(wildcard patches/*)
SRC = $(wildcard src/*)
OBJDIR = obj
PLATFORM_OBJS = $(patsubst patches/%,$(OBJDIR)/patches/%,$(PLATFORM_SRC:.c=.o))
OBJS = $(patsubst src/%,$(OBJDIR)/%,$(SRC:.c=.o)) $(PLATFORM_OBJS)
PLOOSHFINDER = plooshfinder/libplooshfinder.a
INCLDIRS = -I./include -I./plooshfinder/include

LDFLAGS ?=
LDFLAGS += -L./plooshfinder
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
CC := clang
LIBS = -lplooshfinder

.PHONY: all dirs clean

all: dirs $(PLOOSHFINDER) KPlooshFinder

dirs:
	@mkdir -p $(OBJDIR)
	@mkdir -p $(OBJDIR)/patches

clean:
	@rm -rf KPlooshFinder obj
	@$(MAKE) -C plooshfinder clean

KPlooshFinder: $(OBJS) $(PLOOSHFINDER)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(OBJDIR)/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLDIRS) -c -o $@ $<

$(OBJDIR)/patches/%.o: patches/%.c
	$(CC) $(CFLAGS) $(INCLDIRS) -c -o $@ $<

$(PLOOSHFINDER):
	$(MAKE) -C plooshfinder all
