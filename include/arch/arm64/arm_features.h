#ifndef RELM_ARCH_ARM64_GUEST_FEATURE_REGS_H
#define RELM_ARCH_ARM64_GUEST_FEATURE_REGS_H

#include <linux/types.h>


struct relm_arm64_guest_features{

    
};

struct relm_arm64_guest_feature_regs {
    /*
     * ID_AA64PFR0_EL1, Processor Feature Register 0.
     * Describes execution-level support and facilities including FP,
     * Advanced SIMD, SVE, and RAS. Linux consults this during CPU discovery.
     * SVE occupies bits [35:32]: 0 means absent, 1 means implemented. Other
     * fields have their own encodings; do not generalize that zero rule.
     */
    u64 id_aa64pfr0_el1;

    /*
     * ID_AA64PFR1_EL1, Processor Feature Register 1.
     * Extends discovery with facilities such as BTI, SSBS, MTE, and SME.
     * Linux's choice of corresponding kernel/userspace support must match
     * RELM's controls, exception handling, and per-vCPU saved-state coverage.
     */
    u64 id_aa64pfr1_el1;

    /*
     * ID_AA64PFR2_EL1, Processor Feature Register 2.
     * Further processor extensions, including FPMR and additional MTE
     * capabilities. Keep unsupported facilities unavailable; older hosts
     * cannot be assumed to implement this register or its newer fields.
     */
    u64 id_aa64pfr2_el1;

    /*
     * ID_AA64ISAR0_EL1, Instruction Set Attribute Register 0.
     * Describes instruction families including AES/SHA, CRC32, and atomics.
     * Linux can select optimized routines and expose application HWCAPs
     * from these capabilities. Every advertised instruction must be usable.
     */
    u64 id_aa64isar0_el1;

    /*
     * ID_AA64ISAR1_EL1, Instruction Set Attribute Register 1.
     * Includes pointer-authentication capabilities and further instruction
     * extensions. Advertising pointer authentication also requires key and
     * control-register virtualization, not just support in the host CPU.
     */
    u64 id_aa64isar1_el1;

    /*
     * ID_AA64ISAR2_EL1, Instruction Set Attribute Register 2.
     * Adds instruction capabilities such as MOPS and further authentication
     * extensions. Validate each against guest execution and exception paths;
     * their presence can change the instruction sequences Linux uses.
     */
    u64 id_aa64isar2_el1;

    /*
     * ID_AA64ISAR3_EL1, Instruction Set Attribute Register 3.
     * Further extensions, including FAMINMAX and additional translation/
     * memory-operation instructions. Storage is provided for a controlled
     * reply; do not infer these facilities exist on every supported host.
     */
    u64 id_aa64isar3_el1;

    /*
     * ID_AA64MMFR0_EL1, Memory Model Feature Register 0.
     * Includes physical-address range and supported translation granules.
     * Linux uses such information when configuring its guest stage-1 MMU.
     * Guest-visible widths, RAM placement and RELM's stage-2 geometry must
     * be compatible. This value itself is neither VTCR_EL2 nor a table root.
     */
    u64 id_aa64mmfr0_el1;

    /*
     * ID_AA64MMFR1_EL1, Memory Model Feature Register 1.
     * Includes hardware access/dirty updates, PAN, VMID width and VHE
     * capability reporting. Its guest-visible value is distinct from the
     * host capabilities RELM uses for its own EL2/VMID implementation.
     */
    u64 id_aa64mmfr1_el1;

    /*
     * ID_AA64MMFR2_EL1, Memory Model Feature Register 2.
     * Includes further address-translation, TLB, and virtualization features.
     * Linux may select MMU behavior from these fields. In the initial
     * non-nested VM, do not expose a guest EL2 implementation through them.
     */
    u64 id_aa64mmfr2_el1;

    /*
     * ID_AA64MMFR3_EL1, Memory Model Feature Register 3.
     * Describes newer memory-system extensions, including extended system
     * controls and permission mechanisms. Enabling a feature can require
     * extra guest registers and descriptor handling absent from the baseline.
     */
    u64 id_aa64mmfr3_el1;

    /*
     * ID_AA64MMFR4_EL1, Memory Model Feature Register 4.
     * Adds memory/virtualization capabilities such as ASID extensions and
     * E2H0 support reporting. Keep host EL2 capability detection separate
     * from what an EL1 guest is told about its virtual processor.
     */
    u64 id_aa64mmfr4_el1;

    /*
     * ID_AA64DFR0_EL1, Debug Feature Register 0.
     * Describes architectural debug, breakpoint/watchpoint resources, PMU,
     * and profiling/trace capabilities. Linux perf and debug drivers must
     * not discover resources whose registers RELM cannot isolate/preserve.
     * The supported no-virtual-debug/PMU view needs field-specific encoding.
     */
    u64 id_aa64dfr0_el1;

    /*
     * ID_AA64DFR1_EL1, Debug Feature Register 1.
     * Extends debug/performance discovery, including additional comparator
     * counts and newer facilities. Keep its description consistent with
     * DFR0 and with the actual guest debug/performance-register access policy.
     */
    u64 id_aa64dfr1_el1;

    /*
     * ID_AA64DFR2_EL1, Debug Feature Register 2.
     * Further debug/profiling extensions, including newer step/breakpoint
     * behavior. An explicit field permits a controlled reply without
     * treating physical profiling/debug hardware as guest-owned state.
     */
    u64 id_aa64dfr2_el1;

    /*
     * ID_AA64AFR0_EL1, Auxiliary Feature Register 0.
     * Contains implementation-defined feature information. Generic Linux
     * feature policy cannot assign universal meanings to vendor fields;
     * expose only a deliberately supported virtual-CPU description.
     */
    u64 id_aa64afr0_el1;

    /*
     * ID_AA64AFR1_EL1, Auxiliary Feature Register 1.
     * Companion auxiliary identification register. Honor the selected
     * architecture's reserved/implementation rules rather than copying an
     * arbitrary host value into the guest-visible bank.
     */
    u64 id_aa64afr1_el1;

    /*
     * ID_AA64ZFR0_EL1, SVE Feature Register 0.
     * Describes SVE instruction capabilities/version, not live vector data.
     * Linux must not be offered SVE until Z/P/FFR state and vector-length
     * controls are virtualized. Follow the no-SVE access/readback policy
     * together with PFR0.SVE; clearing this register alone is insufficient.
     */
    u64 id_aa64zfr0_el1;

    /*
     * ID_AA64SMFR0_EL1, SME Feature Register 0.
     * Describes matrix/streaming instruction capabilities, not ZA/ZT0 data.
     * Keep it consistent with PFR1.SME and the initial no-SME execution
     * policy. Future SME support requires substantially more vCPU state.
     */
    u64 id_aa64smfr0_el1;

    /*
     * CTR_EL0, Cache Type Register.
     * Includes instruction/data cache-line and maintenance properties.
     * Linux uses them for cache-maintenance ranges and userspace reporting.
     * These values must match the cache behavior of the allowed host CPUs
     * or a correctly emulated alternative; they are not cosmetic model IDs.
     */
    u64 ctr_el0;

    /*
     * CLIDR_EL1, Cache Level ID Register.
     * Describes cache types/levels and coherency-related levels. Linux's
     * cache discovery and maintenance must see a coherent model. CCSIDR_EL1
     * replies depend on CSSELR_EL1 and need a separate selection-aware model
     * consistent with this value; one static CCSIDR member would not suffice.
     */
    u64 clidr_el1;

    /*
     * DCZID_EL0, Data Cache Zero ID Register.
     * Reports the DC ZVA zeroing-block size and whether its use is prohibited.
     * Guest software may use it to choose memory-zeroing sequences. The
     * reported behavior must agree with DC ZVA execution/trapping; changing
     * this C value does not itself prohibit or resize a hardware DC ZVA.
     */
    u64 dczid_el0;
};

#endif /* RELM_ARCH_ARM64_GUEST_FEATURE_REGS_H */
