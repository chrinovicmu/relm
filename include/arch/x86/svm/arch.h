#ifndef RELM_ARCH_X86_SVM_ARCH_H
#define RELM_ARCH_X86_SVM_ARCH_H

struct relm_vm;

#ifndef RELM_ARCH_NAME
#define RELM_ARCH_NAME "x86-svm"
#endif

/*
 * relm_arch_init   — one-time global bring-up of the virt extension
 *                    (SVM: EFER.SVME on all CPUs). Returns 0 / -errno.
 * relm_arch_exit   — global teardown (clear EFER.SVME on all CPUs).
 * relm_arch_vm_ready        — arch logs/finalises per-VM state after the VM
 *                             is built (nCR3 on x86-svm, VTTBR on arm64).
 * relm_arch_iommu_enabled   — true if the arch wired an IOMMU/SMMU domain.
 *
 */
int  relm_arch_init(void);
void relm_arch_exit(void);
void relm_arch_vm_ready(struct relm_vm *vm);
bool relm_arch_iommu_enabled(struct relm_vm *vm);

#endif /* RELM_ARCH_X86_SVM_ARCH_H */
