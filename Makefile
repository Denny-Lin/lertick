# SPDX-License-Identifier: GPL-2.0-only
# Standalone Linux build; vmlinux.h comes from the running guest kernel.
BUILD ?= build
CLANG ?= clang-18
BPFTOOL ?= bpftool
CC ?= cc
PKG_CONFIG ?= pkg-config
ARCH := $(shell uname -m)
TARGET_ARCH := $(if $(filter x86_64,$(ARCH)),x86,$(if $(filter aarch64,$(ARCH)),arm64,$(ARCH)))
MULTIARCH := $(shell $(CC) -dumpmachine)
LIBBPF_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libbpf)
LIBBPF_LIBS ?= $(shell $(PKG_CONFIG) --libs --static libbpf)
CPPFLAGS += -Ivendor -Isrc -Isrc/shared -I$(BUILD) $(LIBBPF_CFLAGS)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra
BPF_CFLAGS ?= -O2 -g -target bpf -mcpu=v3 -Wall -Wno-compare-distinct-pointer-types
BPF_CFLAGS += -D__TARGET_ARCH_$(TARGET_ARCH) -Ivendor/bpf-compat -I/usr/include/$(MULTIARCH)
BPF_HEADERS := $(wildcard vendor/scx/*.h) $(wildcard src/shared/*.h)

.PHONY: all clean preflight test-model workload reference
all: $(BUILD)/scx_sprout $(BUILD)/burst_workload

$(BUILD):
	mkdir -p $@

$(BUILD)/vmlinux.h: | $(BUILD)
	@test -r /sys/kernel/btf/vmlinux || { echo 'Missing running-kernel BTF at /sys/kernel/btf/vmlinux' >&2; exit 1; }
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@.tmp
	mv $@.tmp $@

$(BUILD)/scx_sprout.bpf.o: src/bpf/scx_sprout.bpf.c $(BUILD)/vmlinux.h $(BPF_HEADERS)
	$(CLANG) $(CPPFLAGS) $(BPF_CFLAGS) -c $< -o $@

$(BUILD)/scx_sprout.bpf.skel.h: $(BUILD)/scx_sprout.bpf.o
	$(BPFTOOL) gen skeleton $< name scx_sprout > $@.tmp
	mv $@.tmp $@

$(BUILD)/scx_sprout: src/userspace/scx_sprout.c $(BUILD)/scx_sprout.bpf.skel.h $(BPF_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LIBBPF_LIBS) -lelf -lz

$(BUILD)/burst_workload: benchmarks/burst_workload.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -pthread -lm

$(BUILD)/test_model: tests/test_model.c src/shared/model.h | $(BUILD)
	$(CC) -Isrc/shared -O2 -std=c11 -Wall -Wextra -Werror $< -o $@

test-model: $(BUILD)/test_model
	$(BUILD)/test_model

workload: $(BUILD)/burst_workload
preflight:
	scripts/preflight.sh
reference:
	scripts/fetch_reference.sh
clean:
	rm -f $(BUILD)/vmlinux.h $(BUILD)/scx_sprout.bpf.o $(BUILD)/scx_sprout.bpf.skel.h $(BUILD)/scx_sprout $(BUILD)/burst_workload $(BUILD)/test_model
