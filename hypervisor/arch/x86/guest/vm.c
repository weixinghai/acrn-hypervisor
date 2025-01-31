/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <sprintf.h>
#include <per_cpu.h>
#include <lapic.h>
#include <vm.h>
#include <vm_reset.h>
#include <bits.h>
#include <e820.h>
#include <multiboot.h>
#include <vtd.h>
#include <reloc.h>
#include <ept.h>
#include <guest_pm.h>
#include <console.h>
#include <ptdev.h>
#include <vmcs.h>
#include <pgtable.h>
#include <mmu.h>
#include <logmsg.h>
#include <vboot.h>
#include <vboot_info.h>
#include <board.h>
#include <sgx.h>
#include <sbuf.h>
#include <pci_dev.h>

vm_sw_loader_t vm_sw_loader;

/* Local variables */

static struct acrn_vm vm_array[CONFIG_MAX_VM_NUM] __aligned(PAGE_SIZE);

static struct acrn_vm *sos_vm_ptr = NULL;

static struct e820_entry sos_ve820[E820_MAX_ENTRIES];

uint16_t get_vmid_by_uuid(const uint8_t *uuid)
{
	uint16_t vm_id = 0U;

	while (!vm_has_matched_uuid(vm_id, uuid)) {
		vm_id++;
		if (vm_id == CONFIG_MAX_VM_NUM) {
			break;
		}
	}
	return vm_id;
}

/**
 * @pre vm != NULL
 */
bool is_poweroff_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_POWERED_OFF);
}

/**
 * @pre vm != NULL
 */
bool is_created_vm(const struct acrn_vm *vm)
{
	return (vm->state == VM_CREATED);
}

bool is_sos_vm(const struct acrn_vm *vm)
{
	return (vm != NULL)  && (get_vm_config(vm->vm_id)->load_order == SOS_VM);
}

/**
 * @pre vm != NULL
 * @pre vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_postlaunched_vm(const struct acrn_vm *vm)
{
	return (get_vm_config(vm->vm_id)->load_order == POST_LAUNCHED_VM);
}

/**
 * @pre vm != NULL
 * @pre vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_prelaunched_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config;

	vm_config = get_vm_config(vm->vm_id);
	return (vm_config->load_order == PRE_LAUNCHED_VM);
}

/**
 * @pre vm != NULL && vm_config != NULL && vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_lapic_pt_configured(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	return ((vm_config->guest_flags & GUEST_FLAG_LAPIC_PASSTHROUGH) != 0U);
}

/**
 * @pre vm != NULL && vm_config != NULL && vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_rt_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	return ((vm_config->guest_flags & GUEST_FLAG_RT) != 0U);
}

/**
 * @pre vm != NULL && vm_config != NULL && vm->vmid < CONFIG_MAX_VM_NUM
 */
bool is_highest_severity_vm(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	return ((vm_config->guest_flags & GUEST_FLAG_HIGHEST_SEVERITY) != 0U);
}

/**
 * @pre vm != NULL && vm_config != NULL && vm->vmid < CONFIG_MAX_VM_NUM
 */
bool vm_hide_mtrr(const struct acrn_vm *vm)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm->vm_id);

	return ((vm_config->guest_flags & GUEST_FLAG_HIDE_MTRR) != 0U);
}

/**
 * @brief Initialize the I/O bitmap for \p vm
 *
 * @param vm The VM whose I/O bitmap is to be initialized
 */
static void setup_io_bitmap(struct acrn_vm *vm)
{
	if (is_sos_vm(vm)) {
		(void)memset(vm->arch_vm.io_bitmap, 0x00U, PAGE_SIZE * 2U);
	} else {
		/* block all IO port access from Guest */
		(void)memset(vm->arch_vm.io_bitmap, 0xFFU, PAGE_SIZE * 2U);
	}
}

/**
 * return a pointer to the virtual machine structure associated with
 * this VM ID
 *
 * @pre vm_id < CONFIG_MAX_VM_NUM
 */
struct acrn_vm *get_vm_from_vmid(uint16_t vm_id)
{
	return &vm_array[vm_id];
}

/* return a pointer to the virtual machine structure of SOS VM */
struct acrn_vm *get_sos_vm(void)
{
	ASSERT(sos_vm_ptr != NULL, "sos_vm_ptr is NULL");

	return sos_vm_ptr;
}

/**
 * @pre vm_config != NULL
 */
static inline uint16_t get_vm_bsp_pcpu_id(const struct acrn_vm_config *vm_config)
{
	uint16_t cpu_id = INVALID_CPU_ID;

	cpu_id = ffs64(vm_config->vcpu_affinity[0]);

	return (cpu_id < get_pcpu_nums()) ? cpu_id : INVALID_CPU_ID;
}

/**
 * @pre vm != NULL && vm_config != NULL
 */
static void prepare_prelaunched_vm_memmap(struct acrn_vm *vm, const struct acrn_vm_config *vm_config)
{
	uint64_t base_hpa = vm_config->memory.start_hpa;
	uint32_t i;

	for (i = 0U; i < vm->e820_entry_num; i++) {
		const struct e820_entry *entry = &(vm->e820_entries[i]);

		if (entry->length == 0UL) {
			break;
		}

		/* Do EPT mapping for GPAs that are backed by physical memory */
		if (entry->type == E820_TYPE_RAM) {
			ept_add_mr(vm, (uint64_t *)vm->arch_vm.nworld_eptp, base_hpa, entry->baseaddr,
				entry->length, EPT_RWX | EPT_WB);

			base_hpa += entry->length;
		}

		/* GPAs under 1MB are always backed by physical memory */
		if ((entry->type != E820_TYPE_RAM) && (entry->baseaddr < (uint64_t)MEM_1M)) {
			ept_add_mr(vm, (uint64_t *)vm->arch_vm.nworld_eptp, base_hpa, entry->baseaddr,
				entry->length, EPT_RWX | EPT_UNCACHED);

			base_hpa += entry->length;
		}
	}
}

static void filter_mem_from_sos_e820(struct acrn_vm *vm, uint64_t start_pa, uint64_t end_pa)
{
	uint32_t i;
	uint64_t entry_start;
	uint64_t entry_end;
	uint32_t entries_count = vm->e820_entry_num;
	struct e820_entry *entry, new_entry = {0};

	for (i = 0U; i < entries_count; i++) {
		entry = &sos_ve820[i];
		entry_start = entry->baseaddr;
		entry_end = entry->baseaddr + entry->length;

		/* No need handle in these cases*/
		if ((entry->type != E820_TYPE_RAM) || (entry_end <= start_pa) || (entry_start >= end_pa)) {
			continue;
		}

		/* filter out the specific memory and adjust length of this entry*/
		if ((entry_start < start_pa) && (entry_end <= end_pa)) {
			entry->length = start_pa - entry_start;
			continue;
		}

		/* filter out the specific memory and need to create a new entry*/
		if ((entry_start < start_pa) && (entry_end > end_pa)) {
			entry->length = start_pa - entry_start;
			new_entry.baseaddr = end_pa;
			new_entry.length = entry_end - end_pa;
			new_entry.type = E820_TYPE_RAM;
			continue;
		}

		/* This entry is within the range of specific memory
		 * change to E820_TYPE_RESERVED
		 */
		if ((entry_start >= start_pa) && (entry_end <= end_pa)) {
			entry->type = E820_TYPE_RESERVED;
			continue;
		}

		if ((entry_start >= start_pa) && (entry_start < end_pa) && (entry_end > end_pa)) {
			entry->baseaddr = end_pa;
			entry->length = entry_end - end_pa;
			continue;
		}
	}

	if (new_entry.length > 0UL) {
		entries_count++;
		ASSERT(entries_count <= E820_MAX_ENTRIES, "e820 entry overflow");
		entry = &sos_ve820[entries_count - 1U];
		entry->baseaddr = new_entry.baseaddr;
		entry->length = new_entry.length;
		entry->type = new_entry.type;
		vm->e820_entry_num = entries_count;
	}

}

/**
 * before boot sos_vm(service OS), call it to hide HV and prelaunched VM memory in e820 table from sos_vm
 *
 * @pre vm != NULL
 */
static void create_sos_vm_e820(struct acrn_vm *vm)
{
	uint16_t vm_id;
	uint64_t hv_start_pa = hva2hpa((void *)(get_hv_image_base()));
	uint64_t hv_end_pa  = hv_start_pa + CONFIG_HV_RAM_SIZE;
	uint32_t entries_count = get_e820_entries_count();
	const struct e820_mem_params *p_e820_mem_info = get_e820_mem_info();
	struct acrn_vm_config *sos_vm_config = get_vm_config(vm->vm_id);

	(void)memcpy_s((void *)sos_ve820, entries_count * sizeof(struct e820_entry),
			(const void *)get_e820_entry(), entries_count * sizeof(struct e820_entry));

	vm->e820_entry_num = entries_count;
	vm->e820_entries = sos_ve820;
	/* filter out hv memory from e820 table */
	filter_mem_from_sos_e820(vm, hv_start_pa, hv_end_pa);
	sos_vm_config->memory.size = p_e820_mem_info->total_mem_size - CONFIG_HV_RAM_SIZE;

	/* filter out prelaunched vm memory from e820 table */
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);

		if (vm_config->load_order == PRE_LAUNCHED_VM) {
			filter_mem_from_sos_e820(vm, vm_config->memory.start_hpa,
					vm_config->memory.start_hpa + vm_config->memory.size);
			sos_vm_config->memory.size -= vm_config->memory.size;
		}
	}
}

/**
 * @param[inout] vm pointer to a vm descriptor
 *
 * @retval 0 on success
 *
 * @pre vm != NULL
 * @pre is_sos_vm(vm) == true
 */
static void prepare_sos_vm_memmap(struct acrn_vm *vm)
{
	uint16_t vm_id;
	uint32_t i;
	uint64_t attr_uc = (EPT_RWX | EPT_UNCACHED);
	uint64_t hv_hpa;
	struct acrn_vm_config *vm_config;
	uint64_t *pml4_page = (uint64_t *)vm->arch_vm.nworld_eptp;
	struct epc_section* epc_secs;

	const struct e820_entry *entry;
	uint32_t entries_count = vm->e820_entry_num;
	const struct e820_entry *p_e820 = vm->e820_entries;
	const struct e820_mem_params *p_e820_mem_info = get_e820_mem_info();

	pr_dbg("sos_vm: bottom memory - 0x%llx, top memory - 0x%llx\n",
		p_e820_mem_info->mem_bottom, p_e820_mem_info->mem_top);

	if (p_e820_mem_info->mem_top > EPT_ADDRESS_SPACE(CONFIG_SOS_RAM_SIZE)) {
		panic("Please configure SOS_VM_ADDRESS_SPACE correctly!\n");
	}

	/* create real ept map for all ranges with UC */
	ept_add_mr(vm, pml4_page, p_e820_mem_info->mem_bottom, p_e820_mem_info->mem_bottom,
			(p_e820_mem_info->mem_top - p_e820_mem_info->mem_bottom), attr_uc);

	/* update ram entries to WB attr */
	for (i = 0U; i < entries_count; i++) {
		entry = p_e820 + i;
		if (entry->type == E820_TYPE_RAM) {
			ept_modify_mr(vm, pml4_page, entry->baseaddr, entry->length, EPT_WB, EPT_MT_MASK);
		}
	}

	pr_dbg("SOS_VM e820 layout:\n");
	for (i = 0U; i < entries_count; i++) {
		entry = p_e820 + i;
		pr_dbg("e820 table: %d type: 0x%x", i, entry->type);
		pr_dbg("BaseAddress: 0x%016llx length: 0x%016llx\n", entry->baseaddr, entry->length);
	}

	/* Unmap all platform EPC resource from SOS.
	 * This part has already been marked as reserved by BIOS in E820
	 * will cause EPT violation if sos accesses EPC resource.
	 */
	epc_secs = get_phys_epc();
	for (i = 0U; (i < MAX_EPC_SECTIONS) && (epc_secs[i].size != 0UL); i++) {
		ept_del_mr(vm, pml4_page, epc_secs[i].base, epc_secs[i].size);
	}

	/* unmap hypervisor itself for safety
	 * will cause EPT violation if sos accesses hv memory
	 */
	hv_hpa = hva2hpa((void *)(get_hv_image_base()));
	ept_del_mr(vm, pml4_page, hv_hpa, CONFIG_HV_RAM_SIZE);
	/* unmap prelaunch VM memory */
	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm_config = get_vm_config(vm_id);
		if (vm_config->load_order == PRE_LAUNCHED_VM) {
			ept_del_mr(vm, pml4_page, vm_config->memory.start_hpa, vm_config->memory.size);
		}
	}
}

/* Add EPT mapping of EPC reource for the VM */
static void prepare_epc_vm_memmap(struct acrn_vm *vm)
{
	struct epc_map* vm_epc_maps;
	uint32_t i;

	if (is_vsgx_supported(vm->vm_id)) {
		vm_epc_maps = get_epc_mapping(vm->vm_id);
		for (i = 0U; (i < MAX_EPC_SECTIONS) && (vm_epc_maps[i].size != 0UL); i++) {
			ept_add_mr(vm, (uint64_t *)vm->arch_vm.nworld_eptp, vm_epc_maps[i].hpa,
				vm_epc_maps[i].gpa, vm_epc_maps[i].size, EPT_RWX | EPT_WB);
		}
	}
}

static void register_pm_io_handler(struct acrn_vm *vm)
{
	if (is_sos_vm(vm)) {
		/* Load pm S state data */
		if (vm_load_pm_s_state(vm) == 0) {
			register_pm1ab_handler(vm);
		}
	}

	/* Intercept the virtual pm port for RTVM */
	if (is_rt_vm(vm)) {
		register_rt_vm_pm1a_ctl_handler(vm);
	}
}

/**
 * @pre vm_id < CONFIG_MAX_VM_NUM && vm_config != NULL && rtn_vm != NULL
 * @pre vm->state == VM_POWERED_OFF
 */
int32_t create_vm(uint16_t vm_id, struct acrn_vm_config *vm_config, struct acrn_vm **rtn_vm)
{
	struct acrn_vm *vm = NULL;
	int32_t status = 0;
	bool need_cleanup = false;
	uint32_t i;
	uint16_t pcpu_id;

	/* Allocate memory for virtual machine */
	vm = &vm_array[vm_id];
	(void)memset((void *)vm, 0U, sizeof(struct acrn_vm));
	vm->vm_id = vm_id;
	vm->hw.created_vcpus = 0U;
	vm->emul_mmio_regions = 0U;

	init_ept_mem_ops(vm);
	vm->arch_vm.nworld_eptp = vm->arch_vm.ept_mem_ops.get_pml4_page(vm->arch_vm.ept_mem_ops.info);
	sanitize_pte((uint64_t *)vm->arch_vm.nworld_eptp, &vm->arch_vm.ept_mem_ops);

	/* Register default handlers for PIO & MMIO if it is, SOS VM or Pre-launched VM */
	if ((vm_config->load_order == SOS_VM) || (vm_config->load_order == PRE_LAUNCHED_VM)) {
		register_pio_default_emulation_handler(vm);
		register_mmio_default_emulation_handler(vm);
	}

	(void)memcpy_s(&vm->uuid[0], sizeof(vm->uuid),
		&vm_config->uuid[0], sizeof(vm_config->uuid));

	if (is_sos_vm(vm)) {
		/* Only for SOS_VM */
		create_sos_vm_e820(vm);
		prepare_sos_vm_memmap(vm);

		status = init_vm_boot_info(vm);
		if (status != 0) {
			need_cleanup = true;
		}
	} else {
		/* For PRE_LAUNCHED_VM and POST_LAUNCHED_VM */
		if ((vm_config->guest_flags & GUEST_FLAG_SECURE_WORLD_ENABLED) != 0U) {
			vm->sworld_control.flag.supported = 1U;
		}
		if (vm->sworld_control.flag.supported != 0UL) {
			struct memory_ops *ept_mem_ops = &vm->arch_vm.ept_mem_ops;

			ept_add_mr(vm, (uint64_t *)vm->arch_vm.nworld_eptp,
				hva2hpa(ept_mem_ops->get_sworld_memory_base(ept_mem_ops->info)),
				TRUSTY_EPT_REBASE_GPA, TRUSTY_RAM_SIZE, EPT_WB | EPT_RWX);
		}
		if (vm_config->name[0] == '\0') {
			/* if VM name is not configured, specify with VM ID */
			snprintf(vm_config->name, 16, "ACRN VM_%d", vm_id);
		}

		 if (vm_config->load_order == PRE_LAUNCHED_VM) {
			create_prelaunched_vm_e820(vm);
			prepare_prelaunched_vm_memmap(vm, vm_config);
			status = init_vm_boot_info(vm);
		 }
	}

	if (status == 0) {
		prepare_epc_vm_memmap(vm);

		spinlock_init(&vm->vm_lock);

		vm->arch_vm.vlapic_state = VM_VLAPIC_XAPIC;
		vm->intr_inject_delay_delta = 0UL;

		/* Set up IO bit-mask such that VM exit occurs on
		 * selected IO ranges
		 */
		setup_io_bitmap(vm);

		vm_setup_cpu_state(vm);

		register_pm_io_handler(vm);

		if (!is_lapic_pt_configured(vm)) {
			vpic_init(vm);
		}

		/* Create virtual uart;*/
		vuart_init(vm, vm_config->vuart);

		if (is_rt_vm(vm) || !is_postlaunched_vm(vm)) {
			vrtc_init(vm);
		}

		vpci_init(vm);
		enable_iommu();

		register_reset_port_handler(vm);

		/* vpic wire_mode default is INTR */
		vm->wire_mode = VPIC_WIRE_INTR;

		/* Init full emulated vIOAPIC instance */
		if (!is_lapic_pt_configured(vm)) {
			vioapic_init(vm);
		}

		/* Populate return VM handle */
		*rtn_vm = vm;
		vm->sw.io_shared_page = NULL;
		if ((vm_config->load_order == POST_LAUNCHED_VM) && ((vm_config->guest_flags & GUEST_FLAG_IO_COMPLETION_POLLING) != 0U)) {
			/* enable IO completion polling mode per its guest flags in vm_config. */
			vm->sw.is_completion_polling = true;
		}
		status = set_vcpuid_entries(vm);
		if (status == 0) {
			vm->state = VM_CREATED;
		} else {
			need_cleanup = true;
		}
	}

	if (need_cleanup) {
		if (vm->arch_vm.nworld_eptp != NULL) {
			(void)memset(vm->arch_vm.nworld_eptp, 0U, PAGE_SIZE);
		}
	}

	if (status == 0) {
		/* We have assumptions:
		 *   1) vcpus used by SOS has been offlined by DM before UOS re-use it.
		 *   2) vcpu_affinity[] passed sanitization is OK for vcpu creating.
		 */
		for (i = 0U; i < vm_config->vcpu_num; i++) {
			pcpu_id = ffs64(vm_config->vcpu_affinity[i]);
			status = prepare_vcpu(vm, pcpu_id);
			if (status != 0) {
				break;
			}
		}
	}

	return status;
}

/*
 * @pre vm != NULL
 */
int32_t shutdown_vm(struct acrn_vm *vm)
{
	uint16_t i;
	uint64_t mask = 0UL;
	struct acrn_vcpu *vcpu = NULL;
	struct acrn_vm_config *vm_config = NULL;
	int32_t ret = 0;

	pause_vm(vm);

	/* Only allow shutdown paused vm */
	if (vm->state == VM_PAUSED) {
		vm->state = VM_POWERED_OFF;

		foreach_vcpu(i, vm, vcpu) {
			reset_vcpu(vcpu);
			offline_vcpu(vcpu);

			if (is_lapic_pt_enabled(vcpu)) {
				bitmap_set_nolock(vcpu->pcpu_id, &mask);
				make_pcpu_offline(vcpu->pcpu_id);
			}
		}

		wait_pcpus_offline(mask);

		if (is_lapic_pt_configured(vm) && !start_pcpus(mask)) {
			pr_fatal("Failed to start all cpus in mask(0x%llx)", mask);
			ret = -ETIMEDOUT;
		}

		vm_config = get_vm_config(vm->vm_id);
		vm_config->guest_flags &= ~DM_OWNED_GUEST_FLAG_MASK;

		if (is_sos_vm(vm)) {
			sbuf_reset();
		}

		vpci_cleanup(vm);

		vuart_deinit(vm);

		ptdev_release_all_entries(vm);

		/* Free iommu */
		destroy_iommu_domain(vm->iommu);

		/* Free EPT allocated resources assigned to VM */
		destroy_ept(vm);

		ret = 0;
	} else {
	        ret = -EINVAL;
	}

	/* Return status to caller */
	return ret;
}

/**
 *  * @pre vm != NULL
 */
void start_vm(struct acrn_vm *vm)
{
	struct acrn_vcpu *bsp = NULL;

	vm->state = VM_STARTED;

	/* Only start BSP (vid = 0) and let BSP start other APs */
	bsp = vcpu_from_vid(vm, BOOT_CPU_ID);
	schedule_vcpu(bsp);
}

/**
 *  * @pre vm != NULL
 */
int32_t reset_vm(struct acrn_vm *vm)
{
	uint16_t i;
	struct acrn_vcpu *vcpu = NULL;
	int32_t ret;

	if (vm->state == VM_PAUSED) {
		foreach_vcpu(i, vm, vcpu) {
			reset_vcpu(vcpu);
		}
		/*
		 * Set VM vLAPIC state to VM_VLAPIC_XAPIC
		 */

		vm->arch_vm.vlapic_state = VM_VLAPIC_XAPIC;

		if (is_sos_vm(vm)) {
			(void )vm_sw_loader(vm);
		}

		reset_vm_ioreqs(vm);
		vioapic_reset(vm);
		destroy_secure_world(vm, false);
		vm->sworld_control.flag.active = 0UL;
		vm->state = VM_CREATED;

		ret = 0;
	} else {
		ret = -1;
	}

	return ret;
}

/**
 *  * @pre vm != NULL
 */
void pause_vm(struct acrn_vm *vm)
{
	uint16_t i;
	struct acrn_vcpu *vcpu = NULL;

	if (vm->state != VM_PAUSED) {
		if (is_rt_vm(vm)) {
			/**
			 * For RTVM, we can only pause its vCPUs when it stays at following states:
			 *  - It is powering off by itself
			 *  - It is created but doesn't start
			 */
			if ((vm->state == VM_POWERING_OFF) || (vm->state == VM_CREATED)) {
				foreach_vcpu(i, vm, vcpu) {
					pause_vcpu(vcpu, VCPU_ZOMBIE);
				}

				vm->state = VM_PAUSED;
			}
		} else {
			foreach_vcpu(i, vm, vcpu) {
				pause_vcpu(vcpu, VCPU_ZOMBIE);
			}

			vm->state = VM_PAUSED;
		}
	}
}

/**
 * @brief Resume vm from S3 state
 *
 * To resume vm after guest enter S3 state:
 * - reset BSP
 * - BSP will be put to real mode with entry set as wakeup_vec
 * - init_vmcs BSP. We could call init_vmcs here because we know current
 *   pcpu is mapped to BSP of vm.
 *
 * @vm[in]		vm pointer to vm data structure
 * @wakeup_vec[in]	The resume address of vm
 *
 * @pre vm != NULL
 */
void resume_vm_from_s3(struct acrn_vm *vm, uint32_t wakeup_vec)
{
	struct acrn_vcpu *bsp = vcpu_from_vid(vm, BOOT_CPU_ID);

	vm->state = VM_STARTED;

	reset_vcpu(bsp);

	/* When SOS resume from S3, it will return to real mode
	 * with entry set to wakeup_vec.
	 */
	set_vcpu_startup_entry(bsp, wakeup_vec);

	init_vmcs(bsp);
	schedule_vcpu(bsp);
	switch_to_idle(default_idle);
}

/**
 * Prepare to create vm/vcpu for vm
 *
 * @pre vm_id < CONFIG_MAX_VM_NUM && vm_config != NULL
 */
void prepare_vm(uint16_t vm_id, struct acrn_vm_config *vm_config)
{
	int32_t err = 0;
	struct acrn_vm *vm = NULL;

	err = create_vm(vm_id, vm_config, &vm);

	if (err == 0) {
		if (is_prelaunched_vm(vm)) {
			build_vacpi(vm);
		}

		(void )vm_sw_loader(vm);

		/* start vm BSP automatically */
		start_vm(vm);

		pr_acrnlog("Start VM id: %x name: %s", vm_id, vm_config->name);
	}
}

/**
 * @pre vm_config != NULL
 */
void launch_vms(uint16_t pcpu_id)
{
	uint16_t vm_id, bsp_id;
	struct acrn_vm_config *vm_config;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		vm_config = get_vm_config(vm_id);
		if ((vm_config->load_order == SOS_VM) || (vm_config->load_order == PRE_LAUNCHED_VM)) {
			if (vm_config->load_order == SOS_VM) {
				sos_vm_ptr = &vm_array[vm_id];
			}

			bsp_id = get_vm_bsp_pcpu_id(vm_config);
			if (pcpu_id == bsp_id) {
				prepare_vm(vm_id, vm_config);
			}
		}
	}
}

/*
 * @brief Update state of vLAPICs of a VM
 * vLAPICs of VM switch between modes in an asynchronous fashion. This API
 * captures the "transition" state triggered when one vLAPIC switches mode.
 * When the VM is created, the state is set to "xAPIC" as all vLAPICs are setup
 * in xAPIC mode.
 *
 * Upon reset, all LAPICs switch to xAPIC mode accroding to SDM 10.12.5
 * Considering VM uses x2apic mode for vLAPIC, in reset or shutdown flow, vLAPIC state
 * moves to "xAPIC" directly without going thru "transition".
 *
 * VM_VLAPIC_X2APIC - All the online vCPUs/vLAPICs of this VM use x2APIC mode
 * VM_VLAPIC_XAPIC - All the online vCPUs/vLAPICs of this VM use xAPIC mode
 * VM_VLAPIC_DISABLED - All the online vCPUs/vLAPICs of this VM are in Disabled mode
 * VM_VLAPIC_TRANSITION - Online vCPUs/vLAPICs of this VM are in between transistion
 *
 * TODO: offline_vcpu need to call this API to reflect the status of rest of the
 * vLAPICs that are online.
 *
 * @pre vm != NULL
 */
void update_vm_vlapic_state(struct acrn_vm *vm)
{
	uint16_t i;
	struct acrn_vcpu *vcpu;
	uint16_t vcpus_in_x2apic, vcpus_in_xapic;
	enum vm_vlapic_state vlapic_state = VM_VLAPIC_XAPIC;

	vcpus_in_x2apic = 0U;
	vcpus_in_xapic = 0U;
	spinlock_obtain(&vm->vm_lock);
	foreach_vcpu(i, vm, vcpu) {
		if (is_x2apic_enabled(vcpu_vlapic(vcpu))) {
			vcpus_in_x2apic++;
		} else if (is_xapic_enabled(vcpu_vlapic(vcpu))) {
			vcpus_in_xapic++;
		} else {
			/*
			 * vCPU is using vLAPIC in Disabled mode
			 */
		}
	}

	if ((vcpus_in_x2apic == 0U) && (vcpus_in_xapic == 0U)) {
		/*
		 * Check if the counts vcpus_in_x2apic and vcpus_in_xapic are zero
		 * VM_VLAPIC_DISABLED
		 */
		vlapic_state = VM_VLAPIC_DISABLED;
	} else if ((vcpus_in_x2apic != 0U) && (vcpus_in_xapic != 0U)) {
		/*
		 * Check if the counts vcpus_in_x2apic and vcpus_in_xapic are non-zero
		 * VM_VLAPIC_TRANSITION
		 */
		vlapic_state = VM_VLAPIC_TRANSITION;
	} else if (vcpus_in_x2apic != 0U) {
		/*
		 * Check if the counts vcpus_in_x2apic is non-zero
		 * VM_VLAPIC_X2APIC
		 */
		vlapic_state = VM_VLAPIC_X2APIC;
	} else {
		/*
		 * Count vcpus_in_xapic is non-zero
		 * VM_VLAPIC_XAPIC
		 */
		vlapic_state = VM_VLAPIC_XAPIC;
	}

	vm->arch_vm.vlapic_state = vlapic_state;
	spinlock_release(&vm->vm_lock);
}

/*
 * @brief Check state of vLAPICs of a VM
 *
 * @pre vm != NULL
 */
enum vm_vlapic_state check_vm_vlapic_state(const struct acrn_vm *vm)
{
	enum vm_vlapic_state vlapic_state;

	vlapic_state = vm->arch_vm.vlapic_state;
	return vlapic_state;
}

/**
 * if there is RT VM return true otherwise return false.
 */
bool has_rt_vm(void)
{
	uint16_t vm_id;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if (is_rt_vm(get_vm_from_vmid(vm_id))) {
			break;
		}
	}

	return ((vm_id == CONFIG_MAX_VM_NUM) ? false : true);
}

void make_shutdown_vm_request(uint16_t pcpu_id)
{
	bitmap_set_lock(NEED_SHUTDOWN_VM, &per_cpu(pcpu_flag, pcpu_id));
	if (get_pcpu_id() != pcpu_id) {
		send_single_ipi(pcpu_id, VECTOR_NOTIFY_VCPU);
	}
}

bool need_shutdown_vm(uint16_t pcpu_id)
{
	return bitmap_test_and_clear_lock(NEED_SHUTDOWN_VM, &per_cpu(pcpu_flag, pcpu_id));
}
