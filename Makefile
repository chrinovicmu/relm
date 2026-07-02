# Makefile — RELM hypervisor, multi-architecture Kbuild
MODULE_NAME := relm

AS      ?= $(CROSS_COMPILE)as
OBJCOPY ?= $(CROSS_COMPILE)objcopy

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ARCH      ?= x86
SUBTARGET ?= vmx

SUPPORTED_ARCHS := x86 arm64 riscv
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHS)),)
    $(error ARCH='$(ARCH)' is not supported. Supported: $(SUPPORTED_ARCHS))
endif

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

ifeq ($(SUBTARGET),)
    ARCH_INCLUDE_DIR := $(src)/include/arch/$(ARCH)
else
    ARCH_INCLUDE_DIR := $(src)/include/arch/$(ARCH)/$(SUBTARGET)
endif

ifeq ($(SUBTARGET),)
    ARCH_SHARED_DIR :=
else
    ARCH_SHARED_DIR := $(src)/include/arch/$(ARCH)
endif

ifeq ($(SUBTARGET),)
    ARCH_SRC_DIR := src/arch/$(ARCH)
else
    ARCH_SRC_DIR := src/arch/$(ARCH)/$(SUBTARGET)
endif

ARCH_C_SRCS  := $(wildcard $(src)/$(ARCH_SRC_DIR)/*.c)
ARCH_C_OBJS  := $(patsubst $(src)/%.c, %.o, $(ARCH_C_SRCS))

ARCH_S_SRCS  := $(wildcard $(src)/$(ARCH_SRC_DIR)/*.S)
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
    src/boot/linux_loader.o \
    src/boot/stub.o         \
    src/firmware/e820.o     \
    src/firmware/fw_cfg.o   \
    src/firmware/seabios.o

MODULE_OBJS := \
    src/core/relm.o 

TRACE_OBJS := \
    src/tracing/ebpf/relm_trace.o

GUEST_OBJS := \
    guest/guest_kernel_embed.o

$(MODULE_NAME)-y := \
    $(MODULE_OBJS)  \
    $(CORE_OBJS)    \
    $(BOOT_OBJS)    \
    $(ARCH_OBJS)    \
    $(GUEST_OBJS)

ARCH_UPPER      := $(shell echo $(ARCH)      | tr a-z A-Z)
SUBTARGET_UPPER := $(shell echo $(SUBTARGET) | tr a-z A-Z)

ccflags-y := \
    -I$(src)                    \
    -I$(src)/utils              \
    -I$(ARCH_INCLUDE_DIR)       \
    -DRELM_ARCH_$(ARCH_UPPER)

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

.PHONY: all modules clean info

all: modules

guest/guest_kernel.bin: guest/guest_kernel.S
	@echo "  AS       $< → guest/guest_kernel.o"
	$(AS) --64 -o guest/guest_kernel.o $<

	@echo "  OBJCOPY  guest/guest_kernel.o → $@"
	$(OBJCOPY) --output-target binary guest/guest_kernel.o $@

	@printf "  GUEST    %d bytes → $@\n" "$$(wc -c < $@)"

modules: guest/guest_kernel.bin
	@echo ""
	@echo "  RELM build configuration:"
	@echo "    ARCH      = $(ARCH)"
	@echo "    SUBTARGET = $(if $(SUBTARGET),$(SUBTARGET),(none))"
	@echo "    ARCH_DIR  = $(ARCH_INCLUDE_DIR)"
	@echo "    ARCH_SRCS = $(ARCH_OBJS)"
	@echo ""
	$(MAKE) -C $(KDIR) M=$(PWD) \
	    ARCH=$(ARCH)             \
	    SUBTARGET=$(SUBTARGET)   \
	    modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f guest/guest_kernel.o \
	      guest/guest_kernel.bin

info:
	@echo "========================================"
	@echo "  RELM build configuration"
	@echo "========================================"
	@echo "  MODULE_NAME      = $(MODULE_NAME)"
	@echo "  ARCH             = $(ARCH)"
	@echo "  SUBTARGET        = $(if $(SUBTARGET),$(SUBTARGET),(none))"
	@echo "  ARCH_INCLUDE_DIR = $(ARCH_INCLUDE_DIR)"
	@echo "  ARCH_SHARED_DIR  = $(if $(ARCH_SHARED_DIR),$(ARCH_SHARED_DIR),(none))"
	@echo "  KDIR             = $(KDIR)"
	@echo "  CROSS_COMPILE    = $(if $(CROSS_COMPILE),$(CROSS_COMPILE),(native))"
	@echo ""
	@echo "  Arch source objects:"
	@$(foreach obj,$(ARCH_OBJS),echo "    $(obj)";)
	@echo ""
	@echo "  Core objects:"
	@$(foreach obj,$(CORE_OBJS),echo "    $(obj)";)
	@echo ""
	@echo "  ccflags-y:"
	@echo "    $(ccflags-y)" | tr ' ' '\n' | sed 's/^/    /'
