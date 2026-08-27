#ifndef RELM_SVM_MMU_H
#define RELM_SVM_MMU_H

/*
 * mmu.h — guest-virtual -> guest-physical address translation for the SVM
 * backend ("the software MMU walker").
 */

#include <linux/types.h>

struct vcpu; 

/*
 * relm_mmu_rip_to_linear() — convert a CS-relative RIP into a guest linear
 * address by applying x86 segmentation.
 */
uint64_t relm_mmu_rip_to_linear(struct vcpu *vcpu, uint64_t rip);

/*
 * relm_mmu_gva_to_gpa() — software page walk: translate one guest linear
 * address to the guest-physical address it maps to, using the guest's live
 * paging state (vmcb->save.cr0/cr3/cr4/efer).
 *
 * Handles all three paging modes the guest can be in — identical
 * breakdown to the VMX version:
 *   CR0.PG=0                → no paging: GPA == GVA (identity),
 *   CR0.PG=1, CR4.PAE=0     → 2-level 32-bit paging (4K + CR4.PSE 4M pages),
 *   CR0.PG=1, CR4.PAE=1,
 *     EFER.LMA=0            → 3-level PAE paging (4K + 2M pages),
 *   EFER.LMA=1              → 4-level long-mode paging (4K + 2M + 1G pages).
 *
 * The translation is valid ONLY for the 4K page containing gva — callers
 * copying ranges must re-translate at each page boundary (or just use
 * relm_mmu_copy_from_guest_virt()).
 *
 * Returns 0 and stores the translation in *gpa, or:
 *   -EFAULT  a page-table entry on the walk path is not-present, or a
 *            paging-structure read from guest RAM failed;
 *   -EINVAL  unsupported configuration (5-level LA57 paging).
 */
int relm_mmu_gva_to_gpa(struct vcpu *vcpu, uint64_t gva, uint64_t *gpa);

int relm_mmu_copy_from_guest_virt(struct vcpu *vcpu, uint64_t gva,
                                  void *data, size_t size);

#endif /* RELM_SVM_MMU_H */
