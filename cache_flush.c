// SPDX-License-Identifier: GPL-2.0

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/csr.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "cache_flush_ioctl.h"

#define DEVICE_NAME "cache_flush"
#define BUFFER_SIZE (1U << 20)

#define TRIAL_NUM 100ULL
#define TARGET_CHUNKS 32ULL
#define MAX_CHUNK 100000ULL
#define MICRORESET_BTB_THRASH_COUNT 512

typedef void (*microreset_fn_t)(void);
typedef void (*workload_fn_t)(void);

/* Keep executable pages exclusive to the uploaded program buffer. */
static char kernel_buffer[BUFFER_SIZE] __aligned(PAGE_SIZE);
static size_t data_size;

/* Global GPIO number used as the external measurement gate. */
static int trigger_gpio = -1;
module_param(trigger_gpio, int, 0444);
MODULE_PARM_DESC(trigger_gpio, "GPIO number used as measurement trigger");

/* CPU used for calibration and measurement. -1 keeps the caller's affinity. */
static int run_cpu = -1;
module_param(run_cpu, int, 0444);
MODULE_PARM_DESC(run_cpu, "CPU used for workload execution (-1: do not repin)");

/* Give counters and the external measurement setup time to settle once. */
static unsigned int settle_cycles = 256;
module_param(settle_cycles, uint, 0644);
MODULE_PARM_DESC(settle_cycles,
		 "Approximate loop iterations to wait before measurement");

static struct cache_flush_results last_run;
static DEFINE_MUTEX(cache_flush_lock);
static bool gpio_active;

struct kernel_tlb_range {
	unsigned long start;
	unsigned long end;
};

static pte_t *kernel_pte(unsigned long address)
{
	unsigned int shifts[5];
	unsigned int levels;
	unsigned long satp = csr_read(CSR_SATP);
	unsigned long root_phys = (satp & SATP_PPN) << PAGE_SHIFT;
	unsigned long linear_offset;
	unsigned long *table;
	unsigned int level;

	/*
	 * Avoid init_mm and the variable-depth p4d/pud helpers: none of those
	 * symbols is exported to out-of-tree modules on RISC-V. The active mm's
	 * PGD is the virtual alias of the SATP root, which also gives us the
	 * physical-to-linear-map offset needed for every child table.
	 */
	if (!current->active_mm || !current->active_mm->pgd || !root_phys)
		return NULL;
	table = (unsigned long *)current->active_mm->pgd;
	linear_offset = (unsigned long)table - root_phys;

#ifdef CONFIG_64BIT
	switch (satp & (~0UL << SATP_MODE_SHIFT)) {
	case SATP_MODE_57:
		shifts[0] = 48;
		shifts[1] = 39;
		shifts[2] = 30;
		shifts[3] = 21;
		shifts[4] = PAGE_SHIFT;
		levels = 5;
		break;
	case SATP_MODE_48:
		shifts[0] = 39;
		shifts[1] = 30;
		shifts[2] = 21;
		shifts[3] = PAGE_SHIFT;
		levels = 4;
		break;
	case SATP_MODE_39:
		shifts[0] = 30;
		shifts[1] = 21;
		shifts[2] = PAGE_SHIFT;
		levels = 3;
		break;
	default:
		return NULL;
	}
#else
	if (satp & SATP_MODE_32) {
		shifts[0] = 22;
		shifts[1] = PAGE_SHIFT;
		levels = 2;
	} else {
		return NULL;
	}
#endif

	for (level = 0; level < levels; ++level) {
		unsigned long *entry = table +
			((address >> shifts[level]) & (PTRS_PER_PTE - 1));
		unsigned long value = READ_ONCE(*entry);

		if (!(value & _PAGE_PRESENT))
			return NULL;
		if (level == levels - 1)
			return (pte_t *)entry;
		if (value & _PAGE_LEAF)
			return NULL;

		table = (unsigned long *)
			(((value & _PAGE_PFN_MASK) <<
			  (PAGE_SHIFT - _PAGE_PFN_SHIFT)) + linear_offset);
	}

	return NULL;
}

static void local_flush_kernel_tlb_pages(void *info)
{
	const struct kernel_tlb_range *range = info;
	unsigned long address;

	for (address = range->start; address < range->end;
	     address += PAGE_SIZE)
		local_flush_tlb_page(address);
}

static void dutizer_flush_kernel_tlb_range(unsigned long start,
					    unsigned long end)
{
	struct kernel_tlb_range range = {
		.start = start,
		.end = end,
	};

	/* flush_tlb_kernel_range() is not exported to external modules. */
	on_each_cpu(local_flush_kernel_tlb_pages, &range, 1);
}

static int make_kernel_buffer_rwx(void)
{
	const unsigned long start = (unsigned long)kernel_buffer;
	const unsigned long end = start + sizeof(kernel_buffer);
	unsigned long address;
	pte_t *pte;

	if (!IS_ALIGNED(start, PAGE_SIZE) || !IS_ALIGNED(end, PAGE_SIZE))
		return -EINVAL;

	/* Validate the complete walk before changing any entry. */
	for (address = start; address < end; address += PAGE_SIZE) {
		pte = kernel_pte(address);
		if (!pte || !pte_present(*pte))
			return -EFAULT;
	}

	for (address = start; address < end; address += PAGE_SIZE) {
		pte = kernel_pte(address);
		set_pte(pte, __pte(pte_val(*pte) |
			_PAGE_READ | _PAGE_WRITE | _PAGE_EXEC));
	}

	/* Publish the PTE writes, invalidate every hart, then synchronize I$. */
	smp_wmb();
	dutizer_flush_kernel_tlb_range(start, end);
	flush_icache_range(start, end);
	return 0;
}

/* Uploaded code does not carry a kernel CFI type hash. */
static noinline __nocfi void execute_workload(workload_fn_t workload)
{
	workload();
}

static inline void rv_fence_rw(void)
{
	asm volatile("fence rw,rw" ::: "memory");
}

static __always_inline void riscv_cbo_flush(unsigned long address)
{
	asm volatile(
		".option push\n"
		".option arch,+zicbom\n"
		"cbo.flush 0(%0)\n"
		".option pop\n"
		:
		: "r"(address)
		: "memory");
}

static void microreset_cache_cbo_flush(void)
{
	unsigned long address;
	unsigned long start = (unsigned long)kernel_buffer;
	unsigned long end = start + data_size;
	unsigned long block_size = riscv_cbom_block_size;

	/*
	 * Module loading requires a valid power-of-two Zicbom block size. Align
	 * the uploaded range down and issue exactly one cbo.flush per block.
	 */
	start &= ~(block_size - 1UL);
	for (address = start; address < end; address += block_size)
		riscv_cbo_flush(address);
	rv_fence_rw();
}

static void microreset_train_branch_predictor(void)
{
	volatile int state = 0;
	int i;

	for (i = 0; i < 4096; ++i) {
		if ((i ^ (i >> 1)) & 1)
			state++;
		else
			state--;
		if ((i & 31) == 7 || (i & 63) == 9)
			barrier();
	}
	(void)state;
}

static noinline void microreset_btb_thrash(void)
{
	unsigned long seed = (unsigned long)ktime_get_ns();

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
	asm volatile(
		".option push\n"
		".option norvc\n"
		"addi t0, zero, 0\n"
		"addi t1, zero, 0\n"
		"add t0, t0, %0\n"
		".rept " STR(MICRORESET_BTB_THRASH_COUNT) "\n"
		"xori t0, t0, 0x3f\n"
		"andi t2, t0, 1\n"
		"beqz t2, 1f\n"
		"nop\n"
		"1:\n"
		"addi t1, t1, 1\n"
		".endr\n"
		".option pop\n"
		:
		: "r"(seed)
		: "t0", "t1", "t2", "memory");
	rv_fence_rw();
}

static void microreset_drain_pipeline(void)
{
	unsigned int i;

	rv_fence_rw();
	for (i = 0; i < 1024; ++i)
		asm volatile("nop");
	rv_fence_rw();
}

static void microreset_sync_icache(void)
{
	local_flush_icache_all();
}

static void microreset_none(void) { }
static void microreset_all(void)
{
	microreset_cache_cbo_flush();
	microreset_train_branch_predictor();
	microreset_btb_thrash();
	microreset_sync_icache();
	microreset_drain_pipeline();
}
static void microreset_only_cache(void) { microreset_cache_cbo_flush(); }
static void microreset_only_bp(void) { microreset_train_branch_predictor(); }
static void microreset_only_btb(void) { microreset_btb_thrash(); }
static void microreset_only_pipe(void) { microreset_drain_pipeline(); }
static void microreset_only_icache(void) { microreset_sync_icache(); }
static void microreset_all_except_cache(void)
{
	microreset_train_branch_predictor();
	microreset_btb_thrash();
	microreset_sync_icache();
	microreset_drain_pipeline();
}
static void microreset_all_except_bp(void)
{
	microreset_cache_cbo_flush();
	microreset_btb_thrash();
	microreset_sync_icache();
	microreset_drain_pipeline();
}
static void microreset_all_except_btb(void)
{
	microreset_cache_cbo_flush();
	microreset_train_branch_predictor();
	microreset_sync_icache();
	microreset_drain_pipeline();
}
static void microreset_all_except_pipe(void)
{
	microreset_cache_cbo_flush();
	microreset_train_branch_predictor();
	microreset_btb_thrash();
	microreset_sync_icache();
}
static void microreset_all_except_icache(void)
{
	microreset_cache_cbo_flush();
	microreset_train_branch_predictor();
	microreset_btb_thrash();
	microreset_drain_pipeline();
}

static microreset_fn_t microreset_profile_fn(u32 profile)
{
	switch (profile) {
	case MICRORESET_PROFILE_NONE: return microreset_none;
	case MICRORESET_PROFILE_ALL: return microreset_all;
	case MICRORESET_PROFILE_ONLY_CACHE: return microreset_only_cache;
	case MICRORESET_PROFILE_ONLY_BP: return microreset_only_bp;
	case MICRORESET_PROFILE_ONLY_BTB: return microreset_only_btb;
	case MICRORESET_PROFILE_ONLY_PIPE_DRAIN: return microreset_only_pipe;
	case MICRORESET_PROFILE_ONLY_ICACHE_SYNC: return microreset_only_icache;
	case MICRORESET_PROFILE_ALL_EXCEPT_CACHE:
		return microreset_all_except_cache;
	case MICRORESET_PROFILE_ALL_EXCEPT_BP: return microreset_all_except_bp;
	case MICRORESET_PROFILE_ALL_EXCEPT_BTB: return microreset_all_except_btb;
	case MICRORESET_PROFILE_ALL_EXCEPT_PIPE_DRAIN:
		return microreset_all_except_pipe;
	case MICRORESET_PROFILE_ALL_EXCEPT_ICACHE_SYNC:
		return microreset_all_except_icache;
	default: return NULL;
	}
}

static __always_inline void settle_measurement(void)
{
	unsigned int i;

	for (i = 0; i < READ_ONCE(settle_cycles); ++i)
		asm volatile("nop");
}

static __always_inline void run_one_iteration(microreset_fn_t reset,
					       workload_fn_t workload)
{
	/* Exactly one selected reset precedes every workload invocation. */
	reset();
	execute_workload(workload);
}

static void run_dynamic_workload(const struct cache_flush_run *request,
				 microreset_fn_t reset,
				 workload_fn_t workload)
{
	struct cache_flush_results result = { };
	u64 trial_start, trial_end, trial_time;
	u64 one_iter, expected_num;
	u64 chunk;
	u64 i;
	int cpu;

	preempt_disable();
	cpu = smp_processor_id();

	/* Calibrate the reset and workload cost used inside the measurement. */
	trial_start = ktime_get_ns();
	for (i = 0; i < TRIAL_NUM; i++)
		run_one_iteration(reset, workload);
	trial_end = ktime_get_ns();

	trial_time = trial_end - trial_start;
	one_iter = max_t(u64, div_u64(trial_time, TRIAL_NUM), 1);
	expected_num = div_u64(request->target_ns, one_iter);
	chunk = clamp_t(u64, div_u64(expected_num, TARGET_CHUNKS),
			1, MAX_CHUNK);

	/* One reset-only preparation, followed by one settling period. */
	reset();
	settle_measurement();

	gpio_set_value(trigger_gpio, 1);
	result.start = ktime_get_ns();
	for (;;) {
		for (i = 0; i < chunk; i++)
			run_one_iteration(reset, workload);

		result.chunks++;
		result.total_iterations += chunk;
		result.end = ktime_get_ns();
		if (result.end - result.start >= request->target_ns)
			break;

		{
			u64 elapsed = result.end - result.start;
			u64 remaining = request->target_ns - elapsed;
			u64 next_chunk = div_u64(remaining, one_iter);

			chunk = clamp_t(u64, next_chunk, 1, chunk);
		}
	}
	gpio_set_value(trigger_gpio, 0);

	result.actual_time = result.end - result.start;
	result.last_chunk = chunk;
	last_run = result;
	preempt_enable();

	pr_info("cache_flush: cpu=%d target=%llu ns actual=%llu ns iterations=%llu chunks=%llu last_chunk=%llu profile=%u\n",
		cpu, request->target_ns, result.actual_time,
		result.total_iterations, result.chunks, result.last_chunk,
		request->reset_profile);
}

static int cache_open(struct inode *inode, struct file *file)
{
	int ret;

	if (mutex_lock_interruptible(&cache_flush_lock))
		return -ERESTARTSYS;
	if (gpio_active) {
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = gpio_request(trigger_gpio, "cache_flush_trigger");
	if (ret)
		goto out_unlock;
	if (gpiod_cansleep(gpio_to_desc(trigger_gpio))) {
		ret = -EINVAL;
		goto out_free_gpio;
	}
	ret = gpio_direction_output(trigger_gpio, 0);
	if (ret)
		goto out_free_gpio;

	gpio_active = true;
	ret = nonseekable_open(inode, file);
	if (!ret)
		goto out_unlock;
	gpio_active = false;

out_free_gpio:
	gpio_free(trigger_gpio);
out_unlock:
	mutex_unlock(&cache_flush_lock);
	return ret;
}

static int cache_release(struct inode *inode, struct file *file)
{
	mutex_lock(&cache_flush_lock);
	if (gpio_active) {
		gpio_set_value(trigger_gpio, 0);
		gpio_free(trigger_gpio);
		gpio_active = false;
	}
	mutex_unlock(&cache_flush_lock);
	return 0;
}

static ssize_t cache_write(struct file *file, const char __user *user_buffer,
			   size_t count, loff_t *offset)
{
	if (!count)
		return -EINVAL;
	if (count > BUFFER_SIZE)
		return -EFBIG;

	if (mutex_lock_interruptible(&cache_flush_lock))
		return -ERESTARTSYS;
	if (copy_from_user(kernel_buffer, user_buffer, count)) {
		mutex_unlock(&cache_flush_lock);
		return -EFAULT;
	}

	data_size = count;
	smp_wmb();
	flush_icache_range((unsigned long)kernel_buffer,
			   (unsigned long)kernel_buffer + count);
	mutex_unlock(&cache_flush_lock);
	return count;
}

static ssize_t cache_read(struct file *file, char __user *user_buffer,
			  size_t count, loff_t *offset)
{
	struct cache_flush_results result;

	if (mutex_lock_interruptible(&cache_flush_lock))
		return -ERESTARTSYS;
	result = last_run;
	mutex_unlock(&cache_flush_lock);

	return simple_read_from_buffer(user_buffer, count, offset,
				       &result, sizeof(result));
}

static long cache_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cache_flush_run request;
	microreset_fn_t reset;
	workload_fn_t workload;
	cpumask_var_t old_mask;
	long ret = 0;
	bool repinned = false;

	if (cmd != CACHE_FLUSH_IOC_RUN)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.target_ns)
		return -EINVAL;

	reset = microreset_profile_fn(request.reset_profile);
	if (!reset)
		return -EINVAL;

	if (mutex_lock_interruptible(&cache_flush_lock))
		return -ERESTARTSYS;
	if (!data_size) {
		ret = -ENOEXEC;
		goto out_unlock;
	}
	if (request.entry_offset >= data_size) {
		ret = -ENOEXEC;
		goto out_unlock;
	}
	workload = (workload_fn_t)(kernel_buffer + request.entry_offset);

	if (run_cpu >= 0) {
		if (run_cpu >= nr_cpu_ids || !cpu_online(run_cpu)) {
			ret = -EINVAL;
			goto out_unlock;
		}
		if (!cpumask_test_cpu(run_cpu, current->cpus_ptr)) {
			pr_err("cache_flush: CPU %d is outside caller's allowed CPU mask\n",
			       run_cpu);
			ret = -EINVAL;
			goto out_unlock;
		}
		if (!alloc_cpumask_var(&old_mask, GFP_KERNEL)) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		cpumask_copy(old_mask, current->cpus_ptr);
		ret = set_cpus_allowed_ptr(current, cpumask_of(run_cpu));
		if (ret)
			goto out_free_mask;
		repinned = true;
	}

	pr_info("cache_flush: target=%llu ns program_size=%zu bytes entry=%u profile=%u requested_cpu=%d\n",
		request.target_ns, data_size, request.entry_offset,
		request.reset_profile, run_cpu);
	run_dynamic_workload(&request, reset, workload);
	file->f_pos = 0;

	if (repinned) {
		long restore_ret = set_cpus_allowed_ptr(current, old_mask);

		if (restore_ret) {
			pr_err("cache_flush: failed to restore caller CPU affinity: %ld\n",
			       restore_ret);
			ret = restore_ret;
		}
	}

out_free_mask:
	if (run_cpu >= 0)
		free_cpumask_var(old_mask);
out_unlock:
	mutex_unlock(&cache_flush_lock);
	return ret;
}

static const struct file_operations cache_fops = {
	.owner = THIS_MODULE,
	.open = cache_open,
	.release = cache_release,
	.read = cache_read,
	.write = cache_write,
	.unlocked_ioctl = cache_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice cache_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &cache_fops,
	.mode = 0600,
};

static int __init cache_init(void)
{
	int ret;

	if (!riscv_cbom_block_size || !is_power_of_2(riscv_cbom_block_size)) {
		pr_err("cache_flush: RISC-V Zicbom with a valid cbom block size is required\n");
		return -EOPNOTSUPP;
	}
	if (!gpio_is_valid(trigger_gpio)) {
		pr_err("cache_flush: trigger_gpio=<n> is required\n");
		return -EINVAL;
	}
	if (run_cpu < -1 || run_cpu >= nr_cpu_ids) {
		pr_err("cache_flush: invalid run_cpu=%d\n", run_cpu);
		return -EINVAL;
	}
	if (run_cpu >= 0 && !cpu_online(run_cpu)) {
		pr_err("cache_flush: run_cpu=%d is not online\n", run_cpu);
		return -EINVAL;
	}
	ret = make_kernel_buffer_rwx();
	if (ret) {
		pr_err("cache_flush: failed to make program buffer RWX: %d\n",
		       ret);
		return ret;
	}

	ret = misc_register(&cache_device);
	if (ret) {
		pr_err("cache_flush: failed to register device: %d\n", ret);
		return ret;
	}

	memset(&last_run, 0, sizeof(last_run));
	gpio_active = false;
	pr_info("cache_flush: /dev/%s registered, trigger GPIO=%d, cbom=%u bytes, settle=%u cycles\n",
		DEVICE_NAME, trigger_gpio, riscv_cbom_block_size, settle_cycles);
	return 0;
}

static void __exit cache_exit(void)
{
	misc_deregister(&cache_device);
	pr_info("cache_flush: driver unloaded\n");
}

module_init(cache_init);
module_exit(cache_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DUTizer contributors");
MODULE_DESCRIPTION("DUTizer RISC-V kernel measurement driver");
