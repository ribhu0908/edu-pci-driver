# Build the module against a kernel tree:
#   make KDIR=/lib/modules/$(uname -r)/build        (running kernel)
#   make KDIR=~/linux                                (a configured source tree)
KDIR   ?= /lib/modules/$(shell uname -r)/build
BUILD  := build

.PHONY: all module test-bin initramfs qemu clean

all: module test-bin

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver W=1 modules

test-bin: | $(BUILD)
	$(CC) -static -O2 -Wall -Wextra -Werror tests/edu_test.c -o $(BUILD)/edu_test

$(BUILD):
	mkdir -p $(BUILD)

# initramfs = the test binary as /init + the module
initramfs: module test-bin
	rm -rf $(BUILD)/rootfs && mkdir -p $(BUILD)/rootfs/dev $(BUILD)/rootfs/proc $(BUILD)/rootfs/sys
	cp $(BUILD)/edu_test $(BUILD)/rootfs/init
	cp driver/edu.ko $(BUILD)/rootfs/edu.ko
	cd $(BUILD)/rootfs && find . | cpio -o -H newc --quiet | gzip > ../initramfs.cpio.gz

# Boot the kernel in QEMU with an edu card attached and run the suite
qemu: initramfs
	bash scripts/run-qemu.sh $(KDIR)/arch/x86/boot/bzImage $(BUILD)/initramfs.cpio.gz

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver clean || true
	rm -rf $(BUILD)
