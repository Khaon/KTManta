/*
 *  drivers/cpufreq/cpufreq_ktoonservative.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>

/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define DEF_FREQUENCY_UP_THRESHOLD		(57)
#define DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG	(58)
#define DEF_FREQUENCY_DOWN_THRESHOLD		(52)
#define DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG	(35)
#define DEF_BOOST_CPU				(800000)
#define DEF_BOOST_CPU_TURN_ON_2ND_CORE		(1)
#define DEF_BOOST_GPU				(350)
#define DEF_BOOST_HOLD_CYCLES			(22)
#define DEF_DISABLE_HOTPLUGGING			(0)
#define DEF_UP_FREQ_THRESHOLD_HOTPLUG 		(1200000)
#define DEF_DOWN_FREQ_THRESHOLD_HOTPLUG 	(800000)

/*
 * The polling frequency of this governor depends on the capability of
 * the processor. Default polling frequency is 1000 times the transition
 * latency of the processor. The governor will work on any processor with
 * transition latency <= 10mS, using appropriate sampling
 * rate.
 * For CPUs with transition latency > 10mS (mostly drivers with CPUFREQ_ETERNAL)
 * this governor will not work.
 * All times here are in uS.
 */
#define MIN_SAMPLING_RATE_RATIO			(2)

static unsigned int stored_sampling_rate;

static unsigned int Lblock_cycles_online = 0;
static unsigned int Lblock_cycles_offline = 0;
static unsigned int Lblock_cycles_raise = 0;
static unsigned int Lblock_cycles_reduce = 0;

static bool boostpulse_relayf = false;
static int boost_hold_cycles_cnt = 0;
static bool screen_is_on = true;

extern void ktoonservative_is_active(bool val);
extern void boost_the_gpu(int freq, int cycles);

#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(10)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

struct work_struct hotplug_offline_work;
struct work_struct hotplug_online_work;

static void do_dbs_timer(struct work_struct *work);

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	unsigned int down_skip;
	unsigned int requested_freq;
	int cpu;
	unsigned int enable:1;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, cs_cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/*
 * dbs_mutex protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int sampling_rate_screen_off;
	unsigned int sampling_down_factor;
	unsigned int up_threshold;
	unsigned int up_threshold_hotplug;
	unsigned int down_threshold;
	unsigned int down_threshold_hotplug;
	unsigned int block_cycles_online;
	unsigned int block_cycles_offline;
	unsigned int block_cycles_raise;
	unsigned int block_cycles_reduce;
	unsigned int boost_cpu;
	unsigned int boost_turn_on_2nd_core;
	unsigned int boost_gpu;
	unsigned int boost_hold_cycles;
	unsigned int disable_hotplugging;
	unsigned int no_2nd_cpu_screen_off;
	unsigned int ignore_nice;
	unsigned int freq_step_up;
	unsigned int freq_step_down;
	unsigned int up_freq_threshold_hotplug;
	unsigned int down_freq_threshold_hotplug;
} dbs_tuners_ins = {
	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.up_threshold_hotplug = DEF_FREQUENCY_UP_THRESHOLD_HOTPLUG,
	.down_threshold = DEF_FREQUENCY_DOWN_THRESHOLD,
	.down_threshold_hotplug = DEF_FREQUENCY_DOWN_THRESHOLD_HOTPLUG,
	.block_cycles_online=10,
	.block_cycles_offline=25,
	.block_cycles_raise=2,	
	.block_cycles_reduce=3,
	.boost_cpu = DEF_BOOST_CPU,
	.boost_turn_on_2nd_core = DEF_BOOST_CPU_TURN_ON_2ND_CORE,
	.boost_gpu = DEF_BOOST_GPU,
	.boost_hold_cycles = DEF_BOOST_HOLD_CYCLES,
	.disable_hotplugging = DEF_DISABLE_HOTPLUGGING,
	.no_2nd_cpu_screen_off = 1,
	.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR,
	.sampling_rate_screen_off = 45000,
	.ignore_nice = 0,
	.freq_step_down = 5,
	.freq_step_up = 5,
	.up_freq_threshold_hotplug = DEF_UP_FREQ_THRESHOLD_HOTPLUG,
	.down_freq_threshold_hotplug = DEF_DOWN_FREQ_THRESHOLD_HOTPLUG,
};

static inline u64 get_cpu_idle_time_jiffy(unsigned int cpu,
							u64 *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = jiffies_to_usecs(cur_wall_time);

	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);

	return idle_time;
}

/* keep track of frequency transitions */
static int
dbs_cpufreq_notifier(struct notifier_block *nb, unsigned long val,
		     void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpu_dbs_info_s *this_dbs_info = &per_cpu(cs_cpu_dbs_info,
							freq->cpu);

	struct cpufreq_policy *policy;

	if (!this_dbs_info->enable)
		return 0;

	policy = this_dbs_info->cur_policy;

	/*
	 * we only care if our internally tracked freq moves outside
	 * the 'valid' ranges of freqency available to us otherwise
	 * we do not change it
	*/
	if (this_dbs_info->requested_freq > policy->max
			|| this_dbs_info->requested_freq < policy->min)
		this_dbs_info->requested_freq = freq->new;

	return 0;
}

static struct notifier_block dbs_cpufreq_notifier_block = {
	.notifier_call = dbs_cpufreq_notifier
};

/************************** sysfs interface ************************/

static ssize_t show_boost_cpu(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", dbs_tuners_ins.boost_cpu / 1000);
}

static ssize_t show_up_freq_threshold_hotplug(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", dbs_tuners_ins.up_freq_threshold_hotplug/ 1000);
}

static ssize_t show_down_freq_threshold_hotplug(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", dbs_tuners_ins.down_freq_threshold_hotplug/ 1000);
}

/* cpufreq_ktoonservative Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(sampling_rate_screen_off, sampling_rate_screen_off);
show_one(sampling_down_factor, sampling_down_factor);
show_one(up_threshold, up_threshold);
show_one(up_threshold_hotplug, up_threshold_hotplug);
show_one(down_threshold, down_threshold);
show_one(down_threshold_hotplug, down_threshold_hotplug);
show_one(boost_turn_on_2nd_core, boost_turn_on_2nd_core);
show_one(boost_gpu, boost_gpu);
show_one(boost_hold_cycles, boost_hold_cycles);
show_one(disable_hotplugging, disable_hotplugging);
show_one(no_2nd_cpu_screen_off, no_2nd_cpu_screen_off);
show_one(ignore_nice_load, ignore_nice);
show_one(block_cycles_online, block_cycles_online);
show_one(block_cycles_offline, block_cycles_offline);
show_one(block_cycles_raise, block_cycles_raise);
show_one(block_cycles_reduce, block_cycles_reduce);
show_one(freq_step_down, freq_step_down);
show_one(freq_step_up, freq_step_up);

static ssize_t store_sampling_down_factor(struct kobject *a,
					  struct attribute *b,
					  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 1)
		return -EINVAL;

	dbs_tuners_ins.sampling_down_factor = input;
	return count;
}

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;
	boostpulse_relayf = false;
	dbs_tuners_ins.sampling_rate = input;
	return count;
}

static ssize_t store_sampling_rate_screen_off(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	dbs_tuners_ins.sampling_rate_screen_off = input;
	return count;
}

static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 ||
			input <= dbs_tuners_ins.down_threshold)
		return -EINVAL;

	dbs_tuners_ins.up_threshold = input;
	return count;
}

static ssize_t store_up_threshold_hotplug(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 ||
			input <= dbs_tuners_ins.down_threshold)
		return -EINVAL;

	dbs_tuners_ins.up_threshold_hotplug = input;
	return count;
}

static ssize_t store_up_freq_threshold_hotplug(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 100 otherwise freq will not fall */
	if (ret != 1 || input < 100 ||
			input >= 2100)
		return -EINVAL;

	dbs_tuners_ins.up_freq_threshold_hotplug = input * 1000;
	return count;
}

static ssize_t store_down_threshold(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	dbs_tuners_ins.down_threshold = input;
	return count;
}

static ssize_t store_down_threshold_hotplug(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	dbs_tuners_ins.down_threshold_hotplug = input;
	return count;
}

static ssize_t store_down_freq_threshold_hotplug(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 100 otherwise freq will not fall */
	if (ret != 1 || input < 100  ||
			input >= 2100)
		return -EINVAL;

	dbs_tuners_ins.down_freq_threshold_hotplug = input * 1000;
	return count;
}

static ssize_t store_block_cycles_online(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (input < 0)
		return -EINVAL;

	dbs_tuners_ins.block_cycles_online = input;
	return count;
}

static ssize_t store_block_cycles_offline(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (input < 0)
		return -EINVAL;

	dbs_tuners_ins.block_cycles_offline = input;
	return count;
}

static ssize_t store_block_cycles_raise(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (input < 0)
		return -EINVAL;

	dbs_tuners_ins.block_cycles_raise = input;
	return count;
}

static ssize_t store_block_cycles_reduce(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (input < 0)
		return -EINVAL;

	dbs_tuners_ins.block_cycles_reduce = input;
	return count;
}

static ssize_t store_boost_cpu(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input * 1000 > 2100000)
		input = 2100000;
	if (input * 1000 < 0)
		input = 0;
	dbs_tuners_ins.boost_cpu = input * 1000;
	return count;
}

static ssize_t store_boost_turn_on_2nd_core(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (input != 0 && input != 1)
		input = 0;

	dbs_tuners_ins.boost_turn_on_2nd_core = input;
	return count;
}

static ssize_t store_boost_gpu(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (input != 100 && input != 160 && input != 266 && input != 350 && input != 400 && input != 450 && input != 533 && input != 612 && input != 667 && input != 720)
		input = 0;

	dbs_tuners_ins.boost_gpu = input;
	return count;
}

static ssize_t store_boost_hold_cycles(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (input < 0)
		return -EINVAL;

	dbs_tuners_ins.boost_hold_cycles = input;
	return count;
}

static ssize_t store_disable_hotplugging(struct kobject *a, struct attribute *b, const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (input != 0 && input != 1)
		input = 0;

	dbs_tuners_ins.disable_hotplugging = input;
	return count;
}

static ssize_t store_no_2nd_cpu_screen_off(struct kobject *a, struct attribute *b, const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (input != 0 && input != 1)
		input = 0;

	dbs_tuners_ins.no_2nd_cpu_screen_off = input;
	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == dbs_tuners_ins.ignore_nice) /* nothing to do */
		return count;

	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(cs_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
	}
	return count;
}

static ssize_t store_freq_step_down(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	/* no need to test here if freq_step is zero as the user might actually
	 * want this, they would be crazy though :) */
	dbs_tuners_ins.freq_step_down = input;
	return count;
}

static ssize_t store_freq_step_up(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	/* no need to test here if freq_step is zero as the user might actually
	 * want this, they would be crazy though :) */
	dbs_tuners_ins.freq_step_up = input;
	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(sampling_rate_screen_off);
define_one_global_rw(sampling_down_factor);
define_one_global_rw(up_threshold);
define_one_global_rw(up_threshold_hotplug);
define_one_global_rw(down_threshold);
define_one_global_rw(down_threshold_hotplug);
define_one_global_rw(block_cycles_online);
define_one_global_rw(block_cycles_offline);
define_one_global_rw(block_cycles_raise);
define_one_global_rw(block_cycles_reduce);
define_one_global_rw(boost_cpu);
define_one_global_rw(boost_turn_on_2nd_core);
define_one_global_rw(boost_gpu);
define_one_global_rw(boost_hold_cycles);
define_one_global_rw(disable_hotplugging);
define_one_global_rw(no_2nd_cpu_screen_off);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(freq_step_down);
define_one_global_rw(freq_step_up);
define_one_global_rw(up_freq_threshold_hotplug);
define_one_global_rw(down_freq_threshold_hotplug);

static struct attribute *dbs_attributes[] = {
	&sampling_rate.attr,
	&sampling_rate_screen_off.attr,
	&sampling_down_factor.attr,
	&up_threshold.attr,
	&up_threshold_hotplug.attr,
	&down_threshold.attr,
	&down_threshold_hotplug.attr,
	&boost_cpu.attr,
	&boost_turn_on_2nd_core.attr,
	&boost_gpu.attr,
	&boost_hold_cycles.attr,
	&block_cycles_raise.attr,
	&block_cycles_reduce.attr,
	&block_cycles_online.attr,
	&block_cycles_offline.attr,
	&disable_hotplugging.attr,
	&no_2nd_cpu_screen_off.attr,
	&ignore_nice_load.attr,
	&freq_step_down.attr,
	&freq_step_up.attr,
	&up_freq_threshold_hotplug.attr,
	&down_freq_threshold_hotplug.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "ktoonservative",
};

/************************** sysfs end ************************/

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	unsigned int load = 0;
	unsigned int max_load = 0;
	unsigned int freq_target;

	struct cpufreq_policy *policy;
	unsigned int j;

	policy = this_dbs_info->cur_policy;

	if (boostpulse_relayf)
	{
		
		if (boost_hold_cycles_cnt >= dbs_tuners_ins.boost_hold_cycles)
		{
			boostpulse_relayf = false;
			boost_hold_cycles_cnt = 0;
		}
		boost_hold_cycles_cnt++;

		this_dbs_info->down_skip = 0;
		/* if we are already at full speed then break out early */
		if (this_dbs_info->requested_freq == policy->max || policy->cur >= dbs_tuners_ins.boost_cpu || this_dbs_info->requested_freq > dbs_tuners_ins.boost_cpu)
			return;

		this_dbs_info->requested_freq = dbs_tuners_ins.boost_cpu;
		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
			CPUFREQ_RELATION_H);
		return;
	}
	
	/*
	 * Every sampling_rate, we check, if current idle time is less
	 * than 20% (default), then we try to increase frequency
	 * Every sampling_rate*sampling_down_factor, we check, if current
	 * idle time is more than 80%, then we try to decrease frequency
	 *
	 * Any frequency increase takes it to the maximum frequency.
	 * Frequency reduction happens at minimum steps of
	 * 5% (default) of maximum frequency
	 */

	/* Get Absolute Load */
	for_each_cpu(j, policy->cpus) {
		struct cpu_dbs_info_s *j_dbs_info;
		cputime64_t cur_wall_time, cur_idle_time;
		unsigned int idle_time, wall_time;

		j_dbs_info = &per_cpu(cs_cpu_dbs_info, j);

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);

		wall_time = (unsigned int)
			(cur_wall_time - j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int)
			(cur_idle_time - j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		if (dbs_tuners_ins.ignore_nice) {
			u64 cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE] -
					 j_dbs_info->prev_cpu_nice;
			/*
			 * Assumption: nice time between sampling periods will
			 * be less than 2^32 jiffies for 32 bit sys
			 */
			cur_nice_jiffies = (unsigned long)
					cputime64_to_jiffies64(cur_nice);

			j_dbs_info->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		load = 100 * (wall_time - idle_time) / wall_time;

		if (load > max_load)
			max_load = load;
	}

	/*
	 * break out if we 'cannot' reduce the speed as the user might
	 * want freq_step to be zero
	 */
	if (dbs_tuners_ins.freq_step_up == 0 || dbs_tuners_ins.freq_step_down == 0)
		return;

	/* Check for frequency increase is greater than hotplug value */
	if (max_load > dbs_tuners_ins.up_threshold_hotplug && policy->cur > dbs_tuners_ins.up_freq_threshold_hotplug ) {
		if (num_online_cpus() < 2 && policy->cur != policy->min)
		{
			Lblock_cycles_online ++;
			if (Lblock_cycles_online > dbs_tuners_ins.block_cycles_online
				&& (dbs_tuners_ins.no_2nd_cpu_screen_off == 0 || (dbs_tuners_ins.no_2nd_cpu_screen_off == 1 && screen_is_on)))
			{
				schedule_work_on(0, &hotplug_online_work);
				Lblock_cycles_online  = 0;
				Lblock_cycles_offline  = 0;
			}
		}
	}

	/* Check for frequency increase */
	if (max_load > dbs_tuners_ins.up_threshold) {
		Lblock_cycles_raise++;
		if ( Lblock_cycles_raise >= dbs_tuners_ins.block_cycles_raise)
		 {
			/* if we are already at full speed then break out early */
			if (this_dbs_info->requested_freq == policy->max){
				Lblock_cycles_raise = 0;
				Lblock_cycles_reduce= 0;
				return;
			}

			freq_target = (dbs_tuners_ins.freq_step_up * policy->max) / 100;

			/* max freq cannot be less than 100. But who knows.... */
			if (unlikely(freq_target == 0))
				freq_target = 5;

			this_dbs_info->requested_freq += freq_target;
			if (  this_dbs_info->requested_freq > policy->max) {
				this_dbs_info->requested_freq = policy->max;
			}
			Lblock_cycles_raise = 0;
			Lblock_cycles_reduce= 0;
			__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
				CPUFREQ_RELATION_H);

		}
		return;
	}

	if (max_load < dbs_tuners_ins.down_threshold_hotplug && !dbs_tuners_ins.disable_hotplugging && policy->cur < dbs_tuners_ins.down_freq_threshold_hotplug) {
		if (num_online_cpus() > 1)
		{
			Lblock_cycles_offline ++;
			if (Lblock_cycles_offline > dbs_tuners_ins.block_cycles_offline)
			{
				schedule_work_on(0, &hotplug_offline_work);
                                Lblock_cycles_online  = 0;
                                Lblock_cycles_offline = 0;
			}
		}
	}
	/*
	 * The optimal frequency is the frequency that is the lowest that
	 * can support the current CPU usage without triggering the up
	 * policy. To be safe, we focus 10 points under the threshold.
	 */
	if (max_load < (dbs_tuners_ins.down_threshold - 10)) {
		Lblock_cycles_reduce++;
		if ( Lblock_cycles_reduce > dbs_tuners_ins.block_cycles_reduce) 
		{
			/*
			 * if we cannot reduce the frequency anymore, break out early
			 */
			if (policy->cur == policy->min) 
			{
				Lblock_cycles_raise = 0;
				Lblock_cycles_reduce= 0;
				return;
			}
			freq_target = (dbs_tuners_ins.freq_step_down * policy->max) / 100;

			this_dbs_info->requested_freq -= freq_target;
			if (this_dbs_info->requested_freq < policy->min)
				this_dbs_info->requested_freq = policy->min;

			Lblock_cycles_raise = 0;
			Lblock_cycles_reduce= 0;
						
			__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
					CPUFREQ_RELATION_H);
		}
		return;
	}
}

void screen_is_on_relay_kt(bool state)
{
	screen_is_on = state;
	if (state == true)
	{
		if (stored_sampling_rate > 0)
			dbs_tuners_ins.sampling_rate = stored_sampling_rate;
	}
	else
	{
		stored_sampling_rate = dbs_tuners_ins.sampling_rate;
		dbs_tuners_ins.sampling_rate = dbs_tuners_ins.sampling_rate_screen_off;
	}
	
}

void boostpulse_relay_kt(void)
{
	if (!boostpulse_relayf)
	{
		if (dbs_tuners_ins.boost_gpu > 0)
		{
			int bpc = (dbs_tuners_ins.boost_hold_cycles / 2);
			if (dbs_tuners_ins.boost_hold_cycles > 0)
				boost_the_gpu(dbs_tuners_ins.boost_gpu, bpc);
			else
				boost_the_gpu(dbs_tuners_ins.boost_gpu, 0);
		}
		if (num_online_cpus() < 2 && dbs_tuners_ins.boost_turn_on_2nd_core)
			schedule_work_on(0, &hotplug_online_work);
		else if (dbs_tuners_ins.boost_turn_on_2nd_core == 0 && dbs_tuners_ins.boost_cpu == 0 && dbs_tuners_ins.boost_gpu == 0)
			return;
	
		boostpulse_relayf = true;
		boost_hold_cycles_cnt = 0;
	}
	else
	{
		if (dbs_tuners_ins.boost_gpu > 0)
		{
			int bpc = (dbs_tuners_ins.boost_hold_cycles / 2);
			if (dbs_tuners_ins.boost_hold_cycles > 0)
				boost_the_gpu(dbs_tuners_ins.boost_gpu, bpc);
			else
				boost_the_gpu(dbs_tuners_ins.boost_gpu, 0);
		}
		boost_hold_cycles_cnt = 0;
	}
}

static void hotplug_offline_work_fn(struct work_struct *work)
{
	int cpu;
	//pr_info("ENTER OFFLINE");
	for_each_online_cpu(cpu) {
		if (likely(cpu_online(cpu) && (cpu))) {
			cpu_down(cpu);
			//pr_info("auto_hotplug: CPU%d down.\n", cpu);
			break;
		}
	}
}

static void hotplug_online_work_fn(struct work_struct *work)
{
	int cpu;
	//pr_info("ENTER ONLINE");
	for_each_possible_cpu(cpu) {
		if (likely(!cpu_online(cpu) && (cpu))) {
			cpu_up(cpu);
			//pr_info("auto_hotplug: CPU%d up.\n", cpu);
			break;
		}
	}
}

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;

	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	delay -= jiffies % delay;

	mutex_lock(&dbs_info->timer_mutex);

	dbs_check_cpu(dbs_info);

	schedule_delayed_work_on(cpu, &dbs_info->work, delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	delay -= jiffies % delay;

	dbs_info->enable = 1;
	INIT_DEFERRABLE_WORK(&dbs_info->work, do_dbs_timer);
	schedule_delayed_work_on(dbs_info->cpu, &dbs_info->work, delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	dbs_info->enable = 0;
	cancel_delayed_work_sync(&dbs_info->work);
}

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int j;
	int rc;

	this_dbs_info = &per_cpu(cs_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		ktoonservative_is_active(true);
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		mutex_lock(&dbs_mutex);

		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(cs_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kcpustat_cpu(j).cpustat[CPUTIME_NICE];
			}
		}
		this_dbs_info->cpu = cpu;
		this_dbs_info->down_skip = 0;
		this_dbs_info->requested_freq = policy->cur;

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_enable++;
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			unsigned int latency;
			/* policy latency is in nS. Convert it to uS first */
			latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;

			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}
			
			dbs_tuners_ins.sampling_rate = 45000;
				//max((min_sampling_rate * 20),
				    //latency * LATENCY_MULTIPLIER);

			cpufreq_register_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
		}
		mutex_unlock(&dbs_mutex);

		dbs_timer_init(this_dbs_info);

		break;

	case CPUFREQ_GOV_STOP:
		ktoonservative_is_active(false);
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
		dbs_enable--;
		mutex_destroy(&this_dbs_info->timer_mutex);

		/*
		 * Stop the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 0)
			cpufreq_unregister_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);

		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);

		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->min, CPUFREQ_RELATION_L);
		dbs_check_cpu(this_dbs_info);
		mutex_unlock(&this_dbs_info->timer_mutex);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_KTOONSERVATIVE
static
#endif
struct cpufreq_governor cpufreq_gov_ktoonservative = {
	.name			= "ktoonservative",
	.governor		= cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	INIT_WORK(&hotplug_offline_work, hotplug_offline_work_fn);
	INIT_WORK(&hotplug_online_work, hotplug_online_work_fn);
	
	return cpufreq_register_governor(&cpufreq_gov_ktoonservative);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_ktoonservative);
}


MODULE_AUTHOR("Alexander Clouter <alex@digriz.org.uk>");
MODULE_DESCRIPTION("'cpufreq_ktoonservative' - A dynamic cpufreq governor for "
		"Low Latency Frequency Transition capable processors "
		"optimised for use in a battery environment");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_KTOONSERVATIVE
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
