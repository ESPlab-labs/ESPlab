# Makefile — ESPlab host build (the virtual boards, tests, and EOD tooling)
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -std=gnu11 -O2
CORE    = core/src/glados.c core/src/vm.c core/src/console.c
INC     = -Icore/inc

all: build/vboard build/test_cluster build/vbasm build/eod_enc build/eod_decode.bin

build/vboard: host/vboard.c $(CORE) core/inc/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) host/vboard.c $(CORE) -o $@ -lpthread

build/test_cluster: core/tests/test_cluster.c $(CORE) core/inc/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) core/tests/test_cluster.c $(CORE) -o $@

build/vbasm: tools/vbasm.c core/inc/vm.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/vbasm.c -o $@

build/eod_enc: tools/eod_enc.c tools/eod_codec.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Itools tools/eod_enc.c -o $@

# ESP On Demand's cluster decoder: assembly -> sandboxed VM bytecode
build/eod_decode.bin: examples/eod_decode.s build/vbasm
	./build/vbasm examples/eod_decode.s -o $@

build/eod_bandtest: tools/eod_bandtest.c $(CORE)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/eod_bandtest.c core/src/vm.c -o $@

test: build/test_cluster
	./build/test_cluster

demo: build/vboard
	python3 tools/drive_cluster.py

# ESP On Demand end-to-end: every frame of a clip decoded by the cluster
eod-e2e: build/vboard build/eod_decode.bin
	python3 tools/eod_e2e.py

clean:
	rm -rf build

.PHONY: all test demo eod-e2e clean
