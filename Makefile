# ============================================================================
# Makefile — RELM hypervisor (Kbuild Mangle Bypass)
# ============================================================================

MODULE_NAME := relm

# Tools and Paths
AS      ?= $(CROSS_COMPILE)as
OBJCOPY ?= $(CROSS_COMPILE)objcopy
KDIR    ?= /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)

# Kbuild variables
obj-m      := $(MODULE_NAME).o
ccflags-y  := -I$(src) -I$(src)/utils

# FIX: Tell the assembler to look for .incbin files in your local guest/ folder.
# We use $(src) which is the kbuild-safe way to reference your source.
asflags-y  := -I$(src)/guest

$(MODULE_NAME)-y := \
    src/module.o      \
    src/vm.o          \
    src/vmx.o         \
    src/vmx_asm.o     \
    src/vmexit.o      \
    src/ept.o         \
    src/apic.o        \
    guest/guest_kernel_embed.o

.PHONY: all modules clean

all: modules

# PHASE A: Local build of the guest binary.
guest/guest_kernel.bin: guest/guest_kernel.S
	$(AS) --64 -o guest/guest_kernel.o $<
	$(OBJCOPY) --output-target binary guest/guest_kernel.o $@
	@printf "  GUEST    %d bytes → $@\n" "$$(wc -c < $@)"

# PHASE B: Invoke Kbuild.
modules: guest/guest_kernel.bin
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f guest/guest_kernel.o \
	      guest/guest_kernel.bin
