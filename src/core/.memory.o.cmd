cmd_/home/chrinovic/Workspace/Projects/relm/src/core/memory.o :=  gcc-12 -Wp,-MMD,/home/chrinovic/Workspace/Projects/relm/src/core/.memory.o.d -nostdinc -I/usr/src/linux-headers-6.1.0-43-common/arch/x86/include -I./arch/x86/include/generated -I/usr/src/linux-headers-6.1.0-43-common/include -I./include -I/usr/src/linux-headers-6.1.0-43-common/arch/x86/include/uapi -I./arch/x86/include/generated/uapi -I/usr/src/linux-headers-6.1.0-43-common/include/uapi -I./include/generated/uapi -include /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler-version.h -include /usr/src/linux-headers-6.1.0-43-common/include/linux/kconfig.h -include /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler_types.h -D__KERNEL__ -fmacro-prefix-map=/usr/src/linux-headers-6.1.0-43-common/= -Wall -Wundef -Werror=strict-prototypes -Wno-trigraphs -fno-strict-aliasing -fno-common -fshort-wchar -fno-PIE -Werror=implicit-function-declaration -Werror=implicit-int -Werror=return-type -Wno-format-security -std=gnu11 -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx -fcf-protection=none -m64 -falign-jumps=1 -falign-loops=1 -mno-80387 -mno-fp-ret-in-387 -mpreferred-stack-boundary=3 -mskip-rax-setup -mtune=generic -mno-red-zone -mcmodel=kernel -Wno-sign-compare -fno-asynchronous-unwind-tables -mindirect-branch=thunk-extern -mindirect-branch-register -mindirect-branch-cs-prefix -mfunction-return=thunk-extern -fno-jump-tables -mharden-sls=all -fno-delete-null-pointer-checks -Wno-frame-address -Wno-format-truncation -Wno-format-overflow -Wno-address-of-packed-member -O2 -fno-allow-store-data-races -Wframe-larger-than=2048 -fstack-protector-strong -Wno-main -Wno-unused-but-set-variable -Wno-unused-const-variable -Wno-dangling-pointer -ftrivial-auto-var-init=zero -fno-stack-clash-protection -pg -mrecord-mcount -mfentry -DCC_USING_FENTRY -Wvla -Wno-pointer-sign -Wcast-function-type -Wno-stringop-truncation -Wno-stringop-overflow -Wno-restrict -Wno-maybe-uninitialized -Wno-array-bounds -Wno-alloc-size-larger-than -Wimplicit-fallthrough=5 -fno-strict-overflow -fno-stack-check -fconserve-stack -Werror=date-time -Werror=incompatible-pointer-types -Werror=designated-init -fno-builtin-wcslen -Wno-packed-not-aligned -g -I/home/chrinovic/Workspace/Projects/relm -I/home/chrinovic/Workspace/Projects/relm/include -I/home/chrinovic/Workspace/Projects/relm/utils -I/home/chrinovic/Workspace/Projects/relm/include/arch/x86/vmx -DRELM_ARCH_X86 -I/home/chrinovic/Workspace/Projects/relm/include/arch/x86/decoder -I/home/chrinovic/Workspace/Projects/relm/include/arch/x86 -DRELM_SUBTARGET_VMX  -DMODULE  -DKBUILD_BASENAME='"memory"' -DKBUILD_MODNAME='"relm"' -D__KBUILD_MODNAME=kmod_relm -c -o /home/chrinovic/Workspace/Projects/relm/src/core/memory.o /home/chrinovic/Workspace/Projects/relm/src/core/memory.c   ; ./tools/objtool/objtool --hacks=jump_label --hacks=noinstr --orc --retpoline --rethunk --sls --static-call --uaccess   --module /home/chrinovic/Workspace/Projects/relm/src/core/memory.o

source_/home/chrinovic/Workspace/Projects/relm/src/core/memory.o := /home/chrinovic/Workspace/Projects/relm/src/core/memory.c

deps_/home/chrinovic/Workspace/Projects/relm/src/core/memory.o := \
  /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler-version.h \
    $(wildcard include/config/CC_VERSION_TEXT) \
  /usr/src/linux-headers-6.1.0-43-common/include/linux/kconfig.h \
    $(wildcard include/config/CPU_BIG_ENDIAN) \
    $(wildcard include/config/BOOGER) \
    $(wildcard include/config/FOO) \
  /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler_types.h \
    $(wildcard include/config/DEBUG_INFO_BTF) \
    $(wildcard include/config/PAHOLE_HAS_BTF_TAG) \
    $(wildcard include/config/HAVE_ARCH_COMPILER_H) \
    $(wildcard include/config/CC_HAS_ASM_INLINE) \
  /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler_attributes.h \
  /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler-gcc.h \
    $(wildcard include/config/RETPOLINE) \
    $(wildcard include/config/GCC_ASM_GOTO_OUTPUT_WORKAROUND) \
    $(wildcard include/config/ARCH_USE_BUILTIN_BSWAP) \
    $(wildcard include/config/SHADOW_CALL_STACK) \
    $(wildcard include/config/KCOV) \

/home/chrinovic/Workspace/Projects/relm/src/core/memory.o: $(deps_/home/chrinovic/Workspace/Projects/relm/src/core/memory.o)

$(deps_/home/chrinovic/Workspace/Projects/relm/src/core/memory.o):

/home/chrinovic/Workspace/Projects/relm/src/core/memory.o: $(wildcard ./tools/objtool/objtool)
