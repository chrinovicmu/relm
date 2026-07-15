# RELM — Bug Fix Report (x86 VMX backend, boot-a-Linux-guest goal)

This document records **every fix** applied to make the RELM hypervisor kernel
module compile and boot a Linux guest on the Intel VMX backend, and the exact
bug behind each one. It is organized as:

1. **The contract** — the two cross-cutting architectural decisions that every
   other fix depends on (authoritative `struct vcpu`, the `relm_arch` include
   mechanism, and the include-path convention).
2. **Per-file fixes** — grouped by file, each entry: *what was wrong → why it
   broke the build/boot → what changed.*

> **Build note.** This tree is a Linux kernel module; it can only be compiled on
> a Linux host with kernel build headers (`make ARCH=x86 SUBTARGET=vmx`). Fixes
> here are static (source-level) corrections; they were **not** compiler-verified
> in the authoring environment (macOS). Run a build on your Linux box to confirm.

---

## 0. The contract (cross-cutting decisions)

The tree was mid-refactor: a generic architecture-independent layer
(`include/relm/*`, `src/core/*`) was being split out from the x86-VMX backend
(`include/arch/x86/vmx/*`, `src/arch/x86/vmx/*`), and the split was left
half-done. Two incompatible worlds coexisted. Every per-file fix below assumes
these decisions:

### 0.1 Authoritative data model = the generic layer

- **`struct vcpu`** is defined **once**, in `include/relm/vcpu.h`. It holds
  arch-independent fields (`vm`, `vpid`, `state`, `launched`, `halted`,
  `host_task`, `host_stack`, `stats`, `ops`, …) and embeds the arch-specific
  state as `struct vcpu_arch arch;`.
- **`struct vcpu_arch`** (x86 VMX) is defined **once**, in
  `include/arch/x86/vmx/vcpu_arch.h`. It holds all VMX-specific fields: `vmcs`,
  `controls`, bitmaps, MSR areas, `regs` (`struct guest_regs`), `cr0..cr4`,
  `efer`, `gdtr/idtr`, `exit_reason`, `apic`, `cr3_cache`, and (added by this
  work) `host_rsp` + `vmentry_host_rsp`.
- The pre-refactor **flat `struct vcpu`** and the duplicated `struct
  guest_regs`, `enum vcpu_state`, `struct vcpu_stats`, and (minimal)
  `struct cr3_shadow_cache` that lived in `include/arch/x86/vmx/vmx.h` and
  `vcpu_arch.h` are **removed**. They collided (redefinition errors) because
  `vcpu_arch.h` includes `vmx.h`, so both landed in one translation unit.

**Consequence — the universal member rename** applied throughout the arch code:

| Old (flat) access        | New (generic) access            |
|--------------------------|---------------------------------|
| `vcpu->regs.*`           | `vcpu->arch.regs.*`             |
| `vcpu->efer`             | `vcpu->arch.efer`               |
| `vcpu->cr0/cr3/cr4`      | `vcpu->arch.cr0/cr3/cr4`        |
| `vcpu->vmentry_host_rsp` | `vcpu->arch.vmentry_host_rsp`   |
| `vcpu->exit_reason`      | `vcpu->arch.exit_reason`        |
| `vm->ept`                | `vm->arch.ept`                  |
| `vm->pml4_gpa`           | `vm->arch.pml4_gpa`             |
| `vm->kernel_entry_gpa`   | `vm->arch.kernel_entry_gpa`     |
| `vm->boot_params_gpa`    | `vm->arch.boot_params_gpa`      |
| `vm->iommu`              | `vm->arch.iommu`                |
| `vm->total_guest_ram`    | `vm->memory.total_guest_ram`    |

`vcpu->vpid/vm/state/launched/halted/host_task/host_stack/stats/target_cpu_id/ops`
stay as-is — those are genuine generic-`struct vcpu` members.

### 0.2 `relm_arch/` include prefix = a build-generated symlink

Generic headers deliberately do **not** hardcode the arch path; they include
`<relm_arch/vcpu_arch.h>`, `<relm_arch/vm_arch.h>`, `<relm_arch/arch.h>`.
Nothing mapped that prefix, so every one was a fatal *file not found*.

**Fix:** the `Makefile` now creates a symlink `include/relm_arch` →
`arch/$(ARCH)/$(SUBTARGET)` (for this build: `arch/x86/vmx`) before invoking
kbuild, and removes it on `clean`. With the existing `-I$(src)/include`,
`<relm_arch/vcpu_arch.h>` resolves to `include/arch/x86/vmx/vcpu_arch.h`. This
keeps the generic layer arch-agnostic (the whole point of the refactor); to add
a new backend you only re-point the symlink, not edit core headers.

### 0.3 Include-path convention

The kbuild `-I` set is: `$(src)`, `$(src)/include`, `$(src)/utils`,
`$(src)/include/arch/x86/vmx`, `$(src)/include/arch/x86`,
`$(src)/include/arch/x86/decoder` (+ the `relm_arch` symlink under
`$(src)/include`).

The pervasive broken form `#include <include/vmx.h>` resolved to
`$(src)/include/vmx.h` — which does not exist (the header is
`include/arch/x86/vmx/vmx.h`). Corrected forms:

| Broken                        | Fixed                    | Resolves via              |
|-------------------------------|--------------------------|---------------------------|
| `<include/vm.h>`              | `<relm/vm.h>`            | `-I$(src)/include`        |
| `<include/iommu.h>`           | `<relm/iommu.h>`         | `-I$(src)/include`        |
| `<include/vmx.h>`             | `<vmx.h>`                | `-I…/arch/x86/vmx`        |
| `<include/ept.h>`             | `<ept.h>`                | `-I…/arch/x86/vmx`        |
| `<include/apic.h>`            | `<apic.h>`               | `-I…/arch/x86/vmx`        |
| `<include/vmx_ops.h>`         | `<vmx_ops.h>`            | `-I…/arch/x86/vmx`        |
| `<include/vmexit.h>`          | `<vmexit.h>`             | `-I…/arch/x86/vmx`        |
| `<include/vmcs_state.h>`      | `<vmcs_state.h>`         | `-I…/arch/x86/vmx`        |

(The already-working forms `<include/relm/…>`, `<include/firmware/…>`,
`<include/virtio/…>`, `<include/arch/x86/…>`, `<utils/utils.h>` were left alone
to minimize churn.)

---

## 1. Per-file fixes

### 1.1 `Makefile`

- **Bug:** `BOOT_OBJS` listed `src/boot/linux_loader.o` and `src/boot/stub.o`.
  Neither source exists — the real boot sources are `src/arch/x86/boot/boot.c`
  and `src/arch/x86/boot/stub.S`. kbuild aborts with *"No rule to make target
  'src/boot/linux_loader.o'"* before compiling anything.
  **Fix:** point `BOOT_OBJS` at `src/arch/x86/boot/boot.o` and
  `src/arch/x86/boot/stub.o`.
- **Bug (link):** `src/arch/x86/boot/{boot,stub}` were never in the build graph
  (`ARCH_SRC_DIR` wildcards only `src/arch/x86/vmx`), so `relm_boot_load`,
  `relm_boot_info`, and the `relm_idt_stub_template` symbols were undefined at
  link even though `relm.c` calls them. **Fix:** same as above — explicitly
  listing the boot objects pulls them in.
- **Bug:** the `<relm_arch/...>` include prefix used by generic headers mapped to
  nothing → fatal *file not found* in every core translation unit.
  **Fix:** new `RELM_ARCH_LINK` rule creates `include/relm_arch →
  arch/$(ARCH)/$(SUBTARGET)` as a prerequisite of `modules:`, removed on
  `clean`; added to `.gitignore`.
- `src/core/memory.c` (0 bytes) is left in the build: an empty translation unit
  produces an empty object that links cleanly, and the guest-memory helpers that
  would nominally live there are already defined in `src/core/vm.c`. No change
  needed — flagged so it is not mistaken for a missing file.

### 1.2 `include/arch/x86/vmx/vmx.h`

- **Bug (compile):** broken include paths `<include/vmx_ops.h>`, `<include/vmcs.h>`,
  `<include/ept.h>`, `<include/apic.h>`. **Fix:** → `<vmx_ops.h>`, `<vmcs.h>`,
  `<ept.h>`, `<apic.h>`.
- **Bug (compile):** defined a second, flat `struct vcpu` plus duplicate
  `struct guest_regs`, `enum vcpu_state`, `struct vcpu_stats` — colliding with
  the authoritative definitions in `<relm/vcpu.h>` / `<vcpu_arch.h>`
  (redefinition errors; this header is transitively included by `vcpu_arch.h`).
  **Fix:** removed all four; added a `struct vcpu;` forward declaration so the
  prototypes here still compile. `struct cr3_target_entry`,
  `struct cr3_shadow_cache` (the full CR3-cache implementation), and
  `struct host_cpu` are retained.
- **Bug (compile):** `#define RELM_MAX_MANAGED_MSRS 8` collided with the value
  `16` in `vcpu_arch.h`. **Fix:** removed here; the `16` in `vcpu_arch.h` is now
  the single definition.

### 1.3 `include/arch/x86/vmx/vcpu_arch.h`

- **Bug (compile):** redefined `struct cr3_shadow_cache` with different members
  than the one in `vmx.h` (included just above), a hard redefinition error.
  **Fix:** removed the minimal local definition; the full one from `vmx.h` is
  used.
- **Bug (compile/link):** `struct vcpu_arch` was missing `host_rsp` and
  `vmentry_host_rsp`, both referenced as `vcpu->arch.host_rsp` (vmx.c) and (after
  the member rename) `vcpu->arch.vmentry_host_rsp` (vmexit.c). **Fix:** added
  both `uint64_t` fields with a comment explaining their role in the VM-entry/exit
  RSP handoff.

### 1.4 `include/arch/x86/vmx/ept.h`

- **Bug (compile):** includes `<include/vmx.h>` and `<include/vm.h>` — broken
  paths, and they formed an include cycle `vm.h → vm_arch.h → ept.h → vm.h`.
  **Fix:** all uses here are through pointers, so replaced the includes with
  forward declarations `struct relm_vm; struct vcpu;` plus
  `<linux/spinlock.h>` (for `spinlock_t`). Cycle-free.
- **Bug (compile):** `int relm_ept_create_guest_page_tables(struct relm_vm *vm)`
  (line 139) and `int relm_virtio_mmio_handle_ept_violation(...)` (line 152) were
  missing their terminating semicolons, merging with the next declaration and
  breaking every file that includes `ept.h`. **Fix:** added the semicolons.
- **Bug:** `EPT_MEMTYPE_WC` was `(1ULL << 4)` which encodes memtype value 2, not
  the write-combining memtype 1. **Fix:** `(1ULL << 3)` (bits[5:3] = 001).

### 1.5 `include/arch/x86/vmx/vmexit.h`, `vmx_ops.h`, `apic.h`

- `vmexit.h` / `vmx_ops.h` — **Bug (compile):** broken `<include/...>` paths.
  **Fix:** `<include/vm.h>`→`<relm/vm.h>`, and `<include/vmcs_state.h>`,
  `<include/vmx.h>`, `<include/vmx_ops.h>`, `<include/vmcs.h>` → their bare arch
  forms.
- `vmx_ops.h` `_vmwrite()` — **Bug (boot):** on a genuine `vmwrite` failure it
  read `VMCS_INSTRUCTION_ERROR_FIELD` and, when that read itself returned 0
  (e.g. VMfailInvalid, no current VMCS), returned `0` — i.e. *success*. Every
  mis-set VMCS field was silently accepted, so setup bugs surfaced only as an
  undiagnosable VMLAUNCH failure. **Fix:** always return a negative error on
  failure (`error_code ? -error_code : -1`).
- `vmx_ops.h` `_cpu_has_vpid()` — **Bug:** tested `CPUID.1:ECX[5]`, which is the
  VMX feature bit, not VPID. **Fix:** read the VPID allowed-1 bit (secondary
  control bit 5) from the high dword of `IA32_VMX_PROCBASED_CTLS2`.
- `vmx_ops.h` `_get_vmcs_size()` — **Bug:** `"0x%ll\n"` truncated format
  specifier. **Fix:** `"0x%llx\n"`.
- `apic.h` — **Bug:** `enum virt_apic_timer_mode timer_mode; ;` — trailing empty
  struct member (GNU-extension warning). **Fix:** removed the extra `;`.

### 1.6 `src/core/vcpu.c`

- **Bug (compile):** `VCPU_STATE_UNINTIALIZED` (misspelled) — the enum member is
  `VCPU_STATE_UNINITIALIZED`. **Fix:** corrected spelling.
- **Bug (compile):** `free_pages(..., HOST_STACK_ORDER)` at two sites —
  `HOST_STACK_ORDER` is not visible in this generic TU (the header defines
  `RELM_HOST_STACK_ORDER`, used for the matching alloc). **Fix:** →
  `RELM_HOST_STACK_ORDER` (also makes alloc/free orders match).
- **Bug (BOOT-CRITICAL):** the vCPU loop tested `vcpu->ops->init` (no such member)
  and then **fell straight into the run loop without ever calling Phase-2
  `vcpu_init`** — so VMCLEAR/VMPTRLD and every host+guest VMCS field write never
  happened, and VM-entry ran against a completely unprogrammed VMCS. The guest
  could never boot. **Fix:** test `vcpu->ops->vcpu_init`, then actually call
  `ret = vcpu->ops->vcpu_init(vcpu);` on the pinned CPU and fail cleanly (jump to
  `_out_arch_destroy`) if it errors. This also makes the previously-unused
  `_out_arch_destroy` label a live `goto` target (removing the unused-label
  warning).

### 1.7 `src/arch/x86/vmx/vmexit.c` (VM-exit dispatch)

- **Bug (compile):** broken `<include/...>` paths + no visibility of the full
  `struct vcpu`. **Fix:** paths corrected per §0.3 and added `#include
  <relm/vcpu.h>`.
- **Member rename:** `vcpu->regs.*`→`vcpu->arch.regs.*` (throughout),
  `vcpu->efer`→`vcpu->arch.efer`, `vcpu->vmentry_host_rsp`→
  `vcpu->arch.vmentry_host_rsp`.
- **Bug (BOOT-CRITICAL — CPUID results discarded):** `emulate_cpuid()` wrote the
  CPUID result into `vcpu->arch.regs` only. But on VMRESUME the exit stub
  (`vmx_asm.S`) restores guest GPRs from the **on-stack save block**
  (`struct stack_guest_gprs *guest_gprs`), not from `vcpu->arch.regs` — so the
  emulated result never reached the guest. Linux issues CPUID pervasively during
  early boot, so this alone stalled the boot. **Fix:** `emulate_cpuid()` now
  takes `guest_gprs` and writes the result into it (still mirroring into
  `vcpu->arch.regs` for tracing); the call site passes `guest_gprs`.
- **Bug (BOOT-CRITICAL — fw_cfg IN corrupts guest IDT):** on a fw_cfg `IN` the
  read value was written with `_vmwrite(0x6818, rax)`. `0x6818` is
  `GUEST_IDTR_BASE`, not a register — every fw_cfg read clobbered the guest IDT
  base, and the value never actually reached the guest (only `vcpu->arch.regs`
  was set). **Fix:** removed the bogus VMCS write; deliver the value via
  `guest_gprs->rax` (the path that actually reaches the guest), mirrored into
  `vcpu->arch.regs.rax`.
- **Bug (BOOT-CRITICAL — long mode never activates):** in the WRMSR-EFER path,
  `bool lma = lma && pg;` used `lma` in its own initializer (garbage);
  `if(lma) val != (1ULL<<10);` was a discarded comparison that never set
  EFER.LMA; and CR0.PG was tested at bit 32 instead of **bit 31**. Result:
  EFER.LMA and the `IA32E_MODE` entry control were driven from garbage, so the
  guest could not reliably enter 64-bit mode. **Fix:** `lma = lme && pg;`,
  `pg = guest_cr0 & (1ULL<<31)`, and `if(lma) val |= (1ULL<<10);`.
- **Bug (BOOT-CRITICAL — VM-entry-failure check dead):**
  `if(exit_reason & (1U << 32))` — the failure flag is **bit 31**, and
  `1U<<32` is an undefined 32-bit shift. **Fix:** test `(1ULL<<31)` on the full
  exit-reason value before masking to 16 bits.
- **Bug (BOOT — HLT permanently stops the vCPU):** HLT set
  `vcpu->state = VCPU_STATE_HALTED`, which the generic loop treats as
  "state != RUNNING" and breaks out of — killing the vCPU on the first HLT.
  Linux's idle path HLTs expecting resume-on-interrupt. **Fix:** advance RIP,
  set `vcpu->halted = true`, **keep** `state == RUNNING`, and return 0 so the
  loop sleeps on the wait queue until an interrupt is injected, then re-enters
  the guest.
- **Bug (BOOT — MMIO emulation stops the guest):** the successful
  `relm_virtio_mmio_handle_ept_violation` branch set `ret = 0` (= stop). **Fix:**
  `ret = 1` (resume via VMRESUME); the handler already advanced RIP past the
  faulting instruction. Killed the guest on its first virtio MMIO access before.
- **Bug (diagnostics/robustness):** no `case EXIT_REASON_EPT_MISCONFIG` — an EPT
  misconfig fell through to the generic "unhandled reason" stop. **Fix:** added a
  dedicated case that reads `GUEST_PHYSICAL_ADDRESS`, calls
  `relm_vcpu_handle_ept_misconfig(vcpu->vm)` (prototype added to `ept.h`), and
  stops with a specific diagnostic.

### 1.8 `src/arch/x86/vmx/vmx.c`

**Compile**
- Broken include paths (`<include/vmx.h>` … `<include/vmcs_state.h>`) → bare arch
  forms; added `#include <relm/vcpu.h>` (this is what defines `struct vcpu` with
  `.arch`, `.launched`, `.stats`).
- The `vmx_vcpu_ops` initializer referenced `vmx_setup_mmu`, `vmx_inject_irq`,
  `vmx_read_msr`, `vmx_write_msr` with no forward declarations (defined much
  later) → added the four `static` prototypes.
- `relm_vmentry_asm(...)` called with no C prototype (implicit declaration,
  fatal under `-Werror`) → added `extern int relm_vmentry_asm(void *guest_regs,
  int launched);`, matching the asm (RDI=guest_regs, RSI=launched) and the call
  `relm_vmentry_asm(&vcpu->arch.regs, vcpu->launched)`.
- `pr_err("…%d…\n"), atomic_read(&work.failed_cpus);` — the `)` closed the call
  before its argument (`%d` had no operand). Fixed to pass the argument.
- Member map: `vcpu->arch.launched`→`vcpu->launched` (4 sites — `launched` is a
  generic `struct vcpu` member, not in `vcpu_arch`); `vcpu->vm->pml4_gpa`/
  `kernel_entry_gpa`/`total_guest_ram` → `vcpu->vm->arch.*` / `vcpu->vm->memory.*`.
- `.dump_regs = vmx_dump_regs` took the address of a `static`-declared but
  **never-defined** function (fatal under `-Werror`). The real defined function
  is `vmx_dump_vcpu`; removed the dead declaration, prototyped `vmx_dump_vcpu`,
  and pointed `.dump_regs` at it.

**Boot-critical**
- **HOST_IA32_PAT never written** although `VM_EXIT_LOAD_IA32_PAT` is enabled → on
  the first VM-exit the host reloads PAT=0 ⇒ all host memory Uncacheable ⇒ host
  hang. **Fix:** `CHECK_VMWRITE(HOST_IA32_PAT, __rdmsr1(MSR_IA32_CR_PAT));` in
  `relm_setup_host_state` (reads the real host PAT so host caching is preserved).
- **GUEST_IA32_PAT never written** although `VM_ENTRY_LOAD_GUEST_PAT` is enabled →
  guest runs fully Uncacheable. **Fix:** `CHECK_VMWRITE(GUEST_IA32_PAT,
  __rdmsr1(MSR_IA32_CR_PAT));` in the Linux-boot guest-state path.

**Correctness**
- `relm_setup_guest_state_longmode`: `cr0`/`cr4`/`cr0_fixed*`/`cr4_fixed*` declared
  but never assigned, then `cr0`/`cr4` read uninitialized in a `pr_info` — and the
  function never actually VMWROTE GUEST_CR0/CR4. **Fix:** read the four VMX
  fixed-bit MSRs (`CR0_FIXED0/1` 0x486/7, `CR4_FIXED0/1` 0x488/9), compute
  long-mode-legal `cr0`/`cr4` (`(val|fixed0)&fixed1`), VMWRITE GUEST_CR0/CR4,
  update `vcpu->arch.cr0/cr4`, and print the real values.
- `memset(cache, 0, sizeof(cache))` in `relm_cr3_cache_init` zeroed only pointer
  size → `sizeof(*cache)`.
- HOST_CS/SS/DS/ES selectors masked with `0xF8` (clears index bits 8-15,
  corrupting `__KERNEL_CS/DS` on modern kernels ⇒ VM-entry failure) → `0xFFF8`.
- `relm_get_max_cr3_targets` truncated a 9-bit field (max 256) via a `uint8_t`
  cast (256→0) → cast to `uint32_t`.

### 1.9 `src/core/vm.c`, `src/core/iommu.c`, `src/core/decoder.c`

**vm.c**
- Removed the duplicate per-CPU `current_vcpu` block (DEFINE_PER_CPU +
  `relm_get_current_vcpu`) — also defined in `vcpu.c`, causing a
  multiple-definition link error.
- `relm_generic_vm_ops` initializer: `;`→`,` (a `;` terminated the initializer
  mid-list, dropping later members); `relm_generic_vm_ops->x`→`.x` (it's a struct
  value, not a pointer).
- `vm-<mem_ops->setup`→`vm->mem_ops->setup` (`-<` typo).
- `relm_run_vcpu(vm)`→`relm_vcpu_run(vcpu)` (correct API, per-vcpu);
  `relm_stop_vcpu(...)` (1- and 2-arg, nonexistent) → `relm_vcpu_stop(vcpu)`;
  `relm_vcpu_destroy(...)`→`relm_vcpu_free(vm->vcpus[i])` (the defined function).
- `goto _out_free_ept` targeted an **undefined label** → retargeted to
  `_out_free_mmu` (the correct cleanup at that failure point).
- `relm_vm_allocate_guest_ram`: page was stored in `region->pages[i]` and then
  `__free_page`'d on map failure, and freed **again** by the cleanup loop (double
  free) → store only after a successful map.
- `relm_vm_zero_guest_memory`: `region` used uninitialized on the first iteration
  → initialized to `NULL`.
- Removed a duplicated `vcpu = vm->vcpus[i];` line, a dead `return NULL;` after a
  `goto`, and moved declarations above statements where `-Werror=declaration-
  after-statement` would fire.

**iommu.c**
- Broken includes fixed; every `vm->iommu.*`→`vm->arch.iommu.*`,
  `vm->ept`→`vm->arch.ept`, `vm->total_guest_ram`→`vm->memory.total_guest_ram`.
- `relm_iommu_destroy`: inverted guard `if(!enabled || domain)` (returned when a
  domain existed, leaking it) → `if(!enabled || !domain)`.
- Device-detach loop used `list_for_each_entry` with `list_del`+`kfree` inside
  (use-after-free) → `list_for_each_entry_safe` (using the already-declared `tmp`).

**decoder.c**
- `-ENOSYS` used without errno → added `#include <linux/errno.h>`.

**relm.c** — no changes needed; the `relm_boot_load` call and all includes
resolve correctly.

### 1.10 `src/arch/x86/vmx/ept.c`

**Compile**
- Broken includes fixed.
- **Stray `bool relm_virtio_mmio_handle` fragment** (no params/body/`;`) before
  `relm_vcpu_handle_ept_misconfig` made the whole file fail to parse → removed.
- `relm_ept_create_guest_page_tables_vmx(vm)` → `relm_ept_create_guest_page_tables`
  (the defined/declared name).
- `_unlikely(!table)` → `unlikely(!table)`.
- `pr_err("…: %d\n")` missing its `err` argument → added it.

**Boot-critical**
- **EPT leaf forced WB memtype:** the leaf writer OR-ed in `EPT_MEMTYPE_WB`
  unconditionally, so MMIO/APIC-access pages (which pass UC) got WB ⇒ wrong device
  caching / EPT misconfig. **Fix:** leaf writer now trusts the caller's `flags`
  (`hpa | flags`); RAM call sites (`vmx_mem_map_page` RAM path, and the three
  guest page-table pages in `relm_ept_create_guest_page_tables`) were updated to
  pass `EPT_MEMTYPE_WB`, while device callers keep UC. Invariant: *leaf writer
  trusts flags; RAM→WB, device→UC.*

**Correctness**
- `relm_handle_ept_violation`: inverted guard `if(!vm || vm->arch.ept)` (errored
  when EPT was valid) → `if(!vm || !vm->arch.ept)`.
- `free_page((unsigned)table_va)` truncated a 64-bit pointer → `(unsigned long)`.
- `vm->total_guest_ram`→`vm->memory.total_guest_ram`.

### 1.11 `src/arch/x86/vmx/apic.c`

- Broken includes fixed; added `#include <relm/vcpu.h>`.
- `vm->ept`→`vm->arch.ept` (two sites).
- **APIC member location (the audit had this backwards):** since `vcpu_arch` is
  authoritative, the virtual APIC is `vcpu->arch.apic`. The `vcpu->arch.apic`
  uses were already correct; the wrong ones were `candidate->apic.*` /
  `target->apic` → `->arch.apic`, and
  `container_of(apic, struct vcpu, apic)` (wrong — `apic` is a member of
  `struct vcpu_arch`) → the two-step
  `container_of(container_of(apic, struct vcpu_arch, apic), struct vcpu, arch)`.

### 1.12 `src/arch/x86/boot/boot.c`, firmware

**boot.c**
- `<include/vm.h>`→`<relm/vm.h>`; `vm->total_guest_ram`→`vm->memory.total_guest_ram`
  (3 sites).
- `relm_boot_load` **defined** with `struct device dev` (by value) but the header
  prototype and caller use `struct device *` → changed the parameter to a
  pointer, which fixes the cascading `!dev` and `relm_kernel_load(…, dev, …)` uses.
- `relm_install_daig_idt_gdt` (typo "daig") at the call (~423) **and** the
  definition (~520) → `relm_install_diag_idt_gdt`, matching the header.

**e820.c / fw_cfg.c** — `<include/vm.h>`→`<relm/vm.h>`;
`vm->total_guest_ram`→`vm->memory.total_guest_ram`.

**seabios.c** — broken includes fixed; `seabios_hva = seabios_va` (void*→uint64_t)
→ `(uint64_t)(uintptr_t)seabios_va`; sibling `seabios_hva = NULL`→`= 0`.

**include/firmware/fw_cfg.h** — a second block re-`#define`d the `FW_CFG_*`
selector keys to **different, shifted** values (macro-redefinition, fatal under
CONFIG_WERROR, and wrong keys so SeaBIOS read the wrong items). **Fix:** removed
the conflicting block, keeping the single canonical QEMU/SeaBIOS key set
(KERNEL_CMDLINE=0x09, INITRD_ADDR=0x0A, INITRD_SIZE=0x0B, BOOT_DEVICE=0x0C,
MAX_CPUS=0x0F, …); verified every key used by `fw_cfg.c` still exists.

### 1.13 virtio + instruction decoder

**include/virtio/mmio.h** — `RELM_MAC_MMIO_REGIONS`→`RELM_MAX_MMIO_REGIONS`
(used to size `vm.h`'s `mmio_regions[]`); `RELM_VIRTIO_MMIO_REGION`→
`RELM_VIRTIO_MMIO_REGION_SIZE`.

**include/virtio/virtio.h** — `__atrribute__`→`__attribute__` (×2);
`uint16`/`uint32`→`uint16_t`/`uint32_t` (several); `last_avial_idx`→
`last_avail_idx`; struct member `op`→`ops`; `drivers_features_ack`→
`driver_features_ack`; `sizeof(struct vring_used_elem)`→`…vring_used_entry`;
`vring_avail_total_size` ring term scaled by `sizeof(__le16)` (was undercounting
the avail ring by ~`2*queue_size` bytes); added a `find_device_for_gpa` prototype.

**src/virtio/virtio.c** — `unsigned int;`→`unsigned int i;`;
`VIRTIO_MMIO_MAGIC_VALUE`→`VIRTIO_MMIO_VALUE` (+ missing `;`) — without a correct
MagicValue the guest rejects the device; `VERSON`→`VERSION`; `write`→`is_write`
(2 sites); **DeviceFeaturesSel now latches on WRITE** (was `!is_write`, breaking
feature negotiation); `?=`→`>=`; **`if(!value == 0)`→`if(*value == 0)`** (the old
form was always true, so virtqueues never became ready); **`*value == vq->ready`
→ `*value = vq->ready`** (QueueReady read was discarded); `notify`→`queue_notify`;
`case …STATUS_OFF;`→`:`; missing `;`; `opd`→`ops`; `spin_unlock(%dev…)`→`&dev…`;
made `find_device_for_gpa` non-static.

**src/virtio/mmio.c** — `<include/arch/x86/apic.h>`→`<apic.h>` (+ `<relm/vcpu.h>`);
`relm_cm_reserve_mmio_region`→`relm_vm_reserve_mmio_region`;
`mem_regions_count`/`mem_region_count`→`mmio_region_count`,
`mmio_region[i]`→`mmio_regions[i]`; `overlap`→`overlaps` (in-scope);
`relm_virto_device`→`relm_virtio_device`; `insn_bytes`→`insn_buf`; **immediate
MMIO writes (0xC7) now use `decoded.immediate`** instead of an uninitialized GPR
index; `1if (needs_irq)`→`if (needs_irq)`;
`relm_apic_inject_irq(dev->vm, dev->irq)` → the real API
`relm_apic_inject_interrupt(&vcpu->arch.apic, (uint8_t)dev->irq, false)`.

**include/arch/x86/decoder/insn.h** — `#include "inat.h""` (stray `"`) →
`#include "inat.h"`.

---

## 2. REMAINING MANUAL BLOCKER — generated `inat-tables.c` is absent

`src/arch/x86/decoder/inat.c` includes
`<include/arch/x86/decoder/inat-tables.c>`, a **generated** file that is not
present anywhere in the repo (confirmed by search). Without it the symbols
`inat_primary_table`, `inat_escape_tables`, `inat_group_tables`,
`inat_avx_tables`, `inat_xop_tables` are undefined and the x86 instruction
decoder **cannot link** — which also blocks the virtio MMIO EPT-violation path
(`relm_decode_instruction`). This is data, not a code bug; it must be generated
from Linux kernel sources on your Linux build host:

```sh
awk -f <linux>/arch/x86/tools/gen-insn-attr-x86.awk \
    <linux>/arch/x86/lib/x86-opcode-map.txt \
    > include/arch/x86/decoder/inat-tables.c
```

The include was left in place with a documenting comment. Everything else is
fixed at the source level.

---

## 3. How to build / verify

```sh
make ARCH=x86 SUBTARGET=vmx          # on a Linux host with kernel build headers
```

Not compiled in the authoring environment (macOS). Expect to iterate on residual
warnings that only a real `-Werror` kernel build will surface; the structural
blockers (build graph, the two `struct vcpu` worlds, the `relm_arch` prefix, the
pervasive include breakage) and the boot-path logic bugs (unprogrammed VMCS,
CPUID/EFER/PAT/HLT/MMIO handling) are resolved above.


