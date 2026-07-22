#ifndef RELM_VMX_MMU_H
#define RELM_VMX_MMU_H


/*
 * mmu.h — guest-virtual → guest-physical address translation for the VMX
 * backend ("the software MMU walker"). */ 

/*converts a CS-relative RIP into a guest linear address by applu x86 segmentaion */ 
uint64_t relm_mmu_rip_to_linear(struct vcpu *vcpu, uint64_t rip); 

/* translate one guest linear address to the guest-physical address*
*
* handles all three paging modes:
* CR0.PG=0 no paging GPA == GVA
* CR0.PG=1 CR4.PAE=0 2-level 32-bit paging
* CR0.PG=1 CR4.PAE=1, 
*       EFER.LMA=0 3-level PAE paging
*       EFER.LMA=1 4-level long-mode paging*/  

int relm_mmu_gva_to_gpa(struct vcpu *vcpu, uint64_t gva, uint64_t *gpa); 

/*copy size bytes starting at guest virtual address into host buffer */ 
int relm_mmu_copy_from_guest_virt(struct vcpu *vcpu, uint64_t gva,
                                  void *data, size_t size);
#endif

