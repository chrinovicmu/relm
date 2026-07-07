#ifndef VM_H
#define VM_H

#include <linux/spinlock.h>
#include <linux/types.h>

#include <relm_arch/vm_arch.h>
#include <include/virtio/mmio.h>

struct vcpu; 
struct relm_vm_operations; 
struct relm_mem_ops; 

#define RELM_VM_NAME_LEN    16 
#define RELM_MAX_VCPUS 1  
#define GUEST_STACK_ORDER 2 
#define HOST_CPU_ID 1
#define RELM_VM_GUEST_RAM_SIZE 128 * 1024 * 1024 
    
/*represents a single virtual machine */ 

struct guest_mem_region
{
    uint64_t gpa_start; 
    uint64_t size; 
    struct page **pages; 
    uint64_t nr_pages; 
    uint64_t flags;
    struct guest_mem_region *next; 
}; 

/* All VM's memory related structs and info*/ 
struct relm_vm_memory
{
    /*linked list of backed, guest-RAM regions , eahc entry 
     * discribes a contigous ranfe of real struct pages mapped into 
     * the guest's phsyical address space */  
    struct guest_mem_region *mem_regions; 
    uint64_t total_guest_ram; 

    /*GPA ranges reserved for MMIO purposes
     * left unmapped so that any access traps a VMEXIT*/  
    struct relm_mmio_region mmio_regions[RELM_MAX_MMIO_REGIONS]; 
    unsigned int mmio_region_count; 
}; 

/* Memory region flags. */
#define RELM_MEM_F_READ    BIT(0)
#define RELM_MEM_F_WRITE   BIT(1)
#define RELM_MEM_F_EXEC    BIT(2)
#define RELM_MEM_F_MMIO    BIT(3)   /* not backed by pages, MMIO intercept  */

enum vm_state {
    VM_STATE_CREATED,
    VM_STATE_INITIALIZED, 
    VM_STATE_RUNNING, 
    VM_STATE_SUSPENDED, 
    VM_STATE_STOPPED
}; 

struct relm_vm_stats
{
    uint64_t total_exits; 
    uint64_t hypercalls; 
    uint64_t hlt_exits;
    uint64_t cpuid_exits;
    uint64_t start_time_ns; 
    uint64_t end_time_ns; 

}; 

/*A single VM instance */ 
struct relm_vm
{
    int vm_id;
    char vm_name[RELM_VM_NAME_LEN];

    struct relm_vm_memory memory; 
    const struct relm_mem_ops *mem_ops; 

    int max_vcpus;
    int online_vcpus;
    struct vcpu **vcpus;

    struct relm_firmware_data *fw_data;  

    enum vm_state state; 
    struct relm_vm_stats stats; 
    const struct relm_vm_operations *ops; /*points at ops_storage*/ 
    struct relm_vm_operations ops_storage; /*merged arch+generic*/ 

    struct relm_vm_arch arch; 

    spinlock_t lock; 
}; 

struct relm_vm_operations 
{
    int (*vm_init)(struct relm_vm *vm);

    void (*vm_destroy)(struct relm_vm *vm);

    /*create, arch-initilizer, and register one vCPU*/ 
    int (*add_vcpu)(struct relm_vm *vm, int vpid); 

    /* Nanoseconds since VM start. */
    uint64_t (*get_uptime)(struct relm_vm *vm);

    /* Aggregate vCPU utilisation, 0–100. */
    uint64_t (*get_cpu_utilization)(struct relm_vm *vm);

    /* Dump guest registers for a specific vCPU to the kernel log. */
    void (*dump_regs)(struct relm_vm *vm, int vcpu_id);

    /* Print exit statistics to the kernel log. */
    void (*print_stats)(struct relm_vm *vm);
};

struct relm_mem_ops
{
    /*allocate and initialize arch page tables root
     * EPT root on x86, VTTBR/s2_pgd on arm64, hgatp on riscv*/ 
    int (*setup)(struct relm_vm *vm); 
    
    /*map single guest page*/     
    int(*map_page)(struct relm_vm *vm, uint64_t gpa, 
                   uint64_t hpa, uint64_t flags); 
    
    /*unmap single guest page*/ 
    void (*unmap_page)(struct relm_vm *vm, uint64_t gpa); 

    /*build guest visiable page tbals inside guest RAM*/ 
    int (*create_guest_page_tables)(struct relm_vm *vm); 

    /*invalidate cached translation*/ 
    void (*invalidate)(struct relm_vm *vm);

    /*destroy all allocated by setup*/ 
    void (*destroy)(struct relm_vm *vm); 
}; 

struct relm_vm * relm_create_vm(int vm_id, const char *name, uint64_t ram_size); 
void relm_destroy_vm(struct relm_vm *vm); 

int relm_vm_add_vcpu(struct relm_vm *vm, int vcpu_id); 
struct vcpu *relm_vm_get_vcpu(struct relm_vm *vm, uint16_t vpid); 

int relm_run_vm(struct relm_vm *vm);
int relm_stop_vm(struct relm_vm *vm);

int relm_vm_allocate_guest_ram(struct relm_vm *vm, uint64_t size, uint64_t gpa_start); 
int relm_vm_map_mmio_region(struct relm_vm *vm, uint64_t gpa, uint64_t hpa, uint64_t size); 
void relm_vm_free_guest_mem(struct relm_vm *vm);

int relm_vm_copy_to_guest(struct relm_vm *vm, uint64_t gpa, const void *data, size_t size);
int relm_vm_copy_from_guest(struct relm_vm *vm, uint64_t gpa, void *data, size_t size);
int relm_vm_zero_guest_memory(struct relm_vm *vm, uint64_t gpa, size_t size); 
int relm_vm_create_guest_page_tables(struct relm_vm *vm);

struct vcpu *relm_get_current_vcpu(void);
#endif 
