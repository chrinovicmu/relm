#ifndef RELM_PAGE_FAULT_H
#define RELM_PAGE_FAULT_H

#include <linux/types.h>

struct vcpu;

void relm_dump_page_fault(struct vcpu *vcpu, uint64_t guest_rip);

int relm_arch_get_page_fault_info(struct vcpu *vcpu, uint64_t *out_cr2,
                                  uint32_t *out_error_code);

int relm_arch_translate_gva_to_gpa(struct vcpu *vcpu, uint64_t gva,
                                   uint64_t *out_gpa);

bool relm_page_fault_addr_in_guest_ram(struct vcpu *vcpu, uint64_t gpa,
                                       uint64_t *out_region_start,
                                       uint64_t *out_region_size);

#endif /* RELM_PAGE_FAULT_H */
