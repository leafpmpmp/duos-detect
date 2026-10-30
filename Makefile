# SPDX-License-Identifier: AGPL-3.0-or-later
# Native or cross build for the Duo S, ARM (A53) or RISC-V (C906) mode:
#   make CROSS_COMPILE=aarch64-linux-gnu- CVIRT=/path/to/cviruntime/install
#   make CROSS_COMPILE=riscv64-linux-gnu- CVIRT=/path/to/cviruntime/install
# Needs libjpeg (libjpeg62-turbo-dev) for the target. Output goes to
# build/<target triplet>/.
CC = $(CROSS_COMPILE)gcc
CVIRT ?= /usr/local
TARGET := $(shell $(CC) -dumpmachine)
B := build/$(TARGET)

ifneq ($(findstring aarch64,$(TARGET)),)
ARCHFLAGS = -mcpu=cortex-a53
endif
ifneq ($(findstring riscv64,$(TARGET)),)
# The vector front end is built for XTheadVector and used only when the
# kernel enables it; everything else stays rv64gc.
ARCHFLAGS = -march=rv64gc -mabi=lp64d
XTV = $(B)/frontend_xtv.o $(B)/ycc_xtv.o
endif

CFLAGS ?= -O3
CFLAGS += $(ARCHFLAGS) -std=gnu11 -Wall -Wextra -I$(CVIRT)/include
LDLIBS = -L$(CVIRT)/lib -lcviruntime -lcvikernel -ljpeg -lpthread -lm

OBJ = $(addprefix $(B)/, main.o capture.o frontend.o detector.o http.o \
	preview.o ycc.o cpu.o lens.o af.o camera.o) $(XTV)

all: $(B)/duos-detect $(B)/fe-bench $(B)/jpeg-bench

$(B)/duos-detect: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(B)/fe-bench: $(B)/fe_bench.o $(B)/frontend.o $(B)/cpu.o $(XTV)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

$(B)/jpeg-bench: $(B)/jpeg_bench.o $(B)/preview.o $(B)/ycc.o $(B)/cpu.o $(XTV)
	$(CC) $(LDFLAGS) -o $@ $^ -ljpeg

$(B)/jpeg_bench.o: tools/jpeg_bench.c src/*.h | $(B)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

$(B)/%.o: src/%.c src/*.h | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/fe_bench.o: tools/fe_bench.c src/frontend.h | $(B)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

$(B)/%_xtv.o: src/%_xtv.c src/*.h | $(B)
	$(CC) $(CFLAGS) -march=rv64gc_xtheadvector -c -o $@ $<

$(B):
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean
