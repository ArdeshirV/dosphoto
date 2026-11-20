# dphoto - DOS image viewer (PNG / JPEG / BMP -> VGA 256 colours)
#
#   make dos    build DPHOTO.EXE for real-mode DOS (ia16-elf-gcc)
#   make host   build the host test harness in build/
#   make test   run the host decoder comparisons (needs ffmpeg)
#   make clean

CC16    ?= ia16-elf-gcc
CC      ?= gcc

CFLAGS16 ?= -march=i8086 -mcmodel=small -Os -std=gnu99 -Wall -Wextra
LDFLAGS16 ?= -li86

CFLAGS  ?= -O2 -g -std=c99 -Wall -Wextra
LDLIBS  ?= -lm

SRC     := src/img.c src/stream.c src/inflate.c src/png.c src/bmp.c \
           src/jpeg.c src/dimg.c src/render.c

DOS_SRC := $(SRC) src/dos/mem.c src/dos/ui.c src/dos/main.c

OBJDIR  := build/dos
DOS_OBJ := $(patsubst %.c,$(OBJDIR)/%.o,$(DOS_SRC))

HOST_OBJ := build/host/img.o build/host/stream.o build/host/inflate.o \
            build/host/png.o build/host/bmp.o build/host/jpeg.o \
            build/host/dimg.o build/host/render.o build/host/host_mem.o \
            build/host/hosttest.o

.PHONY: all dos host test clean

all: dos host

dos: build/DPHOTO.EXE

build/DPHOTO.EXE: $(DOS_OBJ)
	@mkdir -p build
	$(CC16) $(CFLAGS16) -o $@ $(DOS_OBJ) $(LDFLAGS16)
	@ls -l $@

$(OBJDIR)/src/%.o: src/%.c src/img.h src/render.h
	@mkdir -p $(dir $@)
	$(CC16) $(CFLAGS16) -c $< -o $@

$(OBJDIR)/src/dos/%.o: src/dos/%.c src/img.h src/render.h src/dos/ui.h
	@mkdir -p $(dir $@)
	$(CC16) $(CFLAGS16) -c $< -o $@

host: build/hosttest

build/hosttest: $(HOST_OBJ)
	$(CC) $(CFLAGS) -o $@ $(HOST_OBJ) $(LDLIBS)

build/host/%.o: src/%.c src/img.h src/render.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/host/host_mem.o: test/host_mem.c src/img.h
	@mkdir -p build/host
	$(CC) $(CFLAGS) -c $< -o $@

build/host/hosttest.o: test/hosttest.c src/img.h src/render.h
	@mkdir -p build/host
	$(CC) $(CFLAGS) -c $< -o $@

test: build/hosttest
	./test/run_host_tests.sh

clean:
	rm -rf build
