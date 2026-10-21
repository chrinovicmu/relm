/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_ARCH_ARM64_HYP_REGS_H
#define RELM_ARCH_ARM64_HYP_REGS_H

/*
 * HCR_EL2 - Hypervisor Configuration Register, EL2.
 * Controls the guest's execution state, stage-2 address translation,
 * exception routing, and which guest operations enter the hypervisor.
 * RELM runs the host at EL2, guest Linux at EL1, and guest userspace at
 * EL0. The guest-policy descriptions below assume E2H=1 and TGE=0.
 * Clearing a trap bit only disables that particular interception;
 * other architectural controls can still prevent or trap an access.
 */

/*
 * HCR_EL2.TERR [36] - RAS error-record register access trapping.
 * RAS means Reliability, Availability and Serviceability. On CPUs with
 * this extension, setting TERR intercepts the covered error-record
 * accesses so a guest cannot freely inspect or change host error state.
 * RELM enables it when RAS exists; handling an actual SError is separate.
 */
#define HCR_TERR                 (1ULL << 36)

/*
 * HCR_EL2.TLOR [35] - Trap Limited Ordering Region register accesses.
 * LOR is an optional facility for controlling memory ordering in regions.
 * Setting TLOR intercepts its control-register accesses, preventing guest
 * software from directly reconfiguring the physical CPU's LOR state.
 * RELM sets this only when the CPU implements the LOR extension.
 */
#define HCR_TLOR                 (1ULL << 35)

/*
 * HCR_EL2.E2H [34] - EL2 Host mode, part of VHE.
 * the host kernel can run at 
 * EL2 with the associated host translation regime and register aliases.
 * RELM keeps E2H=1 while running either host or guest code. This does not
 * move the guest to EL2; guest Linux still executes at EL1.
 */
#define HCR_E2H                  (1ULL << 34)

/*
 * HCR_EL2.RW [31] - Execution-state selection for lower Exception levels.
 * Where this control applies, 1 selects AArch64 at EL1; 0 selects AArch32
 * if supported. RELM sets it because this backend boots 64-bit Linux.
 * This field selects the instruction-set state, not memory permissions.
 */
#define HCR_RW                   (1ULL << 31)

/*
 * HCR_EL2.TGE [27] - Trap General Exceptions.
 * Together with E2H=1, TGE=1 selects the VHE host regime. RELM clears it
 * before entering guest EL1 and restores it on return to host Linux.
 * TGE=0 lets guest userspace system calls reach guest Linux at EL1 while
 * separately selected virtualization traps still reach RELM at EL2.
 */
#define HCR_TGE                  (1ULL << 27)

/*
 * HCR_EL2.TVM [26] - Trap Virtual Memory control writes.
 * Setting this intercepts writes to the specified EL1 registers,
 * including SCTLR_EL1, TTBR0/1_EL1 and TCR_EL1. Linux uses these to enable
 * its MMU/caches and configure page tables. RELM initially intercepts
 * them so exit handling can enforce its cache/translation policy.
 * Reads of these registers have a separate trap control, TRVM.
 */
#define HCR_TVM                  (1ULL << 26)

/*
 * HCR_EL2.TSW [22] - Trap cache maintenance by Set/Way.
 * Set/way instructions identify physical cache entries rather than a
 * guest virtual address. RELM intercepts these operations so guest Linux
 * cannot freely maintain cache state shared with the host or other VMs.
 */
#define HCR_TSW                  (1ULL << 22)

/*
 * HCR_EL2.TACR [21] - Trap Auxiliary Control Register accesses.
 * ACTLR_EL1 is the Auxiliary Control Register, containing CPU-specific
 * implementation controls. RELM traps guest accesses so the guest cannot
 * directly modify those physical CPU settings.
 */
#define HCR_TACR                 (1ULL << 21)

/*
 * HCR_EL2.TIDCP [20] - Trapping of implementation-defined accesses.
 * Setting this intercepts the CPU-specific accesses covered by this
 * architectural field. RELM uses it to retain control of host-specific
 * functionality; it does not cover every implementation-defined register.
 */
#define HCR_TIDCP                (1ULL << 20)

/*
 * HCR_EL2.TSC [19] - Trap SMC, the Secure Monitor Call instruction.
 * Setting this intercepts guest EL1 SMC requests before they reach host
 * firmware. Guest Linux may use SMC for PSCI firmware services such as
 * CPU startup or shutdown; RELM needs to handle the VM's request at EL2.
 */
#define HCR_TSC                  (1ULL << 19)

/*
 * HCR_EL2.TID3 [18] - Trap identification-register group 3.
 * Covers feature-register reads such as ID_AA64PFR0_EL1. Guest Linux
 * probes these to select supported instructions and kernel code paths.
 * RELM traps them so handlers can report the VM's configured features.
 */
#define HCR_TID3                 (1ULL << 18)

/*
 * HCR_EL2.TID2 [17] - Trap identification-register group 2.
 * Covers cache identification/selection accesses, including the relevant
 * CTR, CLIDR, CCSIDR and CSSELR interfaces. Linux uses them to discover
 * cache properties; RELM intercepts them to provide a controlled VM view.
 */
#define HCR_TID2                 (1ULL << 17)

/*
 * HCR_EL2.TID1 [16] - Trap identification-register group 1.
 * Covers additional identification reads such as REVIDR_EL1, the revision
 * identifier. RELM intercepts these so guest CPU identification can agree
 * with the virtual CPU model rather than expose uncontrolled host values.
 */
#define HCR_TID1                 (1ULL << 16)

/*
 * HCR_EL2.TWE [14] - Trap WFE, the Wait For Event instruction.
 * Guest synchronization code can use WFE while waiting for another CPU.
 * Trapping an eligible wait lets RELM yield or block that vCPU. WFE can
 * complete without trapping, for example if an event is already pending.
 */
#define HCR_TWE                  (1ULL << 14)

/*
 * HCR_EL2.TWI [13] - Trap WFI, the Wait For Interrupt instruction.
 * Guest Linux's idle loop can use WFI. Intercepting an eligible wait lets
 * RELM schedule the physical CPU instead of leaving it idle for this VM.
 * Like WFE, a WFI that completes immediately need not generate a trap.
 */
#define HCR_TWI                  (1ULL << 13)

/*
 * HCR_EL2.BSU [11:10] - Barrier Shareability Upgrade.
 * HCR_BSU_IS encodes 01: applicable guest barriers cover at least the
 * Inner Shareable domain, the CPU group sharing that coherency domain.
 * RELM uses this ordering policy alongside its own DSB/ISB/TLBI sequences.
 * This macro is an encoded field value, not a mask for both BSU bits.
 */
#define HCR_BSU_IS               (1ULL << 10)

/*
 * HCR_EL2.FB [9] - Force Broadcast.
 * Setting this broadens the visibility of the maintenance operations
 * covered by this field. RELM uses it so applicable guest maintenance
 * reaches the required sharing domain; this is not a software TLB flush.
 */
#define HCR_FB                   (1ULL << 9)

/*
 * HCR_EL2.AMO [5] - Physical SError interrupt routing.
 * SError means System Error, used for asynchronous error reporting.
 * With TGE=0, setting AMO routes physical SError from guest execution to
 * EL2, subject to higher-level routing. RELM must handle the host error;
 * this setting alone neither recovers from it nor injects a guest error.
 */
#define HCR_AMO                  (1ULL << 5)

/*
 * HCR_EL2.IMO [4] - Physical IRQ (Interrupt Request) routing.
 * With TGE=0, setting IMO routes physical IRQs from guest execution to
 * EL2, subject to higher-level routing, so the host retains control of
 * physical devices. Delivering a virtual IRQ to guest Linux is separate.
 */
#define HCR_IMO                  (1ULL << 4)

/*
 * HCR_EL2.FMO [3] - Physical FIQ (Fast Interrupt Request) routing.
 * With TGE=0, setting FMO routes physical FIQs from guest execution to
 * EL2, subject to higher-level routing. RELM uses it to keep physical
 * interrupt handling in the host; it does not make a virtual FIQ pending.
 */
#define HCR_FMO                  (1ULL << 3)

/*
 * HCR_EL2.PTW [2] - Protection of guest stage-1 Page Table Walks.
 * Setting this faults a guest page-table walk whose memory access is
 * classified as Device by stage 2. Linux page tables belong in RAM;
 * this prevents those walks from reaching device memory as normal tables.
 */
#define HCR_PTW                  (1ULL << 2)

/*
 * HCR_EL2.SWIO [1] - Set/Way Invalidation Override.
 * Setting this makes applicable executed set/way invalidations perform
 * a clean as well as an invalidate, preserving dirty data before removal
 * from the cache. RELM's TSW trap still needs software handling; SWIO does
 * not execute or emulate an instruction which was trapped.
 */
#define HCR_SWIO                 (1ULL << 1)

/*
 * HCR_EL2.VM [0] - Virtualization MMU: stage-2 translation enable.
 * Setting this translates guest IPAs (Intermediate Physical Addresses)
 * into host physical addresses using VTCR_EL2/VTTBR_EL2. Linux still
 * controls its own stage-1 virtual-address mapping. RELM installs the
 * VM's stage-2 configuration before enabling this guest control word.
 */
#define HCR_VM                   (1ULL << 0)

/*
 * CPTR_EL2 - Architectural Feature Trap Register, EL2.
 * Controls access to processor extensions such as floating point and
 * tracing. These definitions describe E2H=1, the VHE layout RELM uses.
 * The VHE FPEN and TTA positions differ from the E2H=0 TFP/TTA layout.
 * Naming that layout explicitly avoids using the wrong bits at EL2.
 */

/*
 * CPTR_EL2.TAM [30] - Trap Activity Monitor accesses.
 * AMU means Activity Monitors Unit, which supplies CPU activity counters.
 * RELM sets this when AMU exists to intercept guest counter/control
 * accesses instead of exposing the physical CPU's monitoring interface.
 */
#define CPTR_EL2_TAM             (1ULL << 30)

/*
 * CPTR_EL2.TTA [28, VHE] - Trap Trace Access.
 * Setting this intercepts the implemented trace system-register interface.
 * RELM enables it when that interface exists so guest tracing software
 * cannot directly configure host tracing. This is bit 28 in VHE, not
 * the bit-20 TTA position used by the E2H=0 layout.
 */
#define CPTR_EL2_VHE_TTA         (1ULL << 28)

/*
 * CPTR_EL2.FPEN [21:20, VHE] - Floating-point/Advanced SIMD access control.
 * This macro is the two-bit field mask. FIELD_PREP(mask, 3) selects 11,
 * allowing basic FP/AdvSIMD through this control, including the Q-register
 * accesses entry.S uses to switch guest state. Guest Linux separately
 * controls its EL1/EL0 access policy through CPACR_EL1. Enabling access
 * does not save registers or indicate whether the guest has used them.
 */
#define CPTR_EL2_VHE_FPEN_MASK   (0x3ULL << 20)

/*
 * MDCR_EL2 - Monitor Debug Configuration Register, EL2.
 * Controls guest debug/performance-monitor accesses and counter ownership.
 * RELM uses register traps to protect shared physical CPU facilities.
 * These controls do not save breakpoint banks or implement a virtual PMU.
 */

/*
 * MDCR_EL2.TTRF [19] - Trap Trace Filter controls.
 * Trace filters select which execution contributes to a trace. RELM sets
 * this when the interface exists to intercept guest filter-register
 * accesses and retain control of physical tracing configuration.
 */
#define MDCR_EL2_TTRF             (1ULL << 19)

/*
 * MDCR_EL2.TPMS [14] - Trap Performance Monitor Sampling controls.
 * SPE, the Statistical Profiling Extension, samples execution for
 * performance analysis. RELM sets TPMS when SPE exists so guest profiling
 * software cannot directly configure the host's sampling machinery.
 */
#define MDCR_EL2_TPMS             (1ULL << 14)

/*
 * MDCR_EL2.TDRA [11] - Trap Debug ROM Address register accesses.
 * Debug ROM tables describe discoverable debug components. For AArch64,
 * this trap covers MDRAR_EL1, the Monitor Debug ROM Address Register.
 * RELM intercepts it to control the debug discovery information seen
 * by guest software.
 */
#define MDCR_EL2_TDRA             (1ULL << 11)

/*
 * MDCR_EL2.TDOSA [10] - Trap Debug OS-related register accesses.
 * Covers the applicable debug OS-lock, status and power-control accesses.
 * RELM intercepts these so guest debug management cannot directly change
 * the host CPU's debug ownership/power state. Coverage depends on the
 * implemented debug architecture, including optional fine-grained traps.
 */
#define MDCR_EL2_TDOSA            (1ULL << 10)

/*
 * MDCR_EL2.TDA [9] - Trap Debug register Accesses.
 * Intercepts the covered debug registers, including breakpoint/watchpoint
 * controls. RELM uses this to prevent guest Linux from programming host
 * debug state directly. Routing a guest debug exception is a different
 * operation, controlled by TDE; register trapping alone is not a debugger.
 */
#define MDCR_EL2_TDA              (1ULL << 9)

/*
 * MDCR_EL2.TPM [6] - Trap Performance Monitor register accesses.
 * PMU means Performance Monitors Unit, used by tools such as Linux perf.
 * RELM sets TPM when a PMU exists to intercept the covered guest accesses
 * and keep the host's physical counters under host control.
 */
#define MDCR_EL2_TPM              (1ULL << 6)

/*
 * MDCR_EL2.TPMCR [5] - Trap Performance Monitors Control Register access.
 * PMCR_EL0 contains global PMU controls, including counter reset/enable
 * requests. RELM traps it alongside TPM so guest performance tools cannot
 * directly reset or reconfigure the host's counter machinery.
 */
#define MDCR_EL2_TPMCR            (1ULL << 5)

/*
 * MDCR_EL2.HPMN [4:0] - Hypervisor performance-monitor counter partition.
 * This numeric field sets the boundary between event counters available
 * to lower levels and counters reserved for higher-level use. It is not
 * an enable bit; its valid values depend on the implemented PMU. RELM
 * preserves the current host CPU's partition when installing guest traps.
 */
#define MDCR_EL2_HPMN_MASK        0x1fULL

/*
 * CNTHCTL_EL2 - Counter-timer Hypervisor Control Register, EL2.
 * Controls access to the architectural counter and timer interfaces.
 * The counter supplies a time value; a timer compares that time against
 * a deadline and can signal an interrupt. Their access controls differ.
 * These definitions use the E2H=1 (VHE) layout.
 */

/*
 * CNTHCTL_EL2.EL1PCTEN [10, VHE] - EL1 Physical Counter access Enable.
 * With TGE=0, setting this allows guest reads of CNTPCT_EL0 through this
 * EL2 control; guest Linux still controls its userspace access policy.
 * Clearing it causes the covered accesses to trap to EL2. RELM permits
 * counter reads but leaves physical-timer permission EL1PTEN[11] clear
 * for timer emulation. This permits reading time, not owning a comparator
 * or receiving its interrupt. In the E2H=0 layout this field is at bit 0.
 */
#define RELM_CNTHCTL_VHE_EL1PCTEN (1ULL << 10)

#endif /* RELM_ARCH_ARM64_HYP_REGS_H */
