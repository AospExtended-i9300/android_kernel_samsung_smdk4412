/*
 *  drivers/cpufreq/cpufreq_dynamic.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *            (C)  2011 Samsung Electronics co. ltd
 *                      ByungChang Cha <bc.cha@samsung.com>
 *            (C)  2014-2015 Marcin Kaluza (mkaluza@xda-developers.com) <marcin.kaluza@trioptimum.com>
 *            (C)  2018 Victor Shilin (ChronoMonochrome@xda-developers.com) <chrono.monochrome@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/ratelimit.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/reboot.h>

/*
 * runqueue average
 */

#define RQ_AVG_TIMER_RATE	10

struct runqueue_data {
	unsigned int nr_run_avg;
	unsigned int update_rate;
	int64_t last_time;
	int64_t total_time;
	struct delayed_work work;
	struct workqueue_struct *nr_run_wq;
	spinlock_t lock;
};

static struct runqueue_data *rq_data;
static void rq_work_fn(struct work_struct *work);

static void start_rq_work(void)
{
	rq_data->nr_run_avg = 0;
	rq_data->last_time = 0;
	rq_data->total_time = 0;
	if (rq_data->nr_run_wq == NULL)
		rq_data->nr_run_wq =
			create_singlethread_workqueue("nr_run_avg");

	queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
			   msecs_to_jiffies(rq_data->update_rate));
	return;
}

static void stop_rq_work(void)
{
	if (rq_data->nr_run_wq)
		cancel_delayed_work(&rq_data->work);
	return;
}

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct runqueue_data), GFP_KERNEL);
	if (rq_data == NULL) {
		pr_err("%s cannot allocate memory\n", __func__);
		return -ENOMEM;
	}
	spin_lock_init(&rq_data->lock);
	rq_data->update_rate = RQ_AVG_TIMER_RATE;
	INIT_DELAYED_WORK_DEFERRABLE(&rq_data->work, rq_work_fn);

	return 0;
}

static void rq_work_fn(struct work_struct *work)
{
	int64_t time_diff = 0;
	int64_t nr_run = 0;
	unsigned long flags = 0;
	int64_t cur_time = ktime_to_ns(ktime_get());

	spin_lock_irqsave(&rq_data->lock, flags);

	if (rq_data->last_time == 0)
		rq_data->last_time = cur_time;
	if (rq_data->nr_run_avg == 0)
		rq_data->total_time = 0;

	nr_run = nr_running() * 100;
	time_diff = cur_time - rq_data->last_time;
	do_div(time_diff, 1000 * 1000);

	if (time_diff != 0 && rq_data->total_time != 0) {
		nr_run = (nr_run * time_diff) +
			(rq_data->nr_run_avg * rq_data->total_time);
		do_div(nr_run, rq_data->total_time + time_diff);
	}
	rq_data->nr_run_avg = nr_run;
	rq_data->total_time += time_diff;
	rq_data->last_time = cur_time;

	if (rq_data->update_rate != 0)
		queue_delayed_work(rq_data->nr_run_wq, &rq_data->work,
				   msecs_to_jiffies(rq_data->update_rate));

	spin_unlock_irqrestore(&rq_data->lock, flags);
}

unsigned int get_nr_run_avg(void)
{
	unsigned int nr_run_avg;
	unsigned long flags = 0;

	spin_lock_irqsave(&rq_data->lock, flags);
	nr_run_avg = rq_data->nr_run_avg;
	rq_data->nr_run_avg = 0;
	spin_unlock_irqrestore(&rq_data->lock, flags);

	return nr_run_avg;
}


/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

#define DEF_FREQUENCY_UP_THRESHOLD		(85)
#define DEF_DOWN_DIFFERENTIAL		(5)

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

// min_sampling_rate is in usecs, all other rates are in jiffies
static unsigned int min_sampling_rate;

#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)
#define MICRO_FREQUENCY_UP_THRESHOLD		(95)
#define MICRO_FREQUENCY_DOWN_DIFFERENTIAL		(10)
#define DEF_SAMPLING_DOWN_FACTOR		(2)
#define MAX_SAMPLING_DOWN_FACTOR		(10)
#define DEF_SAMPLING_UP_FACTOR		(6)
#define MAX_SAMPLING_UP_FACTOR		(20)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)


#define DEF_MAX_CPU_LOCK			(0)
#define DEF_MIN_CPU_LOCK			(4)
#define DEF_CPU_UP_FREQ				(500000)
#define DEF_CPU_DOWN_FREQ			(200000)
#define DEF_UP_NR_CPUS				(1)
#define DEF_CPU_UP_RATE				(10)
#define DEF_CPU_DOWN_RATE			(20)
#define MAX_HOTPLUG_RATE			(40u)
#define DEF_FREQ_STEP				(37)
#define DEF_START_DELAY				(0)

#define DEF_UP_THRESHOLD_AT_MIN_FREQ		(40)
#define DEF_FREQ_FOR_RESPONSIVENESS		(500000)

#define HOTPLUG_DOWN_INDEX			(0)
#define HOTPLUG_UP_INDEX			(1)

#if defined(CONFIG_MACH_MIDAS)
static int _hotplug_rq[4][2] = {
	{0, 100}, {100, 200}, {200, 300}, {300, 0}
};

static int _hotplug_freq[4][2] = {
	{0, 500000},
	{200000, 500000},
	{200000, 500000},
	{200000, 0}
};
#elif defined(CONFIG_MACH_SMDK4210)
static int _hotplug_rq[2][2] = {
	{0, 100}, {100, 0}
};

static int _hotplug_freq[2][2] = {
	{0, 500000},
	{200000, 0}
};
#else
static int _hotplug_rq[4][2] = {
	{0, 100}, {100, 200}, {200, 300}, {300, 0}
};

static int _hotplug_freq[4][2] = {
	{0, 500000},
	{200000, 500000},
	{200000, 500000},
	{200000, 0}
};
#endif

enum ignore_nice_enum {
	IGNORE_NICE_SUSPEND,
	IGNORE_NICE_STANDBY,
	IGNORE_NICE_ALWAYS
};

static void do_dbs_timer(struct work_struct *work);

static void dbs_suspend(void);
static void dbs_resume(void);

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	cputime64_t prev_cpu_io;
	struct cpufreq_policy *cur_policy;
	struct cpufreq_frequency_table *freq_table;
	unsigned int freq_lo;
	struct delayed_work work;
	struct work_struct up_work;
	struct work_struct down_work;
	unsigned int down_skip;
	unsigned int requested_freq;
	unsigned int sampling_up_counter;
	unsigned int standby_counter;
	unsigned int down_threshold;
	unsigned int oc_boost_cycles;
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

static bool suspend = false;
module_param(suspend, bool, 0644);
static bool standby  = false;
module_param(standby, bool, 0644);

static inline bool is_boosted(void);
static inline bool is_active(void);

static struct workqueue_struct *dbs_wq;

/* input boost */

static u64 last_input_time = 0;
#define MIN_INPUT_INTERVAL (50 * USEC_PER_MSEC)

/* input boost end */

/*
 * dbs_mutex protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct dbs_tuners {
	unsigned int input_boost_freq;
	unsigned int input_boost_us;
	unsigned int power_optimal_freq;
	unsigned int high_freq_sampling_up_factor;

	unsigned int up_threshold;
	unsigned int down_differential;
	unsigned int ignore_nice;
	unsigned int io_is_busy;

	unsigned int sampling_rate;
	unsigned int sampling_down_factor;
	unsigned int sampling_down_factor_relax_khz;
	unsigned int max_non_oc_freq;
	unsigned int oc_freq_boost_ms;
	unsigned int standby_delay_factor;
	unsigned int standby_threshold_freq;

	unsigned int standby_sampling_rate;
	unsigned int standby_sampling_up_factor;

	unsigned int suspend_sampling_rate;
	unsigned int suspend_sampling_up_factor;
	unsigned int suspend_max_freq;

	unsigned int cpu_up_rate;
	unsigned int cpu_down_rate;
	unsigned int cpu_up_freq;
	unsigned int cpu_down_freq;
	unsigned int up_nr_cpus;
	unsigned int max_cpu_lock;
	unsigned int min_cpu_lock;
	unsigned int dvfs_debug;
	unsigned int max_freq;
	unsigned int min_freq;
	unsigned int boost_mincpus;

//internal
	unsigned int _suspend_max_freq_soft;
	unsigned int _suspend_max_freq_hard;
	unsigned int _standby_max_freq_soft;
	unsigned int _oc_limit;
	unsigned int _standby_threshold_freq;
} dbs_tuners_ins = {
	.input_boost_freq = 200000,
	.input_boost_us = 0*1000,
	.power_optimal_freq = 800000,
	.high_freq_sampling_up_factor = 2,

	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.down_differential = DEF_DOWN_DIFFERENTIAL,
	.ignore_nice = 1,
	.io_is_busy = 10*128/100,
	.standby_delay_factor = 1,
	.standby_threshold_freq = 100000,

	.sampling_rate = 3*HZ/100,
	.sampling_down_factor = 1,
	.sampling_down_factor_relax_khz = 400000,
	.max_non_oc_freq = 900000,
	.oc_freq_boost_ms = 2000,

	.standby_sampling_rate = 2*HZ/100,
	.standby_sampling_up_factor = 5,

	.suspend_sampling_rate = 3*HZ/100,
	.suspend_sampling_up_factor = 7,
	.suspend_max_freq = 600000,

	.cpu_up_rate = DEF_CPU_UP_RATE,
	.cpu_down_rate = DEF_CPU_DOWN_RATE,
	.cpu_up_freq = DEF_CPU_UP_FREQ,
	.cpu_down_freq = DEF_CPU_DOWN_FREQ,
	.up_nr_cpus = DEF_UP_NR_CPUS,
	.max_cpu_lock = DEF_MAX_CPU_LOCK,
	.min_cpu_lock = DEF_MIN_CPU_LOCK,
	.dvfs_debug = 0,
};

void cpufreq_dynamic_min_cpu_lock(unsigned int num_core)
{
	int online, flag;
	struct cpu_dbs_info_s *dbs_info;

	dbs_tuners_ins.min_cpu_lock = min(num_core, num_possible_cpus());

	dbs_info = &per_cpu(cs_cpu_dbs_info, 0); /* from CPU0 */
	online = num_online_cpus();
	flag = (int)num_core - online;
	if (flag <= 0)
		return;
	queue_work_on(dbs_info->cpu, dbs_wq, &dbs_info->up_work);
}

void cpufreq_dynamic_min_cpu_unlock(void)
{
	int online;
	struct cpu_dbs_info_s *dbs_info;

	dbs_tuners_ins.min_cpu_lock = 0;

	dbs_info = &per_cpu(cs_cpu_dbs_info, 0); /* from CPU0 */
	online = num_online_cpus();
	if (suspend) { /* if LCD is off-state */
		return;
	}

	queue_work_on(dbs_info->cpu, dbs_wq, &dbs_info->down_work);
}

/*
 * History of CPU usage
 */
struct cpu_usage {
	unsigned int freq;
	unsigned int load[NR_CPUS];
	unsigned int rq_avg;
};

struct cpu_usage_history {
	struct cpu_usage usage[MAX_HOTPLUG_RATE];
	unsigned int num_hist;
};

static struct cpu_usage_history *hotplug_history;

static struct work_struct suspend_work;
static struct work_struct resume_work;

static unsigned int delay;
module_param(delay, uint, 0644);

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
							cputime64_t *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());
	busy_time = kcpustat_cpu(cpu).cpustat[CPUTIME_USER] +
		    kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];

	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = (cur_wall_time - busy_time);
	if (wall)
		*wall = (cputime64_t)jiffies_to_usecs(cur_wall_time);

	return (cputime64_t)jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall, u64 *iowait)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);
	else if (dbs_tuners_ins.io_is_busy != 1)
		*iowait = get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

static inline void recalculate_down_threshold(struct cpu_dbs_info_s *this_dbs_info)
{
	unsigned int temp = (dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential) * this_dbs_info->freq_lo / this_dbs_info->cur_policy->cur;
	if (temp < 10 || temp > (dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential))
		temp = (dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential)/2;
	this_dbs_info->down_threshold = temp;
}

static inline void recalculate_down_threshold_all(void)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		recalculate_down_threshold(&per_cpu(cs_cpu_dbs_info, cpu));
	}
}

/* keep track of frequency transitions */
static int
dbs_cpufreq_notifier(struct notifier_block *nb, unsigned long event,
		     void *data)
{
	unsigned int idx;
	struct cpufreq_freqs *freq = data;
	struct cpu_dbs_info_s *this_dbs_info = &per_cpu(cs_cpu_dbs_info,
							freq->cpu);

	struct cpufreq_policy *policy;

	if (!this_dbs_info->enable)
		return NOTIFY_DONE;

	policy = this_dbs_info->cur_policy;

	/*
	 * we only care if our internally tracked freq moves outside
	 * the 'valid' ranges of freqency available to us otherwise
	 * we do not change it
	*/
	if (this_dbs_info->requested_freq > policy->max
			|| this_dbs_info->requested_freq < policy->min)
		this_dbs_info->requested_freq = freq->new;
	//TODO recalculate freq dependend values: freq_lo, down_threshold, ...?
	if (freq->new > policy->min) {
		cpufreq_frequency_table_target(policy, this_dbs_info->freq_table, freq->new - 1, CPUFREQ_RELATION_H, &idx);
		this_dbs_info->freq_lo = this_dbs_info->freq_table[idx].frequency;
		recalculate_down_threshold(this_dbs_info);
	} else 
		 this_dbs_info->freq_lo = policy->min;

	return NOTIFY_OK;
}

static struct notifier_block dbs_cpufreq_notifier_block = {
	.notifier_call = dbs_cpufreq_notifier
};

static void recalculate_freq_limits(void) {
		struct cpu_dbs_info_s *dbs_info = &per_cpu(cs_cpu_dbs_info, 0);
		struct cpufreq_policy *policy = dbs_info->cur_policy;

		//find suspend  hard limit
		pr_debug("current limits: _standby_max_freq_soft: %d, _suspend_max_freq_soft: %d, _suspend_max_freq_hard:%d\n", dbs_tuners_ins._standby_max_freq_soft, dbs_tuners_ins._suspend_max_freq_soft, dbs_tuners_ins._suspend_max_freq_hard);
		pr_debug("frequency settings: suspend_max_freq: %d, power_optimal_freq: %d, max_non_oc_freq: %d, policy->max: %d, oc_freq_boost_ms: %d\n", dbs_tuners_ins.suspend_max_freq, dbs_tuners_ins.power_optimal_freq, dbs_tuners_ins.max_non_oc_freq, policy->max, dbs_tuners_ins.oc_freq_boost_ms);
		//TODO can't decide which of the two should be first...
		if (dbs_tuners_ins.max_non_oc_freq && (dbs_tuners_ins.oc_freq_boost_ms == 0 || (dbs_tuners_ins.power_optimal_freq == 0 && dbs_tuners_ins.suspend_max_freq == 0)))
			dbs_tuners_ins._suspend_max_freq_hard = dbs_tuners_ins.max_non_oc_freq;
		else if (dbs_tuners_ins.power_optimal_freq)
			dbs_tuners_ins._suspend_max_freq_hard = dbs_tuners_ins.power_optimal_freq;
		else if (dbs_tuners_ins.suspend_max_freq)
			dbs_tuners_ins._suspend_max_freq_hard = dbs_tuners_ins.suspend_max_freq;
		else
			dbs_tuners_ins._suspend_max_freq_hard = policy->max;

		//find suspend soft limit
		if (dbs_tuners_ins.suspend_max_freq)
			dbs_tuners_ins._suspend_max_freq_soft = dbs_tuners_ins.suspend_max_freq;
		else if (dbs_tuners_ins.power_optimal_freq)
			dbs_tuners_ins._suspend_max_freq_soft = dbs_tuners_ins.power_optimal_freq;
		else
			dbs_tuners_ins._suspend_max_freq_soft = policy->max;

		//calculate standby soft freq limit
		if (dbs_tuners_ins.max_non_oc_freq && ((dbs_tuners_ins.max_non_oc_freq < policy->max && dbs_tuners_ins.oc_freq_boost_ms == 0) || dbs_tuners_ins.power_optimal_freq == 0))
			dbs_tuners_ins._standby_max_freq_soft = dbs_tuners_ins.max_non_oc_freq;
		else if (dbs_tuners_ins.power_optimal_freq)
			dbs_tuners_ins._standby_max_freq_soft = dbs_tuners_ins.power_optimal_freq;
		else
			dbs_tuners_ins._standby_max_freq_soft = policy->max;

		if (dbs_tuners_ins._suspend_max_freq_hard > policy->max)
			dbs_tuners_ins._suspend_max_freq_hard = policy->max;
		if (dbs_tuners_ins._suspend_max_freq_soft > policy->max)
			dbs_tuners_ins._suspend_max_freq_soft = policy->max;
		if (dbs_tuners_ins._standby_max_freq_soft > policy->max)
			dbs_tuners_ins._standby_max_freq_soft = policy->max;

		if (policy->max > dbs_tuners_ins.max_non_oc_freq && dbs_tuners_ins.oc_freq_boost_ms)
			dbs_tuners_ins._oc_limit = dbs_tuners_ins.oc_freq_boost_ms*num_present_cpus()*(policy->max-dbs_tuners_ins.max_non_oc_freq)/1000;
		else
			dbs_tuners_ins._oc_limit = 0;

		dbs_tuners_ins._standby_threshold_freq = policy->min + dbs_tuners_ins.standby_threshold_freq;
		pr_debug("calculated limits: _standby_max_freq_soft: %d, _suspend_max_freq_soft: %d, _suspend_max_freq_hard:%d, _oc_limit: %d\n",
				dbs_tuners_ins._standby_max_freq_soft, dbs_tuners_ins._suspend_max_freq_soft, dbs_tuners_ins._suspend_max_freq_hard, dbs_tuners_ins._oc_limit);
}
/************************** sysfs interface ************************/
static ssize_t show_sampling_rate_min(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", min_sampling_rate);
}

define_one_global_ro(sampling_rate_min);

/* cpufreq_dynamic Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}

#define show_one_conv(file_name, object, conv)				\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	unsigned int value = dbs_tuners_ins.object;			\
	return sprintf(buf, "%u\n", conv);				\
}

#define show_rate(file_name, object)					\
show_one_conv(file_name, object, jiffies_to_usecs(value));

show_rate(sampling_rate, sampling_rate);
show_rate(suspend_sampling_rate, suspend_sampling_rate);
show_rate(standby_sampling_rate, standby_sampling_rate);
show_one(suspend_sampling_up_factor, suspend_sampling_up_factor);
show_one(standby_sampling_up_factor, standby_sampling_up_factor);
show_one(standby_delay_factor, standby_delay_factor);
show_one(sampling_down_factor, sampling_down_factor);
show_one(sampling_down_factor_relax_khz, sampling_down_factor_relax_khz);
show_one(up_threshold, up_threshold);
show_one(down_differential, down_differential);
show_one(ignore_nice_load, ignore_nice);
show_one_conv(io_is_busy, io_is_busy, (value+1)*100/128);

show_one(standby_threshold_freq, standby_threshold_freq);
show_one(input_boost_freq, input_boost_freq);
show_one(input_boost_ms, input_boost_us/1000);

show_one(suspend_max_freq, suspend_max_freq);

show_one(power_optimal_freq, power_optimal_freq);
show_one(high_freq_sampling_up_factor, high_freq_sampling_up_factor);

show_one(max_non_oc_freq, max_non_oc_freq);
show_one(oc_freq_boost_ms, oc_freq_boost_ms);

show_one(cpu_up_rate, cpu_up_rate);
show_one(cpu_down_rate, cpu_down_rate);
show_one(cpu_up_freq, cpu_up_freq);
show_one(cpu_down_freq, cpu_down_freq);
show_one(up_nr_cpus, up_nr_cpus);
show_one(max_cpu_lock, max_cpu_lock);
show_one(min_cpu_lock, min_cpu_lock);
show_one(dvfs_debug, dvfs_debug);
show_one(boost_mincpus, boost_mincpus);

static bool verify_freq(unsigned int *freq) {
	unsigned int idx, ret;
	struct cpu_dbs_info_s *dbs_info = &per_cpu(cs_cpu_dbs_info, 0);
	ret = cpufreq_frequency_table_target(dbs_info->cur_policy, dbs_info->freq_table, *freq, CPUFREQ_RELATION_L, &idx);
	if (ret) return false;
	*freq = dbs_info->freq_table[idx].frequency;
	return true;
}

#define __store_int(file_name, object, condition, conversion, pproc)	\
static ssize_t store_##file_name(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{	\
	unsigned int input;	\
	int ret;	\
	ret = sscanf(buf, "%u", &input);	\
\
	if (ret != 1 || !(condition))	\
		return -EINVAL;	\
\
	dbs_tuners_ins.object = conversion;	\
	pproc;	\
	return count;\
}

#define store_int_cond(file_name, object, condition) __store_int(file_name, object, condition, input, if(0) {};);
#define store_int(file_name, object) __store_int(file_name, object, true, input, if (0){};);
#define store_bounded_int(file_name, object, lo_bound, hi_bound) __store_int(file_name, object, lo_bound <= input && input <= hi_bound, input, if (0){ };);
#define store_int_conv(file_name, object, conversion) __store_int(file_name, object, true, conversion, if(0){ };);
#define store_bounded_int_conv(file_name, object, lo_bound, hi_bound, conv) __store_int(file_name, object, lo_bound <= input && input <= hi_bound, conv, if (0){ };);

store_int(sampling_down_factor_relax_khz, sampling_down_factor_relax_khz);
store_bounded_int(sampling_down_factor, sampling_down_factor, 1, MAX_SAMPLING_DOWN_FACTOR);
store_bounded_int(ignore_nice_load, ignore_nice, 0, IGNORE_NICE_ALWAYS);
__store_int(suspend_max_freq, suspend_max_freq, input==0 || verify_freq(&input), input, recalculate_freq_limits());
store_int_cond(input_boost_freq, input_boost_freq, input==0 || verify_freq(&input));
store_int_conv(input_boost_ms, input_boost_us, input*1000);
store_bounded_int(standby_delay_factor, standby_delay_factor, 1, MAX_SAMPLING_DOWN_FACTOR);
store_bounded_int(standby_sampling_up_factor, standby_sampling_up_factor, 1, MAX_SAMPLING_DOWN_FACTOR);
store_bounded_int(suspend_sampling_up_factor, suspend_sampling_up_factor, 1, MAX_SAMPLING_DOWN_FACTOR);
__store_int(power_optimal_freq, power_optimal_freq, input==0 || verify_freq(&input), input, recalculate_freq_limits());
store_bounded_int(high_freq_sampling_up_factor, high_freq_sampling_up_factor, 1, MAX_SAMPLING_DOWN_FACTOR);

__store_int(standby_threshold_freq, standby_threshold_freq, input>=0, input, recalculate_freq_limits());

__store_int(max_non_oc_freq, max_non_oc_freq, verify_freq(&input), input, recalculate_freq_limits());
__store_int(oc_freq_boost_ms, oc_freq_boost_ms, true, input, recalculate_freq_limits());

__store_int(suspend_sampling_rate, suspend_sampling_rate,
		input >= min_sampling_rate,
		usecs_to_jiffies(max(input, min_sampling_rate)),
		if (suspend) delay = dbs_tuners_ins.suspend_sampling_rate
		);

__store_int(standby_sampling_rate, standby_sampling_rate,
		input >= min_sampling_rate,
		usecs_to_jiffies(max(input, min_sampling_rate)),
		if (standby) delay = dbs_tuners_ins.standby_sampling_rate
		);

__store_int(sampling_rate, sampling_rate,
		input >= min_sampling_rate,
		usecs_to_jiffies(max(input, min_sampling_rate)),
		if (!(standby || suspend)) delay = dbs_tuners_ins.sampling_rate
		);

__store_int(up_threshold, up_threshold,
		dbs_tuners_ins.down_differential < input && input <= 100,
		input,
		recalculate_down_threshold_all()
		);

__store_int(down_differential, down_differential,
		0 < input  && input < dbs_tuners_ins.up_threshold,
		input,
		recalculate_down_threshold_all()
		);

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input, prev = dbs_tuners_ins.io_is_busy;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input >= 100)
		input = 1;

	if (input == dbs_tuners_ins.io_is_busy) /* nothing to do */
		return count;

	dbs_tuners_ins.io_is_busy = input*128/100;

	// if io_is_busy == 1 then we just ignore cpu io time completely
	// otherwise we have to keep track of it
	if (dbs_tuners_ins.io_is_busy == 1) return count;

	// if it has been changed from some other non 1 value, prev_cpu_io values are up to date
	if (prev != 1) return count;

	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(cs_cpu_dbs_info, j);
		dbs_info->prev_cpu_io = get_cpu_iowait_time_us(j, NULL);
	}
	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(suspend_sampling_rate);
define_one_global_rw(standby_sampling_rate);
define_one_global_rw(suspend_sampling_up_factor);
define_one_global_rw(standby_sampling_up_factor);
define_one_global_rw(standby_delay_factor);
define_one_global_rw(standby_threshold_freq);
define_one_global_rw(sampling_down_factor);
define_one_global_rw(sampling_down_factor_relax_khz);
define_one_global_rw(up_threshold);
define_one_global_rw(down_differential);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(io_is_busy);

define_one_global_rw(suspend_max_freq);
define_one_global_rw(input_boost_freq);
define_one_global_rw(input_boost_ms);

define_one_global_rw(power_optimal_freq);
define_one_global_rw(high_freq_sampling_up_factor);

define_one_global_rw(max_non_oc_freq);
define_one_global_rw(oc_freq_boost_ms);

static ssize_t show_cpucore_table(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	ssize_t count = 0;
	int i;
	for (i = CONFIG_NR_CPUS; i > 0; i--) {
		count += sprintf(&buf[count], "%d ", i);
	}
	count += sprintf(&buf[count], "\n");

	return count;
}

#define show_hotplug_param(file_name, num_core, up_down)		\
static ssize_t show_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", file_name[num_core - 1][up_down]);	\
}

#define store_hotplug_param(file_name, num_core, up_down)		\
static ssize_t store_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	file_name[num_core - 1][up_down] = input;			\
	return count;							\
}

show_hotplug_param(_hotplug_freq, 1, 1);
show_hotplug_param(_hotplug_freq, 2, 0);
#if CONFIG_NR_CPUS > 2
show_hotplug_param(_hotplug_freq, 2, 1);
show_hotplug_param(_hotplug_freq, 3, 0);
#endif
#if CONFIG_NR_CPUS > 3
show_hotplug_param(_hotplug_freq, 3, 1);
show_hotplug_param(_hotplug_freq, 4, 0);
#endif

show_hotplug_param(_hotplug_rq, 1, 1);
show_hotplug_param(_hotplug_rq, 2, 0);
#if CONFIG_NR_CPUS > 2
show_hotplug_param(_hotplug_rq, 2, 1);
show_hotplug_param(_hotplug_rq, 3, 0);
#endif
#if CONFIG_NR_CPUS > 3
show_hotplug_param(_hotplug_rq, 3, 1);
show_hotplug_param(_hotplug_rq, 4, 0);
#endif

store_hotplug_param(_hotplug_freq, 1, 1);
store_hotplug_param(_hotplug_freq, 2, 0);
#if CONFIG_NR_CPUS > 2
store_hotplug_param(_hotplug_freq, 2, 1);
store_hotplug_param(_hotplug_freq, 3, 0);
#endif
#if CONFIG_NR_CPUS > 3
store_hotplug_param(_hotplug_freq, 3, 1);
store_hotplug_param(_hotplug_freq, 4, 0);
#endif

store_hotplug_param(_hotplug_rq, 1, 1);
store_hotplug_param(_hotplug_rq, 2, 0);
#if CONFIG_NR_CPUS > 2
store_hotplug_param(_hotplug_rq, 2, 1);
store_hotplug_param(_hotplug_rq, 3, 0);
#endif
#if CONFIG_NR_CPUS > 3
store_hotplug_param(_hotplug_rq, 3, 1);
store_hotplug_param(_hotplug_rq, 4, 0);
#endif

define_one_global_rw(_hotplug_freq_1_1);
define_one_global_rw(_hotplug_freq_2_0);
#if CONFIG_NR_CPUS > 2
define_one_global_rw(_hotplug_freq_2_1);
define_one_global_rw(_hotplug_freq_3_0);
#endif
#if CONFIG_NR_CPUS > 3
define_one_global_rw(_hotplug_freq_3_1);
define_one_global_rw(_hotplug_freq_4_0);
#endif

define_one_global_rw(_hotplug_rq_1_1);
define_one_global_rw(_hotplug_rq_2_0);
#if CONFIG_NR_CPUS > 2
define_one_global_rw(_hotplug_rq_2_1);
define_one_global_rw(_hotplug_rq_3_0);
#endif
#if CONFIG_NR_CPUS > 3
define_one_global_rw(_hotplug_rq_3_1);
define_one_global_rw(_hotplug_rq_4_0);
#endif

static ssize_t store_cpu_up_rate(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.cpu_up_rate = min(input, MAX_HOTPLUG_RATE);
	return count;
}

static ssize_t store_cpu_down_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.cpu_down_rate = min(input, MAX_HOTPLUG_RATE);
	return count;
}

static ssize_t store_cpu_up_freq(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.cpu_up_freq = min(input, dbs_tuners_ins.max_freq);
	return count;
}

static ssize_t store_cpu_down_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.cpu_down_freq = max(input, dbs_tuners_ins.min_freq);
	return count;
}

static ssize_t store_up_nr_cpus(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.up_nr_cpus = min(input, num_possible_cpus());
	return count;
}

static ssize_t store_max_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.max_cpu_lock = min(input, num_possible_cpus());
	return count;
}

static ssize_t store_min_cpu_lock(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	if (input == 0)
		cpufreq_dynamic_min_cpu_unlock();
	else
		cpufreq_dynamic_min_cpu_lock(input);
	return count;
}

static ssize_t store_dvfs_debug(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;
	dbs_tuners_ins.dvfs_debug = input > 0;
	return count;
}

static ssize_t store_boost_mincpus(struct kobject *a, struct attribute *b,
				const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	dbs_tuners_ins.boost_mincpus = min(input, 4u);
	return count;
}

define_one_global_rw(cpu_up_rate);
define_one_global_rw(cpu_down_rate);
define_one_global_rw(cpu_up_freq);
define_one_global_rw(cpu_down_freq);
define_one_global_rw(up_nr_cpus);
define_one_global_rw(max_cpu_lock);
define_one_global_rw(min_cpu_lock);
define_one_global_rw(dvfs_debug);
define_one_global_rw(boost_mincpus);
define_one_global_ro(cpucore_table);

static struct attribute *dbs_attributes[] = {
	&input_boost_freq.attr,
	&input_boost_ms.attr,
	&power_optimal_freq.attr,
	&high_freq_sampling_up_factor.attr,

	&up_threshold.attr,
	&down_differential.attr,
	&ignore_nice_load.attr,
	&io_is_busy.attr,

	&sampling_rate.attr,
	&sampling_down_factor.attr,
	&sampling_down_factor_relax_khz.attr,
	&max_non_oc_freq.attr,
	&oc_freq_boost_ms.attr,
	&standby_delay_factor.attr,
	&standby_threshold_freq.attr,

	&standby_sampling_rate.attr,
	&standby_sampling_up_factor.attr,

	&suspend_sampling_rate.attr,
	&suspend_sampling_up_factor.attr,
	&suspend_max_freq.attr,

	&sampling_rate_min.attr,
	&cpu_up_rate.attr,
	&cpu_down_rate.attr,
	&cpu_up_freq.attr,
	&cpu_down_freq.attr,
	&up_nr_cpus.attr,

	&max_cpu_lock.attr,
	&min_cpu_lock.attr,
	&dvfs_debug.attr,
	&_hotplug_freq_1_1.attr,
	&_hotplug_freq_2_0.attr,
#if CONFIG_NR_CPUS > 2
	&_hotplug_freq_2_1.attr,
	&_hotplug_freq_3_0.attr,
#endif
#if CONFIG_NR_CPUS > 3
	&_hotplug_freq_3_1.attr,
	&_hotplug_freq_4_0.attr,
#endif
	&_hotplug_rq_1_1.attr,
	&_hotplug_rq_2_0.attr,
#if CONFIG_NR_CPUS > 2
	&_hotplug_rq_2_1.attr,
	&_hotplug_rq_3_0.attr,
#endif
#if CONFIG_NR_CPUS > 3
	&_hotplug_rq_3_1.attr,
	&_hotplug_rq_4_0.attr,
#endif
	&cpucore_table.attr,
	&boost_mincpus.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "dynamic",
};

/************************** sysfs end ************************/

static inline bool is_boosted(void)
{
	return (dbs_tuners_ins.input_boost_freq > 0) && 
			(ktime_to_us(ktime_get()) < (last_input_time +
							dbs_tuners_ins.input_boost_us));
}

static inline bool is_active(void)
{
	return !(suspend || standby);
}

static void cpu_up_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_up = dbs_tuners_ins.up_nr_cpus;
	int min_cpu_lock = dbs_tuners_ins.min_cpu_lock;
	int boost_mincpus = dbs_tuners_ins.boost_mincpus;

	if (!standby) {
		nr_up = NR_CPUS - online;
		goto do_up_work;
	}

	if (min_cpu_lock)
		nr_up = min_cpu_lock - online;

	if (is_boosted() && boost_mincpus) {
		nr_up = max(nr_up, boost_mincpus - online);
	}

do_up_work:
	for_each_cpu_not(cpu, cpu_online_mask) {
		if (nr_up-- == 0)
			break;
		if (cpu == 0)
			continue;
		printk(KERN_ERR "CPU_UP %d\n", cpu);
		cpu_up(cpu);
	}
}

static void cpu_down_work(struct work_struct *work)
{
	int cpu;
	int online = num_online_cpus();
	int nr_down = online - 1;

	if (nr_down <= 0)
		return;

	if (is_boosted() && dbs_tuners_ins.boost_mincpus)
		nr_down = min(nr_down, online - (int)dbs_tuners_ins.boost_mincpus);

	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		if (--nr_down <= 0)
			break;
		printk(KERN_ERR "CPU_DOWN %d\n", cpu);
		cpu_down(cpu);
	}
}

/*
 * print hotplug debugging info.
 * which 1 : UP, 0 : DOWN
 */
static void debug_hotplug_check(int which, int rq_avg, int freq,
			 struct cpu_usage *usage)
{
	int cpu;
	printk(KERN_ERR "CHECK %s rq %d.%02d freq %d [", which ? "up" : "down",
	       rq_avg / 100, rq_avg % 100, freq);
	for_each_online_cpu(cpu) {
		printk(KERN_ERR "(%d, %d), ", cpu, usage->load[cpu]);
	}
	printk(KERN_ERR "]\n");
}

static int check_up(void)
{
	int num_hist = hotplug_history->num_hist;
	struct cpu_usage *usage;
	int freq, rq_avg;
	int i;
	int up_rate = dbs_tuners_ins.cpu_up_rate;
	int up_freq, up_rq;
	int min_freq = INT_MAX;
	int min_rq_avg = INT_MAX;
	int online;

	online = num_online_cpus();
	up_freq = _hotplug_freq[online - 1][HOTPLUG_UP_INDEX];
	up_rq = _hotplug_rq[online - 1][HOTPLUG_UP_INDEX];

	if (online == num_possible_cpus())
		return 0;

	if (dbs_tuners_ins.max_cpu_lock != 0
		&& online >= dbs_tuners_ins.max_cpu_lock)
		return 0;

	if (dbs_tuners_ins.min_cpu_lock != 0
		&& online < dbs_tuners_ins.min_cpu_lock)
		return 1;

	if (is_boosted() && dbs_tuners_ins.boost_mincpus != 0
		&& online < dbs_tuners_ins.boost_mincpus)
		return 1;

	if (num_hist == 0 || num_hist % up_rate)
		return 0;

	for (i = num_hist - 1; i >= num_hist - up_rate; --i) {
		usage = &hotplug_history->usage[i];

		freq = usage->freq;
		rq_avg =  usage->rq_avg;

		min_freq = min(min_freq, freq);
		min_rq_avg = min(min_rq_avg, rq_avg);
		if (dbs_tuners_ins.dvfs_debug)
			debug_hotplug_check(1, rq_avg, freq, usage);
	}

	if (min_freq >= up_freq && min_rq_avg > up_rq) {
		printk(KERN_ERR "[HOTPLUG IN] %s %d>=%d && %d>%d\n",
			__func__, min_freq, up_freq, min_rq_avg, up_rq);
		hotplug_history->num_hist = 0;
		return 1;
	}
	return 0;
}

static int check_down(void)
{
	int num_hist = hotplug_history->num_hist;
	struct cpu_usage *usage;
	int freq, rq_avg;
	int i;
	int down_rate = dbs_tuners_ins.cpu_down_rate;
	int down_freq, down_rq;
	int max_freq = 0;
	int max_rq_avg = 0;
	int online;

	online = num_online_cpus();
	down_freq = _hotplug_freq[online - 1][HOTPLUG_DOWN_INDEX];
	down_rq = _hotplug_rq[online - 1][HOTPLUG_DOWN_INDEX];

	/* don't bother trying to turn off cpu if we're not done boosting yet,
	 * but allow turning off cpus above minimum */
	if (is_boosted() && dbs_tuners_ins.boost_mincpus != 0
		&& online <= dbs_tuners_ins.boost_mincpus)
		return 0;

	if (online == 1)
		return 0;

	if (dbs_tuners_ins.max_cpu_lock != 0
		&& online > dbs_tuners_ins.max_cpu_lock)
		return 1;

	if (dbs_tuners_ins.min_cpu_lock != 0
		&& online <= dbs_tuners_ins.min_cpu_lock)
		return 0;

	if (num_hist == 0 || num_hist % down_rate)
		return 0;

	for (i = num_hist - 1; i >= num_hist - down_rate; --i) {
		usage = &hotplug_history->usage[i];

		freq = usage->freq;
		rq_avg =  usage->rq_avg;

		max_freq = max(max_freq, freq);
		max_rq_avg = max(max_rq_avg, rq_avg);

		if (dbs_tuners_ins.dvfs_debug)
			debug_hotplug_check(0, rq_avg, freq, usage);
	}

	if (max_freq <= down_freq && max_rq_avg <= down_rq) {
		printk(KERN_ERR "[HOTPLUG OUT] %s %d<=%d && %d<%d\n",
			__func__, max_freq, down_freq, max_rq_avg, down_rq);
		hotplug_history->num_hist = 0;
		return 1;
	}

	return 0;
}

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	struct cpufreq_policy *policy = this_dbs_info->cur_policy;

	unsigned int load = 0;
	unsigned int max_load = 0;
	unsigned int idx;
	unsigned int min_supporting_freq = 0;
	unsigned int max_freq_hard = policy->max;
	unsigned int max_freq_soft = policy->max;


	bool boosted = is_boosted();
	bool active = is_active();

	unsigned int oc_freq_delta = 0;

	unsigned int j;
	int num_hist = hotplug_history->num_hist;
	int max_hotplug_rate = max(dbs_tuners_ins.cpu_up_rate,
				   dbs_tuners_ins.cpu_down_rate);

	if (num_hist >= MAX_HOTPLUG_RATE) {
		pr_err("%s: prevent reading beyond hotplug_history array!\n", __func__);
		num_hist = 0;
		hotplug_history->num_hist = 0;
	}
	hotplug_history->usage[num_hist].freq = policy->cur;
	hotplug_history->usage[num_hist].rq_avg = get_nr_run_avg();
	++hotplug_history->num_hist;

	if (active && policy->cur > dbs_tuners_ins.max_non_oc_freq && this_dbs_info->oc_boost_cycles) {
		pr_debug("this_dbs_info->oc_boost_cycles = %d", this_dbs_info->oc_boost_cycles);
		oc_freq_delta = (policy->cur - dbs_tuners_ins.max_non_oc_freq)/1000;
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
		cputime64_t cur_wall_time, cur_idle_time, cur_io_time=0;
		unsigned int idle_time, wall_time;

		j_dbs_info = &per_cpu(cs_cpu_dbs_info, j);

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time, &cur_io_time);

		wall_time = (unsigned int) (cur_wall_time -
				j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int) (cur_idle_time -
				j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		if (dbs_tuners_ins.io_is_busy != 1 || active == 0) {
			unsigned int io_time = (unsigned int) (cur_io_time -
					j_dbs_info->prev_cpu_io);
			j_dbs_info->prev_cpu_io = cur_io_time;

			if (dbs_tuners_ins.io_is_busy == 0 || active == 0)
				idle_time += io_time;
			else {
				unsigned int max_busy_io_time = (wall_time*dbs_tuners_ins.io_is_busy) >> 7;
				if (io_time >= max_busy_io_time)
					idle_time += io_time - max_busy_io_time;
			}
		}

		if (
				(active && dbs_tuners_ins.ignore_nice == IGNORE_NICE_ALWAYS)
				|| (standby && dbs_tuners_ins.ignore_nice >= IGNORE_NICE_STANDBY)
				|| (suspend && !boosted)
				) {
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

			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}
		j_dbs_info->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		load = 100 * (wall_time - idle_time) / wall_time;
		hotplug_history->usage[num_hist].load[j] = load;

		if (load > max_load)
			max_load = load;

		if (oc_freq_delta) {
			unsigned int oc_workload = oc_freq_delta*(wall_time - idle_time)/1000;
			if (this_dbs_info->oc_boost_cycles > oc_workload)
				this_dbs_info->oc_boost_cycles -= oc_workload;
			else
				this_dbs_info->oc_boost_cycles = 0;
			pr_debug("this_dbs_info->oc_boost_cycles = %d", this_dbs_info->oc_boost_cycles);
		}
	}

	/* Check for CPU hotplug */
	if (check_up()) {
		queue_work_on(this_dbs_info->cpu, dbs_wq,
			      &this_dbs_info->up_work);
	} else if (check_down()) {
		if (standby || suspend)
			queue_work_on(this_dbs_info->cpu, dbs_wq,
			      &this_dbs_info->down_work);
	}

	if (hotplug_history->num_hist  == max_hotplug_rate)
		hotplug_history->num_hist = 0;

	/* frequency changing logic starts here */

	/* input boost logic
	 */
	if (boosted) {
		unsigned int freq_target;
		if (suspend) {
			if (dbs_tuners_ins.max_non_oc_freq) {
				//TODO optimize this
				//this is to avoid a situation where some process is spinning in the background
				//and a volume key is pressed (or any key that does not cause the screen to turn on)
				//without this time limit it could leave the cpu constantly spinning at max oc freq
				//and with it it'll only spin at max_non_oc_freq, which is a "lesser evil"
				if (dbs_tuners_ins.oc_freq_boost_ms) {
					freq_target = policy->max;
				} else {
					freq_target = dbs_tuners_ins.max_non_oc_freq;
					max_freq_hard = freq_target;
				}
			} else
				freq_target = policy->max;
		} else {
			freq_target = dbs_tuners_ins.input_boost_freq;
		}
		if (policy->cur < freq_target) {
			pr_debug("Boosting freq from %d to %d, dt=%llu us\n", this_dbs_info->requested_freq, freq_target, ktime_to_us(ktime_get())-last_input_time);
			this_dbs_info->requested_freq = freq_target;
			__cpufreq_driver_target(policy, freq_target, CPUFREQ_RELATION_H);
			return;
		}
	} else if (suspend) {
		max_freq_hard = dbs_tuners_ins._suspend_max_freq_hard;
		max_freq_soft = dbs_tuners_ins._suspend_max_freq_soft;
	}

	/* calculate turbo boost limits */

	if (active && dbs_tuners_ins.max_non_oc_freq && dbs_tuners_ins.oc_freq_boost_ms) {
		if (this_dbs_info->oc_boost_cycles == 0)
			max_freq_hard = dbs_tuners_ins.max_non_oc_freq;
		else if (this_dbs_info->oc_boost_cycles < dbs_tuners_ins._oc_limit) {
			max_freq_soft = dbs_tuners_ins.max_non_oc_freq;
		}
		if (this_dbs_info->oc_boost_cycles > 0)
			pr_debug("oc limit: %d (%d), freq_delta: %d, soft: %d, hard: %d",
				this_dbs_info->oc_boost_cycles, dbs_tuners_ins._oc_limit, oc_freq_delta, max_freq_soft, max_freq_hard);
	}

	/* calculate and enforce frequency hard limit */

	if (unlikely(max_freq_hard > policy->max))
		max_freq_hard = policy->max;

	if (this_dbs_info->requested_freq > max_freq_hard) {
		pr_debug("enforcing hard limit %d -> %d\n", policy->cur, max_freq_hard);
		this_dbs_info->requested_freq = max_freq_hard;
		__cpufreq_driver_target(policy, max_freq_hard, CPUFREQ_RELATION_H);
		return;
	}

	/* Check for frequency increase */
	if (max_load > (active ? dbs_tuners_ins.up_threshold : 99)) {
		if (standby) {
			max_freq_soft = dbs_tuners_ins._standby_max_freq_soft;
		}

		if (max_freq_soft > max_freq_hard)
			max_freq_soft = max_freq_hard;

		this_dbs_info->down_skip = 0;

		/* if we are already at full speed then break out early */
		if (this_dbs_info->requested_freq >= max_freq_soft)
			return;

		this_dbs_info->standby_counter = 0;

		/* frequency increase delays */
		if (suspend) {
			if (++(this_dbs_info->sampling_up_counter) < dbs_tuners_ins.suspend_sampling_up_factor)
				return;
		} else if (standby) {
			if (++(this_dbs_info->sampling_up_counter) < dbs_tuners_ins.standby_sampling_up_factor)
				return;
		} else if (dbs_tuners_ins.power_optimal_freq && policy->cur >= dbs_tuners_ins.power_optimal_freq) {
			//if we're at or above optimal freq, then delay freq increase by high_freq_sampling_up_factor
			if (++(this_dbs_info->sampling_up_counter) < dbs_tuners_ins.high_freq_sampling_up_factor)
				return;
		}

		this_dbs_info->sampling_up_counter = 0;

		cpufreq_frequency_table_target(policy, this_dbs_info->freq_table, policy->cur + 1, CPUFREQ_RELATION_L, &idx);
		this_dbs_info->requested_freq = this_dbs_info->freq_table[idx].frequency;

		if (this_dbs_info->requested_freq > policy->max)
			this_dbs_info->requested_freq = policy->max;

		pr_debug("freq increase %d -> %d", policy->cur, this_dbs_info->requested_freq);
		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
			CPUFREQ_RELATION_H);
		return;
	}


	/* if the load fell below up_threshold, reset frequency increase delay counter */
	this_dbs_info->sampling_up_counter = 0;

	/* standby mode activation logic */
	if (policy->cur <= dbs_tuners_ins._standby_threshold_freq) {
		if (active && !boosted) {
			if (++(this_dbs_info->standby_counter) >= dbs_tuners_ins.standby_delay_factor) {
				standby = true;
				pr_debug("Entering standby. dt=%lu ms", (unsigned long int)(ktime_to_us(ktime_get())-last_input_time)/1000);
				this_dbs_info->oc_boost_cycles = 0;
			}

			//TODO move all state management code to functions and just call go_active, go_suspend etc...
			delay = dbs_tuners_ins.standby_sampling_rate;
		}
		/* if we cannot reduce the frequency anymore, break out early */
		if (policy->cur == policy->min) return;
	}
	/*
	 * The optimal frequency is the frequency that is the lowest that
	 * can support the current CPU usage without triggering the up
	 * policy.
	 */
	/* Check for frequency decrease */

	if (max_load < this_dbs_info->down_threshold && (!boosted || policy->cur > dbs_tuners_ins.input_boost_freq)) {
		//calculate minimum freq that can support current workload (load_pct*cur_freq) with load < up_threshold-down_diff
		min_supporting_freq = (this_dbs_info->requested_freq*max_load)/(dbs_tuners_ins.up_threshold - dbs_tuners_ins.down_differential);
		cpufreq_frequency_table_target(policy, this_dbs_info->freq_table, min_supporting_freq, CPUFREQ_RELATION_L, &idx);
		min_supporting_freq = this_dbs_info->freq_table[idx].frequency;


		if (active) {
			if (++(this_dbs_info->down_skip) < dbs_tuners_ins.sampling_down_factor) {
				//if the frequency that can support current load
				//is at least sampling_down_factor_relax_khz
				//smaller than current freq then try decreasing freq by one step
				//despite sampling_down_factor timer ticks haven't passed yet
				if (!dbs_tuners_ins.sampling_down_factor_relax_khz || this_dbs_info->freq_lo < policy->min + dbs_tuners_ins.sampling_down_factor_relax_khz)
					return;

				cpufreq_frequency_table_target(policy, this_dbs_info->freq_table, this_dbs_info->freq_lo - dbs_tuners_ins.sampling_down_factor_relax_khz, CPUFREQ_RELATION_L, &idx);
				if (min_supporting_freq > this_dbs_info->freq_table[idx].frequency)
					return;
			}
			this_dbs_info->requested_freq = this_dbs_info->freq_lo;
		} else {
			//Go directly to the lowest frequency that can support current load
			this_dbs_info->requested_freq = min_supporting_freq;
		}

		if (this_dbs_info->requested_freq < policy->min)
			this_dbs_info->requested_freq = policy->min;

		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
				CPUFREQ_RELATION_L);
	}
	this_dbs_info->down_skip = 0;
}

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;

	mutex_lock(&dbs_info->timer_mutex);

	dbs_check_cpu(dbs_info);

	/* We want all CPUs to do sampling nearly on same jiffy */
	queue_delayed_work_on(cpu, dbs_wq, &dbs_info->work, delay - jiffies % delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	delay = dbs_tuners_ins.sampling_rate;

	dbs_info->enable = 1;
	dbs_info->down_skip = 0;
	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	INIT_WORK(&dbs_info->up_work, cpu_up_work);
	INIT_WORK(&dbs_info->down_work, cpu_down_work);

	/* We want all CPUs to do sampling nearly on same jiffy */
	queue_delayed_work_on(dbs_info->cpu, dbs_wq, &dbs_info->work, delay - jiffies % delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	dbs_info->enable = 0;
	cancel_delayed_work_sync(&dbs_info->work);
}

static int pm_notifier_call(struct notifier_block *this,
			    unsigned long event, void *ptr)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		dbs_suspend();
		pr_debug("%s enter suspend\n", __func__);
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		dbs_resume();
		pr_debug("%s exit suspend\n", __func__);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static struct notifier_block pm_notifier = {
	.notifier_call = pm_notifier_call,
};

static int reboot_notifier_call(struct notifier_block *this,
				unsigned long code, void *_cmd)
{
	return NOTIFY_DONE;
}

static struct notifier_block reboot_notifier = {
	.notifier_call = reboot_notifier_call,
};

/* early_suspend */
static void cpufreq_dynamic_resume(struct work_struct *work)
{
	//unsigned int cpu;
	struct cpu_dbs_info_s *this_dbs_info = &per_cpu(cs_cpu_dbs_info, 0);
	struct cpufreq_policy *policy = this_dbs_info->cur_policy;
	unsigned int cpu;

	suspend = false;
	standby = false;
	delay = dbs_tuners_ins.sampling_rate;

	pr_debug("Early resume. dt=%lu ms", (unsigned long int)(ktime_to_us(ktime_get())-last_input_time)/1000);

	//just a little cheat... :)
	//getting here after pressing power button takes 50-100ms... by this time input boost can as well be over...
	last_input_time = ktime_to_us(ktime_get());

	//set max freq
	__cpufreq_driver_target(
			policy,
			policy->max, CPUFREQ_RELATION_H);

	for_each_online_cpu(cpu) {
		this_dbs_info = &per_cpu(cs_cpu_dbs_info, cpu);
		this_dbs_info->requested_freq = policy->max;
	}

	start_rq_work();
}


static void cpufreq_dynamic_suspend(struct work_struct *work)
{
	stop_rq_work();
}


static void dbs_suspend(void)
{
	schedule_work(&suspend_work);
	suspend = true;
	delay = dbs_tuners_ins.suspend_sampling_rate;
}

static void dbs_resume(void)
{
	queue_work(dbs_wq, &resume_work);
}

static void hotplug_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;
	struct cpu_dbs_info_s *dbs_info = &per_cpu(cs_cpu_dbs_info, 0);
	struct cpufreq_policy *policy = dbs_info->cur_policy;

	standby = false;
	delay = dbs_tuners_ins.sampling_rate;

	now = ktime_to_us(ktime_get());
	pr_debug("Input detected at %llu", now);

	if (dbs_tuners_ins._oc_limit)
		dbs_info->oc_boost_cycles = dbs_tuners_ins._oc_limit*2;

	if (now - last_input_time < dbs_tuners_ins.input_boost_us || policy->cur >= dbs_tuners_ins.input_boost_freq) {
		//if input events occur, keep the boost running, just don't flush delayed work
		pr_debug(" - boost trigger not needed: dt=%llu us, freq=%d MHz\n", now - last_input_time, policy->cur/1000);
		last_input_time = now;
		return;
	}

	pr_debug(" - triggering boost\n");
	last_input_time = now;

	if (__cancel_delayed_work(&dbs_info->work) > 0) {
		queue_work_on(dbs_info->cpu, dbs_wq, &dbs_info->work.work);
	}

}

static int hotplug_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void hotplug_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id hotplug_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler hotplug_input_handler = {
	.event          = hotplug_input_event,
	.connect        = hotplug_input_connect,
	.disconnect     = hotplug_input_disconnect,
	.name           = "cpufreq_dynamic",
	.id_table       = hotplug_ids,
};
/* input boost */

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
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;
		
		dbs_tuners_ins.max_freq = policy->max;
		dbs_tuners_ins.min_freq = policy->min;
		
		hotplug_history->num_hist = 0;
		start_rq_work();

		mutex_lock(&dbs_mutex);

		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(cs_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall, &j_dbs_info->prev_cpu_io);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kcpustat_cpu(j).cpustat[CPUTIME_NICE];
			}
			recalculate_down_threshold(j_dbs_info);
		}

		this_dbs_info->freq_table = cpufreq_frequency_get_table(cpu);
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

			/* Bring kernel and HW constraints together */
			min_sampling_rate = max(min_sampling_rate,
					MIN_LATENCY_MULTIPLIER * latency);
			/*
			dbs_tuners_ins.sampling_rate =
				usecs_to_jiffies(max(min_sampling_rate,
				    latency * LATENCY_MULTIPLIER));
			*/
			dbs_tuners_ins.sampling_rate = max(dbs_tuners_ins.sampling_rate, (unsigned int) usecs_to_jiffies(min_sampling_rate));
			dbs_tuners_ins.standby_sampling_rate = max(dbs_tuners_ins.standby_sampling_rate, dbs_tuners_ins.sampling_rate);
			dbs_tuners_ins.suspend_sampling_rate = max(dbs_tuners_ins.suspend_sampling_rate, dbs_tuners_ins.sampling_rate);
			recalculate_freq_limits();

			cpufreq_register_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);

			rc = input_register_handler(&hotplug_input_handler);
			if (rc)
				pr_err("Cannot register hotplug input handler.\n");
		}
		mutex_unlock(&dbs_mutex);

		register_reboot_notifier(&reboot_notifier);

		dbs_timer_init(this_dbs_info);
		register_pm_notifier(&pm_notifier);

		break;

	case CPUFREQ_GOV_STOP:
		unregister_pm_notifier(&pm_notifier);
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);

		unregister_reboot_notifier(&reboot_notifier);

		dbs_enable--;
		mutex_destroy(&this_dbs_info->timer_mutex);

		stop_rq_work();

		/*
		 * Stop the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 0) {
			cpufreq_unregister_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
			input_unregister_handler(&hotplug_input_handler);
		}

		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);

		break;

	case CPUFREQ_GOV_LIMITS:
		pr_debug("dynamic - gov limits %d %d %d\n", policy->min, this_dbs_info->cur_policy->cur, policy->max);
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur) {
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->max, CPUFREQ_RELATION_H);
			this_dbs_info->requested_freq = policy->max;
		} else if (policy->min > this_dbs_info->cur_policy->cur) {
			__cpufreq_driver_target(
					this_dbs_info->cur_policy,
					policy->min, CPUFREQ_RELATION_L);
			this_dbs_info->requested_freq = policy->min;
		}
		recalculate_freq_limits();
		mutex_unlock(&this_dbs_info->timer_mutex);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_DYNAMIC
static
#endif
struct cpufreq_governor cpufreq_gov_dynamic = {
	.name			= "dynamic",
	.governor		= cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	u64 idle_time;
	int ret;
	int cpu = get_cpu();

	ret = init_rq_avg();
	if (ret)
		return ret;

	hotplug_history = kzalloc(sizeof(struct cpu_usage_history), GFP_KERNEL);
	if (!hotplug_history) {
		pr_err("%s cannot create hotplug history array\n", __func__);
		ret = -ENOMEM;
		goto err_hist;
	}

	idle_time = get_cpu_idle_time_us(cpu, NULL);
	put_cpu();
	if (idle_time != -1ULL) {
		/* Idle micro accounting is supported. Use finer thresholds */
		dbs_tuners_ins.up_threshold = MICRO_FREQUENCY_UP_THRESHOLD;
		dbs_tuners_ins.down_differential = MICRO_FREQUENCY_DOWN_DIFFERENTIAL;
		/*
		 * In nohz/micro accounting case we set the minimum frequency
		 * not depending on HZ, but fixed (very low). The deferred
		 * timer might skip some samples if idle/sleeping as needed.
		*/
		min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
	} else {
		/* For correct statistics, we need 10 ticks for each measure */
		min_sampling_rate =
			MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(10);
	}

	INIT_WORK(&resume_work, cpufreq_dynamic_resume);
	INIT_WORK(&suspend_work, cpufreq_dynamic_suspend);
	dbs_wq = alloc_workqueue("dynamic_dbs_wq", WQ_HIGHPRI, 0);
	if (!dbs_wq) {
		printk(KERN_ERR "Failed to create dynamic_dbs_wq workqueue\n");
		return -EFAULT;
	}

	ret = cpufreq_register_governor(&cpufreq_gov_dynamic);
	if (ret)
		goto err_reg;
	return ret;
	
err_reg:
	kfree(hotplug_history);
err_hist:
	kfree(rq_data);
	return ret;
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_dynamic);
	destroy_workqueue(dbs_wq);
	kfree(hotplug_history);
	kfree(rq_data);
}


MODULE_AUTHOR("Marcin Kaluza <mk@flex.pm>");
MODULE_DESCRIPTION("'cpufreq_dynamic' - A dynamic cpufreq governor for "
		"Low Latency Frequency Transition capable processors "
		"optimised for use in a battery environment");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_DYNAMIC
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
