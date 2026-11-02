# SPDX-License-Identifier: GPL-2.0-only
# Native Linux build; vmlinux.h comes from the running target kernel.
BUILD ?= build
CLANG ?= clang-18
BPFTOOL ?= bpftool
CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
DESTDIR ?=
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo source)
ARCH := $(shell uname -m)
TARGET_ARCH := $(if $(filter x86_64,$(ARCH)),x86,$(if $(filter aarch64,$(ARCH)),arm64,$(ARCH)))
MULTIARCH := $(shell $(CC) -dumpmachine)
LIBBPF_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libbpf)
LIBBPF_LIBS ?= $(shell $(PKG_CONFIG) --libs --static libbpf)
CPPFLAGS += -Ivendor -Isrc -Isrc/shared -I$(BUILD) $(LIBBPF_CFLAGS)
CPPFLAGS += -DLERTICK_VERSION=\"$(VERSION)\"
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra
BPF_CFLAGS ?= -O2 -g -target bpf -mcpu=v3 -Wall -Wno-compare-distinct-pointer-types
BPF_CFLAGS += -D__TARGET_ARCH_$(TARGET_ARCH) -Ivendor/bpf-compat -I/usr/include/$(MULTIARCH)
BPF_HEADERS := $(wildcard vendor/scx/*.h) $(wildcard vendor/lerwatt/*.h) $(wildcard src/shared/*.h)

.PHONY: all native install uninstall clean preflight test-model test-accounting test-burst-profiles test-cpuperf test-lerwatt test-deployment test-integration-abi test-report workload reference
all: $(BUILD)/scx_lertick $(BUILD)/burst_workload $(BUILD)/phase2_workload $(BUILD)/lifecycle_workload

native:
	scripts/native_build.sh

install:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./deploy.sh install

uninstall:
	PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" ./deploy.sh uninstall

$(BUILD):
	mkdir -p $@

$(BUILD)/vmlinux.h: | $(BUILD)
	@test -r /sys/kernel/btf/vmlinux || { echo 'Missing running-kernel BTF at /sys/kernel/btf/vmlinux' >&2; exit 1; }
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@.tmp
	mv $@.tmp $@

$(BUILD)/scx_lertick.bpf.o: src/bpf/scx_lertick.bpf.c $(BUILD)/vmlinux.h $(BPF_HEADERS)
	$(CLANG) $(CPPFLAGS) $(BPF_CFLAGS) -c $< -o $@

$(BUILD)/scx_lertick.bpf.skel.h: $(BUILD)/scx_lertick.bpf.o
	$(BPFTOOL) gen skeleton $< name scx_lertick > $@.tmp
	mv $@.tmp $@

$(BUILD)/scx_lertick: src/userspace/scx_lertick.c src/userspace/lerwatt_plugin.c src/userspace/lerwatt_plugin.h src/userspace/runtime.c src/userspace/runtime.h $(BUILD)/scx_lertick.bpf.skel.h $(BPF_HEADERS)
	$(CC) $(CPPFLAGS) -Isrc/userspace $(CFLAGS) src/userspace/scx_lertick.c src/userspace/lerwatt_plugin.c src/userspace/runtime.c -o $@ $(LIBBPF_LIBS) -lelf -lz

$(BUILD)/test_loader_runtime: tests/test_loader_runtime.c src/userspace/runtime.c src/userspace/runtime.h | $(BUILD)
	$(CC) -Isrc/userspace -O2 -std=gnu11 -Wall -Wextra -Werror tests/test_loader_runtime.c src/userspace/runtime.c -o $@

test-deployment: $(BUILD)/test_loader_runtime
	python3 tests/check_integration_abi.py
	$(BUILD)/test_loader_runtime
	sh tests/test_deploy.sh
	sh tests/test_service.sh
	python3 tests/test_report.py

test-integration-abi:
	python3 tests/check_integration_abi.py

test-report:
	python3 tests/test_report.py

$(BUILD)/burst_workload: benchmarks/burst_workload.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -pthread -lm

$(BUILD)/phase2_workload: benchmarks/phase2_workload.c benchmarks/burst_profiles.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -pthread -lm

$(BUILD)/lifecycle_workload: tests/lifecycle_workload.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ -pthread -lm

$(BUILD)/test_model: tests/test_model.c src/shared/model.h | $(BUILD)
	$(CC) -Isrc/shared -O2 -std=c11 -Wall -Wextra -Werror $< -o $@

test-model: $(BUILD)/test_model
	$(BUILD)/test_model

$(BUILD)/test_accounting: tests/test_accounting.c src/shared/accounting.h src/shared/model.h | $(BUILD)
	$(CC) -Isrc/shared -O2 -std=c11 -Wall -Wextra -Werror $< -o $@

test-accounting: $(BUILD)/test_accounting
	$(BUILD)/test_accounting

$(BUILD)/test_burst_profiles: tests/test_burst_profiles.c benchmarks/burst_profiles.h | $(BUILD)
	$(CC) -Ibenchmarks -O2 -std=c11 -Wall -Wextra -Werror $< -o $@

test-burst-profiles: $(BUILD)/test_burst_profiles
	$(BUILD)/test_burst_profiles

$(BUILD)/test_cpuperf: tests/test_cpuperf.c src/shared/cpuperf.h src/shared/model.h | $(BUILD)
	$(CC) -Isrc/shared -O2 -std=c11 -Wall -Wextra -Werror $< -o $@ -lm

test-cpuperf: $(BUILD)/test_cpuperf
	$(BUILD)/test_cpuperf

$(BUILD)/test_lerwatt_plugin: tests/test_lerwatt_plugin.c src/userspace/lerwatt_plugin.c src/userspace/lerwatt_plugin.h src/shared/lertick.h src/shared/cpuperf.h vendor/lerwatt/contract.h | $(BUILD)
	$(CC) -Ivendor -Isrc/shared -Isrc/userspace -O2 -std=gnu11 -Wall -Wextra -Werror tests/test_lerwatt_plugin.c src/userspace/lerwatt_plugin.c -o $@

test-lerwatt: $(BUILD)/test_lerwatt_plugin
	$(BUILD)/test_lerwatt_plugin

workload: $(BUILD)/burst_workload $(BUILD)/phase2_workload
preflight:
	scripts/preflight.sh
reference:
	scripts/fetch_reference.sh
clean:
	rm -f $(BUILD)/vmlinux.h $(BUILD)/scx_lertick.bpf.o $(BUILD)/scx_lertick.bpf.skel.h $(BUILD)/scx_lertick $(BUILD)/burst_workload $(BUILD)/phase2_workload $(BUILD)/lifecycle_workload $(BUILD)/test_model $(BUILD)/test_accounting $(BUILD)/test_burst_profiles $(BUILD)/test_cpuperf $(BUILD)/test_lerwatt_plugin $(BUILD)/test_loader_runtime
