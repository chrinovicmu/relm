/* guest/guest_kernel_embed.S */
.section .rodata, "a"

.globl guest_kernel_bin 
guest_kernel_bin:
    /* The assembler will find this via the -I flag in the Makefile */
    .incbin "guest_kernel.bin"

.globl guest_kernel_bin_end
guest_kernel_bin_end:
