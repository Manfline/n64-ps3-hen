/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — hilo de trabajos largos de baja prioridad. Ver bgjob.h.
 */
#include "platform/ps3/bgjob.h"

#include <string.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/sem.h>

static struct {
	bool ready;
	volatile int stop;
	volatile int busy;     /* un trabajo pedido o en marcha */
	bgjob_fn fn;
	void *arg;
	sys_ppu_thread_t tid;
	sys_sem_t sem;
} job;

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

static void bgjob_thread(void *unused)
{
	(void)unused;
	while (!job.stop) {
		sysSemWait(job.sem, 1000000);
		if (job.stop)
			break;
		if (!job.busy)
			continue;
		lwsync();
		job.fn(job.arg);
		lwsync();
		job.busy = 0;
	}
	sysThreadExit(0);
}

bool bgjob_init(void)
{
	if (job.ready)
		return true;
	memset(&job, 0, sizeof job);
	sys_sem_attr_t attr;
	memset(&attr, 0, sizeof attr);
	attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
	attr.attr_pshared = SYS_SEM_ATTR_PSHARED;
	if (sysSemCreate(&job.sem, &attr, 0, 1) != 0)
		return false;
	if (sysThreadCreate(&job.tid, bgjob_thread, NULL, BGJOB_PRIO, BGJOB_STACK, THREAD_JOINABLE,
			    (char *)"salsa64_bgjob") != 0) {
		sysSemDestroy(job.sem);
		return false;
	}
	job.ready = true;
	return true;
}

bool bgjob_ready(void)
{
	return job.ready;
}

bool bgjob_submit(bgjob_fn fn, void *arg)
{
	if (!job.ready || job.busy)
		return false;
	job.fn = fn;
	job.arg = arg;
	lwsync();
	job.busy = 1;
	sysSemPost(job.sem, 1);  /* si ya está en 1, da error: no importa */
	return true;
}

bool bgjob_busy(void)
{
	bool b = job.busy != 0;
	lwsync();
	return b;
}

bool bgjob_wait(uint64_t timeout_us)
{
	uint64_t t0 = sysGetSystemTime();
	while (bgjob_busy()) {
		if (timeout_us && sysGetSystemTime() - t0 >= timeout_us)
			return false;
		sysUsleep(1000);
	}
	return true;
}

void bgjob_shutdown(void)
{
	if (!job.ready)
		return;
	bgjob_wait(0);
	u64 rv;
	job.stop = 1;
	sysSemPost(job.sem, 1);
	sysThreadJoin(job.tid, &rv);
	sysSemDestroy(job.sem);
	job.ready = false;
}
