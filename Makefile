# Makefile — RELM hypervisor, multi-architecture Kbuild
MODULE_NAME := relm

ARCH      ?= x86
SUBTARGET ?= vmx

SUPPORTED_ARCHS := x86 arm64 riscv
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHS)),)
    $(error ARCH='$(ARCH)' is not supported. Supported: $(SUPPORTED_ARCHS))
endif

# Zydis is compiled from pinned upstream sources instead of linked as a host
# userspace library.
ZYDIS_CPPFLAGS := \
    -DZYAN_NO_LIBC             \
    -DZYCORE_STATIC_BUILD      \
    -DZYDIS_STATIC_BUILD       \
    -DZYDIS_DISABLE_ENCODER    \
    -DZYDIS_DISABLE_FORMATTER  \
    -DZYDIS_DISABLE_SEGMENT    \
    -DZYDIS_DISABLE_AVX512     \
    -DZYDIS_DISABLE_KNC

ZYDIS_C_NAMES := \
    MetaInfo.c    \
    Mnemonic.c    \
    Register.c    \
    SharedData.c  \
    String.c      \
    Utils.c       \
    Zydis.c       \
    Decoder.c     \
    DecoderData.c

ifeq ($(ARCH),x86)
    SUPPORTED_SUBTARGETS := vmx svm
    ifeq ($(filter $(SUBTARGET),$(SUPPORTED_SUBTARGETS)),)
        $(error SUBTARGET='$(SUBTARGET)' not valid for ARCH=x86. \
                Supported: $(SUPPORTED_SUBTARGETS))
    endif
else
    ifneq ($(SUBTARGET),)
        $(warning SUBTARGET='$(SUBTARGET)' ignored for ARCH=$(ARCH))
        SUBTARGET :=
    endif
endif

ifneq ($(KERNELRELEASE),)

ifeq ($(SUBTARGET),)
    ARCH_INCLUDE_DIR := $(src)/include/arch/$(ARCH)
    ARCH_SRC_DIR     := src/arch/$(ARCH)
else
    ARCH_INCLUDE_DIR := $(src)/include/arch/$(ARCH)/$(SUBTARGET)
    ARCH_SRC_DIR     := src/arch/$(ARCH)/$(SUBTARGET)
endif

ifeq ($(SUBTARGET),)
    ARCH_SHARED_DIR :=
else
    ARCH_SHARED_DIR := $(src)/include/arch/$(ARCH)
endif

ARCH_C_SRCS  := $(wildcard $(src)/$(ARCH_SRC_DIR)/*.c)
ARCH_S_SRCS  := $(wildcard $(src)/$(ARCH_SRC_DIR)/*.S)

ifeq ($(ARCH),x86)
    ARCH_C_SRCS += $(src)/src/arch/x86/decoder/utils.c
    ARCH_C_SRCS += $(addprefix $(src)/third_party/zydis/src/,$(ZYDIS_C_NAMES))

    ZYDIS_C_OBJS := $(addprefix third_party/zydis/src/,$(ZYDIS_C_NAMES:.c=.o))
    $(foreach zydis_obj,$(ZYDIS_C_OBJS), \
        $(eval CFLAGS_$(zydis_obj) += -Wno-unused-function \
                                      -Wno-unused-const-variable))
endif

ARCH_C_OBJS  := $(patsubst $(src)/%.c, %.o, $(ARCH_C_SRCS))
ARCH_S_OBJS  := $(patsubst $(src)/%.S, %.o, $(ARCH_S_SRCS))
ARCH_OBJS    := $(ARCH_C_OBJS) $(ARCH_S_OBJS)

obj-m := $(MODULE_NAME).o

CORE_OBJS := \
    src/core/vm.o      \
    src/core/vcpu.o    \
    src/core/memory.o  \
    src/core/iommu.o   \
    src/core/decoder.o \
    src/core/relm.o

BOOT_OBJS := \
    src/arch/x86/boot/boot.o \
    src/arch/x86/boot/stub.o \
    src/firmware/e820.o      \
    src/firmware/fw_cfg.o    \
    src/firmware/seabios.o

VIRTIO_OBJS := \
    src/virtio/mmio.o   \
    src/virtio/virtio.o

GUEST_OBJS := \
    guest/guest_kernel_embed.o

DEBUG_OBJS := \
    src/debug/insn_dump.o 

$(MODULE_NAME)-y := \
    $(CORE_OBJS)    \
    $(BOOT_OBJS)    \
    $(VIRTIO_OBJS)  \
    $(DEBUG_OBJS)   \
    $(ARCH_OBJS)    \
    $(GUEST_OBJS)

ARCH_UPPER      := $(shell echo $(ARCH)      | tr a-z A-Z)
SUBTARGET_UPPER := $(shell echo $(SUBTARGET) | tr a-z A-Z)

ccflags-y := \
    -I$(src)                    \
    -I$(src)/include            \
    -I$(src)/utils              \
    -I$(ARCH_INCLUDE_DIR)       \
    -DRELM_ARCH_$(ARCH_UPPER)

ifeq ($(ARCH),x86)
    ccflags-y += \
        -I$(src)/third_party/zydis/include \
        -I$(src)/third_party/zydis/src \
        $(ZYDIS_CPPFLAGS)
endif

ifneq ($(ARCH_SHARED_DIR),)
    ccflags-y += -I$(ARCH_SHARED_DIR)
endif

ifneq ($(SUBTARGET),)
    ccflags-y += -DRELM_SUBTARGET_$(SUBTARGET_UPPER)
endif

asflags-y := \
    -I$(src)              \
    -I$(src)/include      \
    -I$(src)/guest        \
    -I$(ARCH_INCLUDE_DIR)

ifneq ($(ARCH_SHARED_DIR),)
    asflags-y += -I$(ARCH_SHARED_DIR)
endif

else

AS      ?= $(CROSS_COMPILE)as
OBJCOPY ?= $(CROSS_COMPILE)objcopy

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all modules clean info decoder-test FORCE
.DEFAULT_GOAL := all

# The decoder and its public contract intentionally contain no kernel-only
# runtime dependencies. This tiny host binary therefore exercises the same
# adapter and generic dispatcher used by the module, making prefix/register-
# lane regressions testable even on a development machine without Linux Kbuild.
DECODER_TEST_BIN := /tmp/relm-decoder-test

# The generic layer (include/relm/*) reaches arch-specific headers through the
# stable prefix <relm_arch/...>. We wire that prefix to the active arch backend
# with a symlink instead of hardcoding the path in generic headers, so adding a
# new backend only re-points this link. Resolves via -I$(src)/include.
RELM_ARCH_LINK    := include/relm_arch
RELM_ARCH_TARGET  := arch/$(ARCH)$(if $(SUBTARGET),/$(SUBTARGET),)

# FORCE prerequisite: re-run on every build so switching ARCH/SUBTARGET
# re-points an existing link instead of silently keeping the previous
# backend's headers (ln -sfn is idempotent when nothing changed).
$(RELM_ARCH_LINK): FORCE
	@echo "  LN        $(RELM_ARCH_LINK) -> $(RELM_ARCH_TARGET)"
	ln -sfn $(RELM_ARCH_TARGET) $(RELM_ARCH_LINK)

FORCE:

all: modules

guest/guest_kernel.bin: guest/guest_kernel.S
	@echo "  AS        $< → guest/guest_kernel.o"
	$(AS) --64 -o guest/guest_kernel.o $<

	@echo "  OBJCOPY   guest/guest_kernel.o → $@"
	$(OBJCOPY) --output-target binary guest/guest_kernel.o $@

	@printf "  GUEST     %d bytes → $@\n" "$$(wc -c < $@)"

modules: guest/guest_kernel.bin $(RELM_ARCH_LINK)
	@echo ""
	@echo "  RELM build configuration:"
	@echo "    ARCH      = $(ARCH)"
	@echo "    SUBTARGET = $(if $(SUBTARGET),$(SUBTARGET),(none))"
	@echo ""
	$(MAKE) -C $(KDIR) M=$(PWD) modules

decoder-test:
	$(CC) -std=c11 -Wall -Wextra -Werror \
		-Wno-unused-function -Wno-unused-const-variable \
		-DCONFIG_X86=1 $(ZYDIS_CPPFLAGS) \
		-I$(PWD)/include \
		-I$(PWD)/third_party/zydis/include \
		-I$(PWD)/third_party/zydis/src \
		tests/decoder/decoder_test.c \
		src/core/decoder.c \
		src/arch/x86/decoder/utils.c \
		$(addprefix third_party/zydis/src/,$(ZYDIS_C_NAMES)) \
		-o $(DECODER_TEST_BIN)
	$(DECODER_TEST_BIN)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f guest/guest_kernel.o \
	      guest/guest_kernel.bin
	rm -f $(RELM_ARCH_LINK)

info:
	@echo "========================================"
	@echo "  RELM build configuration"
	@echo "========================================"
	@echo "  MODULE_NAME      = $(MODULE_NAME)"
	@echo "  ARCH             = $(ARCH)"
	@echo "  SUBTARGET        = $(if $(SUBTARGET),$(SUBTARGET),(none))"
	@echo "  KDIR             = $(KDIR)"
	@echo "  CROSS_COMPILE    = $(if $(CROSS_COMPILE),$(CROSS_COMPILE),(native))"

endif
