CC ?= cc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS ?=

HOST_OS := $(shell uname -s)
HOST_ARCH := $(shell uname -m)

SOURCES := src/main.c src/protocol.c src/supervisor.c
ifeq ($(HOST_OS)-$(HOST_ARCH),Linux-x86_64)
SOURCES += src/kvm_linux_x86_64.c src/kvm_boot_linux_x86_64.c
else ifeq ($(HOST_OS)-$(HOST_ARCH),Linux-aarch64)
SOURCES += src/kvm_linux_aarch64.c src/kvm_boot_linux_aarch64.c
else
SOURCES += src/kvm_stub.c src/kvm_boot_stub.c
endif
OBJECTS := $(SOURCES:.c=.o)
TARGET := agent-cell

.PHONY: all clean test sanitize guest kernel-aarch64

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

test: $(TARGET)
	./$(TARGET) selftest
	./$(TARGET) run --memory-mib 32 --timeout-ms 500 -- /bin/echo hello
	@if [ "$(HOST_OS)" = "Linux" ] && [ -r /dev/kvm ]; then ./$(TARGET) kvm-smoke; else echo "kvm-smoke=skip host=$(HOST_OS)-$(HOST_ARCH)"; fi

sanitize: CFLAGS += -O1 -g -fsanitize=address,undefined
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean $(TARGET)
	./$(TARGET) selftest

guest:
	./scripts/build-initramfs.sh

kernel-aarch64:
	@test -n "$(KERNEL_SRC)" || (echo "usage: make kernel-aarch64 KERNEL_SRC=/path/to/linux" >&2; exit 2)
	./scripts/build-kernel-aarch64.sh "$(KERNEL_SRC)"

clean:
	rm -f src/*.o $(TARGET)
