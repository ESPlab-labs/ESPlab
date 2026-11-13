# Makefile — ESPlab host build (the virtual boards and tests)
CC      ?= cc
CFLAGS  ?= -Wall -Wextra -std=gnu11 -O2
CORE    = core/src/glados.c core/src/vm.c core/src/console.c
INC     = -Icore/inc

all: build/vboard build/test_cluster build/vbasm

build/vboard: host/vboard.c $(CORE) core/inc/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) host/vboard.c $(CORE) -o $@ -lpthread

build/test_cluster: core/tests/test_cluster.c $(CORE) core/inc/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) core/tests/test_cluster.c $(CORE) -o $@

build/vbasm: tools/vbasm.c core/inc/vm.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) tools/vbasm.c -o $@

test: build/test_cluster
	./build/test_cluster

demo: build/vboard
	python3 tools/drive_cluster.py

clean:
	rm -rf build

.PHONY: all test demo clean
