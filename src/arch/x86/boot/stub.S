.global relm_idt_stub_template
.global relm_idt_stub_template_end

.align 4
relm_idt_stub_template:
    movl $0x00000000, %eax  /* imm32 path will be done in C */
    vmcall                  /* 3 bytes */
    .fill 24, 1, 0xF4       /* Pad out the rest of the stride with HLT (0xF4) */
relm_idt_stub_template_end:

