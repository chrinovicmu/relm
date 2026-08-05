cmd_/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o :=  gcc-12 -Wp,-MMD,/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/.stub.o.d -nostdinc -I/usr/src/linux-headers-6.1.0-43-common/arch/x86/include -I./arch/x86/include/generated -I/usr/src/linux-headers-6.1.0-43-common/include -I./include -I/usr/src/linux-headers-6.1.0-43-common/arch/x86/include/uapi -I./arch/x86/include/generated/uapi -I/usr/src/linux-headers-6.1.0-43-common/include/uapi -I./include/generated/uapi -include /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler-version.h -include /usr/src/linux-headers-6.1.0-43-common/include/linux/kconfig.h -D__KERNEL__ -fmacro-prefix-map=/usr/src/linux-headers-6.1.0-43-common/= -D__ASSEMBLY__ -fno-PIE -m64 -DCC_USING_FENTRY -g -I/home/chrinovic/Workspace/Projects/relm -I/home/chrinovic/Workspace/Projects/relm/include -I/home/chrinovic/Workspace/Projects/relm/guest -I/home/chrinovic/Workspace/Projects/relm/include/arch/x86/vmx -I/home/chrinovic/Workspace/Projects/relm/include/arch/x86  -DMODULE  -c -o /home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o /home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.S  ; ./tools/objtool/objtool --hacks=jump_label --hacks=noinstr --orc --retpoline --rethunk --sls --static-call --uaccess   --module /home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o

source_/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o := /home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.S

deps_/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o := \
  /usr/src/linux-headers-6.1.0-43-common/include/linux/compiler-version.h \
    $(wildcard include/config/CC_VERSION_TEXT) \
  /usr/src/linux-headers-6.1.0-43-common/include/linux/kconfig.h \
    $(wildcard include/config/CPU_BIG_ENDIAN) \
    $(wildcard include/config/BOOGER) \
    $(wildcard include/config/FOO) \

/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o: $(deps_/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o)

$(deps_/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o):

/home/chrinovic/Workspace/Projects/relm/src/arch/x86/boot/stub.o: $(wildcard ./tools/objtool/objtool)
