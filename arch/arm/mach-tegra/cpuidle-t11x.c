/*
 * arch/arm/mach-tegra/cpuidle-t11x.c
 *
 * CPU idle driver for Tegra11x CPUs
 *
 * Copyright (c) 2012, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/suspend.h>
#include <linux/tick.h>
#include <linux/clk.h>
#include <linux/cpu_pm.h>
#include <linux/module.h>

#include <asm/cacheflush.h>
#include <asm/hardware/gic.h>
#include <asm/localtimer.h>
#include <asm/suspend.h>
#include <asm/cputype.h>

#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/hardware.h>

#include <trace/events/power.h>

#include "clock.h"
#include "cpuidle.h"
#include "dvfs.h"
#include "fuse.h"
#include "gic.h"
#include "pm.h"
#include "reset.h"
#include "sleep.h"
#include "timer.h"
#include "fuse.h"

#define CLK_RST_CONTROLLER_CPU_CMPLX_STATUS \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x470)
#define PMC_POWERGATE_STATUS \
	(IO_ADDRESS(TEGRA_PMC_BASE) + 0x038)

#define ARCH_TIMER_CTRL_ENABLE          (1 << 0)
#define ARCH_TIMER_CTRL_IT_MASK         (1 << 1)

#ifdef CONFIG_SMP
static s64 tegra_cpu_wake_by_time[4] = {
	LLONG_MAX, LLONG_MAX, LLONG_MAX, LLONG_MAX };
#endif

static ulong cpu_power_gating_in_idle __read_mostly = 0x1f;
module_param(cpu_power_gating_in_idle, ulong, 0644);

static bool slow_cluster_power_gating_noncpu __read_mostly;
module_param(slow_cluster_power_gating_noncpu, bool, 0644);

static uint fast_cluster_power_down_mode __read_mostly;
module_param(fast_cluster_power_down_mode, uint, 0644);

static struct clk *cpu_clk_for_dvfs;

static int lp2_exit_latencies[5];

static struct {
	unsigned int cpu_ready_count[5];
	unsigned int tear_down_count[5];
	unsigned long long cpu_wants_lp2_time[5];
	unsigned long long in_lp2_time[5];
	unsigned int lp2_count;
	unsigned int lp2_completed_count;
	unsigned int lp2_count_bin[32];
	unsigned int lp2_completed_count_bin[32];
	unsigned int lp2_int_count[NR_IRQS];
	unsigned int last_lp2_int_count[NR_IRQS];
} idle_stats;

static inline unsigned int time_to_bin(unsigned int time)
{
	return fls(time);
}

static inline void tegra_irq_unmask(int irq)
{
	struct irq_data *data = irq_get_irq_data(irq);
	data->chip->irq_unmask(data);
}

static inline unsigned int cpu_number(unsigned int n)
{
	return is_lp_cluster() ? 4 : n;
}

void tegra11x_cpu_idle_stats_lp2_ready(unsigned int cpu)
{
	idle_stats.cpu_ready_count[cpu_number(cpu)]++;
}

void tegra11x_cpu_idle_stats_lp2_time(unsigned int cpu, s64 us)
{
	idle_stats.cpu_wants_lp2_time[cpu_number(cpu)] += us;
}

/* Allow rail off only if all secondary CPUs are power gated, and no
   rail update is in progress */
static bool tegra_rail_off_is_allowed(void)
{
	u32 rst = readl(CLK_RST_CONTROLLER_CPU_CMPLX_STATUS);
	u32 pg = readl(PMC_POWERGATE_STATUS) >> 8;

	if (((rst & 0xE) != 0xE) || ((pg & 0xE) != 0))
		return false;

	if (tegra_dvfs_rail_updating(cpu_clk_for_dvfs))
		return false;

	return true;
}

bool tegra11x_lp2_is_allowed(struct cpuidle_device *dev,
	struct cpuidle_state *state)
{
	s64 request;

	if (!cpumask_test_cpu(cpu_number(dev->cpu),
				to_cpumask(&cpu_power_gating_in_idle)))
		return false;

	request = ktime_to_us(tick_nohz_get_sleep_length());
	if (state->exit_latency != lp2_exit_latencies[cpu_number(dev->cpu)]) {
		/* possible on the 1st entry after cluster switch*/
		state->exit_latency = lp2_exit_latencies[cpu_number(dev->cpu)];
		tegra_lp2_update_target_residency(state);
	}
	if (request < state->target_residency) {
		/* Not enough time left to enter LP2 */
		return false;
	}

	return true;
}

static inline void tegra11_lp2_restore_affinity(void)
{
#ifdef CONFIG_SMP
	/* Disable the distributor. */
	tegra_gic_dist_disable();

	/* Restore the other CPU's interrupt affinity. */
	tegra_gic_restore_affinity();

	/* Re-enable the distributor. */
	tegra_gic_dist_enable();
#endif
}

static bool tegra_cpu_cluster_power_down(struct cpuidle_device *dev,
			   struct cpuidle_state *state, s64 request)
{
	ktime_t entry_time;
	ktime_t exit_time;
	bool sleep_completed = false;
	bool multi_cpu_entry = false;
	int bin;
	unsigned int flag = 0;
	s64 sleep_time;

	/* LP2 entry time */
	entry_time = ktime_get();

	if (request < state->target_residency) {
		/* Not enough time left to enter LP2 */
		tegra_cpu_wfi();
		return false;
	}

#ifdef CONFIG_SMP
	multi_cpu_entry = !is_lp_cluster() && (num_online_cpus() > 1);
	if (multi_cpu_entry) {
		s64 wake_time;
		unsigned int i;

		/* Disable the distributor -- this is the only way to
		   prevent the other CPUs from responding to interrupts
		   and potentially fiddling with the distributor
		   registers while we're fiddling with them. */
		tegra_gic_dist_disable();

		/* Did an interrupt come in for another CPU before we
		   could disable the distributor? */
		if (!tegra_rail_off_is_allowed()) {
			/* Yes, re-enable the distributor and LP3. */
			tegra_gic_dist_enable();
			tegra_cpu_wfi();
			return false;
		}

		/* LP2 initial targeted wake time */
		wake_time = ktime_to_us(entry_time) + request;

		/* CPU0 must wake up before any of the other CPUs. */
		smp_rmb();
		for (i = 1; i < CONFIG_NR_CPUS; i++)
			wake_time = min_t(s64, wake_time,
				tegra_cpu_wake_by_time[i]);

		/* LP2 actual targeted wake time */
		request = wake_time - ktime_to_us(entry_time);
		BUG_ON(wake_time < 0LL);

		if (request < state->target_residency) {
			/* Not enough time left to enter LP2 */
			tegra_gic_dist_enable();
			tegra_cpu_wfi();
			return false;
		}

		/* Cancel LP2 wake timers for all secondary CPUs */
		tegra_lp2_timer_cancel_secondary();

		/* Save and disable the affinity setting for the other
		   CPUs and route all interrupts to CPU0. */
		tegra_gic_disable_affinity();

		/* Re-enable the distributor. */
		tegra_gic_dist_enable();
	}
#endif
	cpu_pm_enter();

	sleep_time = request -
		lp2_exit_latencies[cpu_number(dev->cpu)];

	bin = time_to_bin((u32)request / 1000);
	idle_stats.tear_down_count[cpu_number(dev->cpu)]++;
	idle_stats.lp2_count++;
	idle_stats.lp2_count_bin[bin]++;

	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_ENTER, &dev->cpu);
	if (is_lp_cluster()) {
		/* here we are not supporting emulation mode, for now */
		flag = TEGRA_POWER_CLUSTER_PART_NONCPU;
	} else {
		tegra_dvfs_rail_off(tegra_cpu_rail, entry_time);
		flag = (fast_cluster_power_down_mode
			<< TEGRA_POWER_CLUSTER_PART_SHIFT)
			&& TEGRA_POWER_CLUSTER_PART_MASK;
	}

	if (tegra_idle_lp2_last(sleep_time, flag) == 0)
		sleep_completed = true;
	else {
		int irq = tegra_gic_pending_interrupt();
		idle_stats.lp2_int_count[irq]++;
	}

	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_EXIT, &dev->cpu);
	exit_time = ktime_get();
	if (!is_lp_cluster())
		tegra_dvfs_rail_on(tegra_cpu_rail, exit_time);

	idle_stats.in_lp2_time[cpu_number(dev->cpu)] +=
		ktime_to_us(ktime_sub(exit_time, entry_time));

	if (multi_cpu_entry)
		tegra11_lp2_restore_affinity();

	if (sleep_completed) {
		/*
		 * Stayed in LP2 for the full time until the next tick,
		 * adjust the exit latency based on measurement
		 */
		int offset = ktime_to_us(ktime_sub(exit_time, entry_time))
			- request;
		int latency = lp2_exit_latencies[cpu_number(dev->cpu)] +
			offset / 16;
		latency = clamp(latency, 0, 10000);
		lp2_exit_latencies[cpu_number(dev->cpu)] = latency;
		state->exit_latency = latency;		/* for idle governor */
		smp_wmb();

		idle_stats.lp2_completed_count++;
		idle_stats.lp2_completed_count_bin[bin]++;

		pr_debug("%lld %lld %d %d\n", request,
			ktime_to_us(ktime_sub(exit_time, entry_time)),
			offset, bin);
	}

	cpu_pm_exit();

	return true;
}

static bool tegra_cpu_core_power_down(struct cpuidle_device *dev,
			   struct cpuidle_state *state, s64 request)
{
#ifdef CONFIG_SMP
	s64 sleep_time;
	ktime_t entry_time;
	struct arch_timer_context timer_context;
	bool sleep_completed = false;
	struct tick_sched *ts = tick_get_tick_sched(dev->cpu);

	if (!arch_timer_get_state(&timer_context)) {
		if ((timer_context.cntp_ctl & ARCH_TIMER_CTRL_ENABLE) &&
		    ~(timer_context.cntp_ctl & ARCH_TIMER_CTRL_IT_MASK)) {
			if (timer_context.cntp_tval <= 0) {
				tegra_cpu_wfi();
				return false;
			}
			request = div_u64((u64)timer_context.cntp_tval *
					1000000, timer_context.cntfrq);
#ifdef CONFIG_TEGRA_LP2_CPU_TIMER
			if (request >= state->target_residency) {
				timer_context.cntp_tval -= state->exit_latency *
					(timer_context.cntfrq / 1000000);
				__asm__("mcr p15, 0, %0, c14, c2, 0\n"
					:
					:
					"r"(timer_context.cntp_tval));
			}
#endif
		}
	}

	if (!tegra_is_lp2_timer_ready(dev->cpu) ||
	    (request < state->target_residency) ||
	    (!ts) || (ts->nohz_mode == NOHZ_MODE_INACTIVE)) {
		/*
		 * Not enough time left to enter LP2, or wake timer not ready
		 */
		tegra_cpu_wfi();
		return false;
	}

	cpu_pm_enter();

#if !defined(CONFIG_TEGRA_LP2_CPU_TIMER)
	sleep_time = request - state->exit_latency;
	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_ENTER, &dev->cpu);
	arch_timer_suspend(&timer_context);
	tegra_lp2_set_trigger(sleep_time);
#endif
	idle_stats.tear_down_count[cpu_number(dev->cpu)]++;

	entry_time = ktime_get();

	/* Save time this CPU must be awakened by. */
	tegra_cpu_wake_by_time[dev->cpu] = ktime_to_us(entry_time) + request;
	smp_wmb();

	cpu_suspend(0, tegra3_sleep_cpu_secondary_finish);

	tegra_cpu_wake_by_time[dev->cpu] = LLONG_MAX;

#ifdef CONFIG_TEGRA_LP2_CPU_TIMER
	if (!arch_timer_get_state(&timer_context))
		sleep_completed = (timer_context.cntp_tval <= 0);
#else
	sleep_completed = !tegra_lp2_timer_remain();
	tegra_lp2_set_trigger(0);
	arch_timer_resume(&timer_context);
	clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_EXIT, &dev->cpu);
#endif
	sleep_time = ktime_to_us(ktime_sub(ktime_get(), entry_time));
	idle_stats.in_lp2_time[cpu_number(dev->cpu)] += sleep_time;
	if (sleep_completed) {
		/*
		 * Stayed in LP2 for the full time until timer expires,
		 * adjust the exit latency based on measurement
		 */
		int offset = sleep_time - request;
		int latency = lp2_exit_latencies[cpu_number(dev->cpu)] +
			offset / 16;
		latency = clamp(latency, 0, 10000);
		lp2_exit_latencies[cpu_number(dev->cpu)] = latency;
		state->exit_latency = latency;		/* for idle governor */
		smp_wmb();
	}
#endif
	cpu_pm_exit();

	return true;
}

bool tegra11x_idle_lp2(struct cpuidle_device *dev,
			   struct cpuidle_state *state)
{
	bool entered_lp2;
	bool cpu_gating_only = false;
	bool power_gating_cpu_only = true;
	s64 request = ktime_to_us(tick_nohz_get_sleep_length());

	tegra_set_cpu_in_lp2(dev->cpu);
	cpu_gating_only = (((fast_cluster_power_down_mode
			<< TEGRA_POWER_CLUSTER_PART_SHIFT)
			&& TEGRA_POWER_CLUSTER_PART_MASK) == 0);

	if (is_lp_cluster()) {
		if (slow_cluster_power_gating_noncpu)
			power_gating_cpu_only = false;
		else
			power_gating_cpu_only = true;
	} else if (!cpu_gating_only &&
		(dev->cpu == 0) &&
		(num_online_cpus() == 1) &&
		tegra_rail_off_is_allowed())
		power_gating_cpu_only = false;
	else
		power_gating_cpu_only = true;

	if (power_gating_cpu_only)
		entered_lp2 = tegra_cpu_core_power_down(dev, state, request);
	else
		entered_lp2 = tegra_cpu_cluster_power_down(dev, state, request);

	tegra_clear_cpu_in_lp2(dev->cpu);

	return entered_lp2;
}

int tegra11x_cpuidle_init_soc(void)
{
	int i;

	cpu_clk_for_dvfs = tegra_get_clock_by_name("cpu_g");

	for (i = 0; i < ARRAY_SIZE(lp2_exit_latencies); i++)
		lp2_exit_latencies[i] = tegra_lp2_exit_latency;

	return 0;
}

#ifdef CONFIG_DEBUG_FS
int tegra11x_lp2_debug_show(struct seq_file *s, void *data)
{
	int bin;
	int i;
	seq_printf(s, "                                    cpu0     cpu1     cpu2     cpu3     cpulp\n");
	seq_printf(s, "-----------------------------------------------------------------------------\n");
	seq_printf(s, "cpu ready:                      %8u %8u %8u %8u %8u\n",
		idle_stats.cpu_ready_count[0],
		idle_stats.cpu_ready_count[1],
		idle_stats.cpu_ready_count[2],
		idle_stats.cpu_ready_count[3],
		idle_stats.cpu_ready_count[4]);
	seq_printf(s, "tear down:                      %8u %8u %8u %8u %8u\n",
		idle_stats.tear_down_count[0],
		idle_stats.tear_down_count[1],
		idle_stats.tear_down_count[2],
		idle_stats.tear_down_count[3],
		idle_stats.tear_down_count[4]);
	seq_printf(s, "lp2:            %8u\n", idle_stats.lp2_count);
	seq_printf(s, "lp2 completed:  %8u %7u%%\n",
		idle_stats.lp2_completed_count,
		idle_stats.lp2_completed_count * 100 /
			(idle_stats.lp2_count ?: 1));

	seq_printf(s, "\n");
	seq_printf(s, "cpu ready time:                 %8llu %8llu %8llu %8llu %8llu ms\n",
		div64_u64(idle_stats.cpu_wants_lp2_time[0], 1000),
		div64_u64(idle_stats.cpu_wants_lp2_time[1], 1000),
		div64_u64(idle_stats.cpu_wants_lp2_time[2], 1000),
		div64_u64(idle_stats.cpu_wants_lp2_time[3], 1000),
		div64_u64(idle_stats.cpu_wants_lp2_time[4], 1000));

	seq_printf(s, "lp2 time:                       %8llu %8llu %8llu %8llu %8llu ms\n",
		div64_u64(idle_stats.in_lp2_time[0], 1000),
		div64_u64(idle_stats.in_lp2_time[1], 1000),
		div64_u64(idle_stats.in_lp2_time[2], 1000),
		div64_u64(idle_stats.in_lp2_time[3], 1000),
		div64_u64(idle_stats.in_lp2_time[4], 1000));

	seq_printf(s, "lp2 %%:                         %7d%% %7d%% %7d%% %7d%% %7d%%\n",
		(int)(idle_stats.cpu_wants_lp2_time[0] ?
			div64_u64(idle_stats.in_lp2_time[0] * 100,
			idle_stats.cpu_wants_lp2_time[0]) : 0),
		(int)(idle_stats.cpu_wants_lp2_time[1] ?
			div64_u64(idle_stats.in_lp2_time[1] * 100,
			idle_stats.cpu_wants_lp2_time[1]) : 0),
		(int)(idle_stats.cpu_wants_lp2_time[2] ?
			div64_u64(idle_stats.in_lp2_time[2] * 100,
			idle_stats.cpu_wants_lp2_time[2]) : 0),
		(int)(idle_stats.cpu_wants_lp2_time[3] ?
			div64_u64(idle_stats.in_lp2_time[3] * 100,
			idle_stats.cpu_wants_lp2_time[3]) : 0),
		(int)(idle_stats.cpu_wants_lp2_time[4] ?
			div64_u64(idle_stats.in_lp2_time[4] * 100,
			idle_stats.cpu_wants_lp2_time[4]) : 0));
	seq_printf(s, "\n");

	seq_printf(s, "%19s %8s %8s %8s\n", "", "lp2", "comp", "%");
	seq_printf(s, "-------------------------------------------------\n");
	for (bin = 0; bin < 32; bin++) {
		if (idle_stats.lp2_count_bin[bin] == 0)
			continue;
		seq_printf(s, "%6u - %6u ms: %8u %8u %7u%%\n",
			1 << (bin - 1), 1 << bin,
			idle_stats.lp2_count_bin[bin],
			idle_stats.lp2_completed_count_bin[bin],
			idle_stats.lp2_completed_count_bin[bin] * 100 /
				idle_stats.lp2_count_bin[bin]);
	}

	seq_printf(s, "\n");
	seq_printf(s, "%3s %20s %6s %10s\n",
		"int", "name", "count", "last count");
	seq_printf(s, "--------------------------------------------\n");
	for (i = 0; i < NR_IRQS; i++) {
		if (idle_stats.lp2_int_count[i] == 0)
			continue;
		seq_printf(s, "%3d %20s %6d %10d\n",
			i, irq_to_desc(i)->action ?
				irq_to_desc(i)->action->name ?: "???" : "???",
			idle_stats.lp2_int_count[i],
			idle_stats.lp2_int_count[i] -
				idle_stats.last_lp2_int_count[i]);
		idle_stats.last_lp2_int_count[i] = idle_stats.lp2_int_count[i];
	};
	return 0;
}
#endif