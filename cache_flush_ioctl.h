/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef CACHE_FLUSH_IOCTL_H
#define CACHE_FLUSH_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#include "microreset_profiles.h"

#define CACHE_FLUSH_IOC_MAGIC 'F'
#define CACHE_FLUSH_IOC_RUN _IOW(CACHE_FLUSH_IOC_MAGIC, 0, struct cache_flush_run)

struct cache_flush_run {
	__u64 target_ns;
	__u32 reset_profile;
	__u32 entry_offset;
};

struct cache_flush_results {
	__u64 start;
	__u64 end;
	__u64 actual_time;
	__u64 last_chunk;
	__u64 chunks;
	__u64 total_iterations;
};

#endif
