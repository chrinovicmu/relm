#ifndef SVM_OPS_H
#define SVM_OPS_H

#include <linux/types.h>

static inline void _vmload(uint64_t vmcb_pa)
{
    asm volatile("vmload" : : "a"(vmcb_pa) : "memory");
}

static inline void _vmsave(uint64_t vmcb_pa)
{
    asm volatile("vmsave" : : "a"(vmcb_pa) : "memory");
}

static inline void _vmrun(uint64_t vmcb_pa)
{
    asm volatile("vmrun" : : "a"(vmcb_pa) : "memory");
}

static inline void _clgi(void)
{
    asm volatile("clgi" ::: "memory");
}

static inline void _stgi(void)
{
    asm volatile("stgi" ::: "memory");
}

#endif /* SVM_OPS_H */

