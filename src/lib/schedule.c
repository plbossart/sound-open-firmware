/*
 * Copyright (c) 2017, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Intel Corporation nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <reef/reef.h>
#include <reef/lock.h>
#include <reef/list.h>
#include <reef/stream.h>
#include <reef/alloc.h>
#include <reef/debug.h>
#include <reef/clock.h>
#include <reef/schedule.h>
#include <reef/work.h>
#include <platform/timer.h>
#include <platform/clk.h>
#include <reef/audio/component.h>
#include <reef/audio/pipeline.h>
#include <arch/task.h>

struct schedule_data {
	spinlock_t lock;
	struct list_item list;	/* list of tasks in priority queue */
	uint32_t clock;
	struct work work;
};

static struct schedule_data *sch;

#define SLOT_ALIGN_TRIES	10

/*
 * Simple rescheduler to calculate tasks new start time and deadline if
 * prevoius deadline was missed. Tries to align at first with current task
 * timing, but will just add onto current if too far behind current.
 * XRUNs will be propagated upto the host if we have to reschedule.
 */
static inline void edf_reschedule(struct task *task, uint64_t current)
{
	uint64_t delta = (task->deadline - task->start) << 1;
	int i;

	/* try and align task with current scheduling slots */
	for (i = 0; i < SLOT_ALIGN_TRIES; i++) {

		task->start += delta;

		if (task->start > current + delta) {
			task->deadline = task->start + delta;
			return;
		}
	}

	/* task has slipped a lot, so just add delay to current */
	task->start = current + delta;
	task->deadline = task->start + delta;
}

/*
 * Find the first non running task with the earliest deadline.
 * TODO: Reduce cache invalidations by checking if the currently
 * running task AND the earliest queued task will both complete before their
 * deadlines. If so, then schedule the earlier queued task after the currently
 * running task has completed.
 */
static inline struct task *edf_get_next(uint64_t current,
	struct task *ignore)
{
	struct task *task, *next_task = NULL;
	struct list_item *clist, *tlist;
	uint64_t next_delta = UINT64_MAX, delta, deadline;
	int reschedule = 0;
 
	/* any tasks in the scheduler ? */
	if (list_is_empty(&sch->list))
		return NULL;

	/* check every queued or running task in list */
	list_for_item_safe(clist, tlist, &sch->list) {
		task = container_of(clist, struct task, list);

		/* only check queued tasks */
		if (task->state != TASK_STATE_QUEUED)
			continue;

		/* include the length of task in deadline calc */
		deadline = task->deadline - task->max_rtime;

		/* get earliest deadline */
		if (current < deadline) {
			delta = deadline - current;

			if (delta < next_delta) {
				next_delta = delta;
				next_task = task;
			}

		} else {
			/* missed scheduling - will be rescheduled */
			trace_pipe("ed!");

			/* have we already tried to rescheule ? */
			if (reschedule++)
				edf_reschedule(task, current);
			else {
				/* reschedule failed */
				list_item_del(&task->list);
				task->state = TASK_STATE_CANCEL;
			}
		}
	}

	return next_task;
}

/* work set in the future when next task can be scheduled */
static uint32_t sch_work(void *data, uint32_t delay)
{
	tracev_pipe("wrk");
	schedule();
	return 0;
}

/*
 * EDF Scheduler - Earliest Deadline First Scheduler.
 *
 * Schedule task with the earliest deadline from task list.
 * Can run in IRQ context.
 */
struct task *schedule_edf(void)
{
	struct task *task, *next_plus1_task = NULL;
	uint64_t current;
	uint32_t flags;

	tracev_pipe("edf");

	/* get next component scheduled  */
	spin_lock_irq(&sch->lock, flags);

	/* get the current time */
	current = platform_timer_get(platform_timer);

	/* get next task to be scheduled */
	task = edf_get_next(current, NULL);

	spin_unlock_irq(&sch->lock, flags);
	interrupt_clear(PLATFORM_SCHEDULE_IRQ);

	/* any tasks ? */
	if (task == NULL)
		return NULL;

	/* can task be started now ? */
	if (task->start > current) {
		/* no, then schedule wake up */
		next_plus1_task = task;
	} else {
		/* yes, get next task and run this one now */
		next_plus1_task = edf_get_next(current, task);

		/* run current task */
		task->start = current;
		arch_run_task(task);
	}

	/* teall caller about next task (after current) */
	return next_plus1_task;
}

/* delete task from scheduler */
int schedule_task_del(struct task *task)
{
	uint32_t flags;
	int ret = 0;

	tracev_pipe("del");

	/* add task to list */
	spin_lock_irq(&sch->lock, flags);

	/* is task already running ? */
	if (task->state == TASK_STATE_RUNNING) {
		ret = -EAGAIN;
		goto out;
	}

	list_item_del(&task->list);
	task->state = TASK_STATE_COMPLETED;

out:
	spin_unlock_irq(&sch->lock, flags);
	return ret;
}

/*
 * Add a new task to the scheduler to be run and define a scheduling
 * window in time for the task to be ran. i.e. task will run between start and
 * deadline times.
 *
 * start is in microseconds relative to last task start time.
 * deadline is in microseconds relative to start.
 */
void schedule_task(struct task *task, uint64_t start, uint64_t deadline)
{
	uint32_t flags;
	uint64_t current;

	tracev_pipe("ad!");

	spin_lock_irq(&sch->lock, flags);

	/* is task already running ? - not enough MIPS to complete ? */
	if (task->state == TASK_STATE_RUNNING) {
		trace_pipe("tsk");
		spin_unlock_irq(&sch->lock, flags);
		return;
	}

	/* get the current time */
	current = platform_timer_get(platform_timer);

	/* calculate start time - TODO: include MIPS */
	if (start == 0)
		task->start = current;
	else
		task->start = task->start + clock_us_to_ticks(sch->clock, start) -
			PLATFORM_SCHEDULE_COST;

	/* calculate deadline - TODO: include MIPS */
	task->deadline = task->start + clock_us_to_ticks(sch->clock, deadline);

	/* add task to list */
	list_item_append(&task->list, &sch->list);
	task->state = TASK_STATE_QUEUED;
	spin_unlock_irq(&sch->lock, flags);

	/* rerun scheduler */
	schedule();
}

/* Remove a task from the scheduler when complete */
void schedule_task_complete(struct task *task)
{
	uint32_t flags;

	tracev_pipe("com");

	spin_lock_irq(&sch->lock, flags);
	list_item_del(&task->list);
	task->state = TASK_STATE_COMPLETED;
	spin_unlock_irq(&sch->lock, flags);
}

void scheduler_run(void *unused)
{
	struct task *next_task;

	tracev_pipe("run");

	/* EDF is only scheduler supported atm */
	next_task = schedule_edf();
	if (next_task) {
		work_reschedule_default_at(&sch->work, next_task->start);
	}
}

/* run the scheduler */
void schedule(void)
{
	tracev_pipe("sch");

	/* TODO: detect current IRQ context and call scheduler_run if both
	 * current context matches scheduler context. saves a DSP context
	 * switch.
	 */

	/* the scheduler is run in IRQ context */
	interrupt_set(PLATFORM_SCHEDULE_IRQ);
}

/* Initialise the scheduler */
int scheduler_init(struct reef *reef)
{
	trace_pipe("ScI");

	sch = rzalloc(RZONE_SYS, RFLAGS_NONE, sizeof(*sch));
	list_init(&sch->list);
	spinlock_init(&sch->lock);
	sch->clock = PLATFORM_SCHED_CLOCK;
	work_init(&sch->work, sch_work, sch, WORK_ASYNC);

	/* configure scheduler interrupt */
	interrupt_register(PLATFORM_SCHEDULE_IRQ, scheduler_run, NULL);
	interrupt_enable(PLATFORM_SCHEDULE_IRQ);

	return 0;
}
