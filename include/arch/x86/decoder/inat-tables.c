/* x86 opcode map generated from x86-opcode-map.txt */
/* Do not change this code. */

#ifndef __BOOT_COMPRESSED

/* Escape opcode map array */
const insn_attr_t * const inat_escape_tables[INAT_ESC_MAX + 1][INAT_LSTPFX_MAX + 1] = {
};

/* Group opcode map array */
const insn_attr_t * const inat_group_tables[INAT_GRP_MAX + 1][INAT_LSTPFX_MAX + 1] = {
};

/* AVX opcode map array */
const insn_attr_t * const inat_avx_tables[X86_VEX_M_MAX + 1][INAT_LSTPFX_MAX + 1] = {
};

/* XOP opcode map array */
const insn_attr_t * const inat_xop_tables[X86_XOP_M_MAX - X86_XOP_M_MIN + 1] = {
};
#else /* !__BOOT_COMPRESSED */

/* Escape opcode map array */
static const insn_attr_t *inat_escape_tables[INAT_ESC_MAX + 1][INAT_LSTPFX_MAX + 1];

/* Group opcode map array */
static const insn_attr_t *inat_group_tables[INAT_GRP_MAX + 1][INAT_LSTPFX_MAX + 1];

/* AVX opcode map array */
static const insn_attr_t *inat_avx_tables[X86_VEX_M_MAX + 1][INAT_LSTPFX_MAX + 1];

/* XOP opcode map array */
static const insn_attr_t *inat_xop_tables[X86_XOP_M_MAX - X86_XOP_M_MIN + 1];

static void inat_init_tables(void)
{
	/* Print Escape opcode map array */

	/* Print Group opcode map array */

	/* Print AVX opcode map array */

	/* Print XOP opcode map array */
}
#endif
