/*
 * Copyright (c) 1996, by Steve Passe
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. The name of the developer may NOT be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/i386/i386/mpapic.c,v 1.37.2.7 2003/01/25 02:31:47 peter Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/machintr.h>
#include <machine/globaldata.h>
#include <machine/clock.h>
#include <machine/limits.h>
#include <machine/smp.h>
#include <machine/md_var.h>
#include <machine/pmap.h>
#include <machine/specialreg.h>
#include <machine_base/apic/lapic.h>
#include <machine_base/apic/ioapic.h>
#include <machine_base/apic/ioapic_abi.h>
#include <machine_base/apic/apicvar.h>
#include <machine_base/icu/icu_var.h>
#include <machine/segments.h>
#include <sys/thread2.h>
#include <sys/spinlock2.h>

#include <machine/cputypes.h>
#include <machine/intr_machdep.h>

extern int naps;

volatile lapic_t *lapic;

static void	lapic_timer_calibrate(void);
static void	lapic_timer_set_divisor(int);
static void	lapic_timer_fixup_handler(void *);
static void	lapic_timer_restart_handler(void *);


static int	lapic_timer_enable = 1;
TUNABLE_INT("hw.lapic_timer_enable", &lapic_timer_enable);

static int	lapic_timer_tscdeadline = 1;
TUNABLE_INT("hw.lapic_timer_tscdeadline", &lapic_timer_tscdeadline);

static void	lapic_timer_tscdlt_reload(struct cputimer_intr *, sysclock_t);
static void	lapic_timer_intr_reload(struct cputimer_intr *, sysclock_t);
static void	lapic_timer_intr_enable(struct cputimer_intr *);
static void	lapic_timer_intr_restart(struct cputimer_intr *);
static void	lapic_timer_intr_pmfixup(struct cputimer_intr *);

static struct cputimer_intr lapic_cputimer_intr = {
	.freq = 0,
	.reload = lapic_timer_intr_reload,
	.enable = lapic_timer_intr_enable,
	.config = cputimer_intr_default_config,
	.restart = lapic_timer_intr_restart,
	.pmfixup = lapic_timer_intr_pmfixup,
	.initclock = cputimer_intr_default_initclock,
	.pcpuhand = NULL,
	.next = SLIST_ENTRY_INITIALIZER,
	.name = "lapic",
	.type = CPUTIMER_INTR_LAPIC,
	.prio = CPUTIMER_INTR_PRIO_LAPIC,
	.caps = CPUTIMER_INTR_CAP_NONE,
	.priv = NULL
};

static int		lapic_timer_divisor_idx = -1;
static const uint32_t	lapic_timer_divisors[] = {
	APIC_TDCR_2,	APIC_TDCR_4,	APIC_TDCR_8,	APIC_TDCR_16,
	APIC_TDCR_32,	APIC_TDCR_64,	APIC_TDCR_128,	APIC_TDCR_1
};
#define APIC_TIMER_NDIVISORS (int)(NELEM(lapic_timer_divisors))

static int	lapic_use_tscdeadline = 0;
/* The raw TSC frequency might not fit into a sysclock_t value. */
static int	lapic_timer_tscfreq_shift;

/*
 * APIC ID <-> CPU ID mapping structures.
 */
int	cpu_id_to_apic_id[NAPICID];
int	apic_id_to_cpu_id[NAPICID];
int	lapic_enable = 1;

/* Separate cachelines for each cpu's info. */
struct deadlines {
	uint64_t timestamp;
	uint64_t downcount_time;
	uint64_t padding[6];
};
struct deadlines *tsc_deadlines = NULL;

/*
 * Enable LAPIC, configure interrupts.
 */
void
lapic_init(boolean_t bsp)
{
	uint32_t timer;
	u_int   temp;

	if (bsp) {
		/* Decide whether we want to use TSC Deadline mode. */
		if (lapic_timer_tscdeadline != 0 &&
		    (cpu_feature2 & CPUID2_TSCDLT) &&
		    tsc_invariant && tsc_frequency != 0) {
			lapic_use_tscdeadline = 1;
			tsc_deadlines = kmalloc_cachealign(
			    sizeof(struct deadlines) * (naps + 1),
			    M_DEVBUF, M_WAITOK | M_ZERO);
		}
	}

	/*
	 * Install vectors
	 *
	 * Since IDT is shared between BSP and APs, these vectors
	 * only need to be installed once; we do it on BSP.
	 */
	if (bsp) {
		if (cpu_vendor_id == CPU_VENDOR_AMD &&
		    CPUID_TO_FAMILY(cpu_id) >= 0x0f &&
		    CPUID_TO_FAMILY(cpu_id) < 0x17) {	/* XXX */
			uint32_t tcr;

			/*
			 * Set the LINTEN bit in the HyperTransport
			 * Transaction Control Register.
			 *
			 * This will cause EXTINT and NMI interrupts
			 * routed over the hypertransport bus to be
			 * fed into the LAPIC LINT0/LINT1.  If the bit
			 * isn't set, the interrupts will go to the
			 * general cpu INTR/NMI pins.  On a dual-core
			 * cpu the interrupt winds up going to BOTH cpus.
			 * The first cpu that does the interrupt ack
			 * cycle will get the correct interrupt.  The
			 * second cpu that does it will get a spurious
			 * interrupt vector (typically IRQ 7).
			 */
			outl(0x0cf8,
			    (1 << 31) |	/* enable */
			    (0 << 16) |	/* bus */
			    (0x18 << 11) | /* dev (cpu + 0x18) */
			    (0 << 8) |	/* func */
			    0x68	/* reg */
			    );
			tcr = inl(0xcfc);
			if ((tcr & 0x00010000) == 0) {
				kprintf("LAPIC: AMD LINTEN on\n");
				outl(0xcfc, tcr|0x00010000);
			}
			outl(0x0cf8, 0);
		}

		/* Install a 'Spurious INTerrupt' vector */
		setidt_global(XSPURIOUSINT_OFFSET, Xspuriousint,
		    SDT_SYSIGT, SEL_KPL, 0);

		/* Install a timer vector */
		setidt_global(XTIMER_OFFSET, Xtimer,
		    SDT_SYSIGT, SEL_KPL, 0);

		/* Install an inter-CPU IPI for TLB invalidation */
		setidt_global(XINVLTLB_OFFSET, Xinvltlb,
		    SDT_SYSIGT, SEL_KPL, 0);

		/* Install an inter-CPU IPI for IPIQ messaging */
		setidt_global(XIPIQ_OFFSET, Xipiq,
		    SDT_SYSIGT, SEL_KPL, 0);

		/* Install an inter-CPU IPI for CPU stop/restart */
		setidt_global(XCPUSTOP_OFFSET, Xcpustop,
		    SDT_SYSIGT, SEL_KPL, 0);

		/* Install an inter-CPU IPI for TLB invalidation */
		setidt_global(XSNIFF_OFFSET, Xsniff,
		    SDT_SYSIGT, SEL_KPL, 0);
	}

	/*
	 * Setup LINT0 as ExtINT on the BSP.  This is theoretically an
	 * aggregate interrupt input from the 8259.  The INTA cycle
	 * will be routed to the external controller (the 8259) which
	 * is expected to supply the vector.
	 *
	 * Must be setup edge triggered, active high.
	 *
	 * Disable LINT0 on BSP, if I/O APIC is enabled.
	 *
	 * Disable LINT0 on the APs.  It doesn't matter what delivery
	 * mode we use because we leave it masked.
	 */
	temp = lapic->lvt_lint0;
	temp &= ~(APIC_LVT_MASKED | APIC_LVT_TRIG_MASK | 
		  APIC_LVT_POLARITY_MASK | APIC_LVT_DM_MASK);
	if (bsp) {
		temp |= APIC_LVT_DM_EXTINT;
		if (ioapic_enable)
			temp |= APIC_LVT_MASKED;
	} else {
		temp |= APIC_LVT_DM_FIXED | APIC_LVT_MASKED;
	}
	lapic->lvt_lint0 = temp;

	/*
	 * Setup LINT1 as NMI.
	 *
	 * Must be setup edge trigger, active high.
	 *
	 * Enable LINT1 on BSP, if I/O APIC is enabled.
	 *
	 * Disable LINT1 on the APs.
	 */
	temp = lapic->lvt_lint1;
	temp &= ~(APIC_LVT_MASKED | APIC_LVT_TRIG_MASK | 
		  APIC_LVT_POLARITY_MASK | APIC_LVT_DM_MASK);
	temp |= APIC_LVT_MASKED | APIC_LVT_DM_NMI;
	if (bsp && ioapic_enable)
		temp &= ~APIC_LVT_MASKED;
	lapic->lvt_lint1 = temp;

	/*
	 * Mask the LAPIC error interrupt, LAPIC performance counter
	 * interrupt.
	 */
	lapic->lvt_error = lapic->lvt_error | APIC_LVT_MASKED;
	lapic->lvt_pcint = lapic->lvt_pcint | APIC_LVT_MASKED;

	/*
	 * Set LAPIC timer vector and mask the LAPIC timer interrupt.
	 */
	timer = lapic->lvt_timer;
	timer &= ~APIC_LVTT_VECTOR;
	timer |= XTIMER_OFFSET;
	timer |= APIC_LVTT_MASKED;
	lapic->lvt_timer = timer;

	/*
	 * Set the Task Priority Register as needed.   At the moment allow
	 * interrupts on all cpus (the APs will remain CLId until they are
	 * ready to deal).
	 */
	temp = lapic->tpr;
	temp &= ~APIC_TPR_PRIO;		/* clear priority field */
	lapic->tpr = temp;

	/*
	 * AMD specific setup
	 */
	if (cpu_vendor_id == CPU_VENDOR_AMD &&
	    (lapic->version & APIC_VER_AMD_EXT_SPACE)) {
		uint32_t ext_feat;
		uint32_t count;
		uint32_t max_count;
		uint32_t lvt;
		uint32_t i;

		ext_feat = lapic->ext_feat;
		count = (ext_feat & APIC_EXTFEAT_MASK) >> APIC_EXTFEAT_SHIFT;
		max_count = sizeof(lapic->ext_lvt) / sizeof(lapic->ext_lvt[0]);
		if (count > max_count)
			count = max_count;
		for (i = 0; i < count; ++i) {
			lvt = lapic->ext_lvt[i].lvt;

			lvt &= ~(APIC_LVT_POLARITY_MASK | APIC_LVT_TRIG_MASK |
				 APIC_LVT_DM_MASK | APIC_LVT_MASKED);
			lvt |= APIC_LVT_MASKED | APIC_LVT_DM_FIXED;

			switch(i) {
			case APIC_EXTLVT_IBS:
				break;
			case APIC_EXTLVT_MCA:
				break;
			case APIC_EXTLVT_DEI:
				break;
			case APIC_EXTLVT_SBI:
				break;
			default:
				break;
			}
			if (bsp) {
				kprintf("   LAPIC AMD elvt%d: 0x%08x",
					i, lapic->ext_lvt[i].lvt);
				if (lapic->ext_lvt[i].lvt != lvt)
					kprintf(" -> 0x%08x", lvt);
				kprintf("\n");
			}
			lapic->ext_lvt[i].lvt = lvt;
		}
	}

	/* 
	 * Enable the LAPIC 
	 */
	temp = lapic->svr;
	temp |= APIC_SVR_ENABLE;	/* enable the LAPIC */
	temp &= ~APIC_SVR_FOCUS_DISABLE; /* enable lopri focus processor */

	if (lapic->version & APIC_VER_EOI_SUPP) {
		if (temp & APIC_SVR_EOI_SUPP) {
			temp &= ~APIC_SVR_EOI_SUPP;
			if (bsp)
				kprintf("    LAPIC disabling EOI supp\n");
		}
	}

	/*
	 * Set the spurious interrupt vector.  The low 4 bits of the vector
	 * must be 1111.
	 */
	if ((XSPURIOUSINT_OFFSET & 0x0F) != 0x0F)
		panic("bad XSPURIOUSINT_OFFSET: 0x%08x", XSPURIOUSINT_OFFSET);
	temp &= ~APIC_SVR_VECTOR;
	temp |= XSPURIOUSINT_OFFSET;

	lapic->svr = temp;

	/*
	 * Pump out a few EOIs to clean out interrupts that got through
	 * before we were able to set the TPR.
	 */
	lapic->eoi = 0;
	lapic->eoi = 0;
	lapic->eoi = 0;

	if (bsp) {
		lapic_timer_calibrate();
		if (lapic_timer_enable) {
			if (cpu_thermal_feature & CPUID_THERMAL_ARAT) {
				/*
				 * Local APIC timer will not stop
				 * in deep C-state.
				 */
				lapic_cputimer_intr.caps |=
				    CPUTIMER_INTR_CAP_PS;
			}
			if (lapic_use_tscdeadline) {
				lapic_cputimer_intr.reload =
				    lapic_timer_tscdlt_reload;
			}
			cputimer_intr_register(&lapic_cputimer_intr);
			cputimer_intr_select(&lapic_cputimer_intr, 0);
		}
	} else if (!lapic_use_tscdeadline) {
		lapic_timer_set_divisor(lapic_timer_divisor_idx);
	}

	if (bootverbose)
		apic_dump("apic_initialize()");
}

static void
lapic_timer_set_divisor(int divisor_idx)
{
	KKASSERT(divisor_idx >= 0 && divisor_idx < APIC_TIMER_NDIVISORS);
	lapic->dcr_timer = lapic_timer_divisors[divisor_idx];
}

static void
lapic_timer_oneshot(u_int count)
{
	uint32_t value;

	value = lapic->lvt_timer;
	value &= ~(APIC_LVTT_PERIODIC | APIC_LVTT_TSCDLT);
	lapic->lvt_timer = value;
	lapic->icr_timer = count;
}

static void
lapic_timer_oneshot_quick(u_int count)
{
	lapic->icr_timer = count;
}

static void
lapic_timer_tscdeadline_quick(uint64_t diff)
{
	uint64_t val = rdtsc() + diff;

	wrmsr(MSR_TSC_DEADLINE, val);
	tsc_deadlines[mycpuid].timestamp = val;
}

static uint64_t
lapic_scale_to_tsc(unsigned value, unsigned scale)
{
	uint64_t val;

	val = value;
	val *= tsc_frequency;
	val += (scale - 1);
	val /= scale;
	return val;
}

static void
lapic_timer_calibrate(void)
{
	sysclock_t value;

	/* No need to calibrate lapic_timer, if we will use TSC Deadline mode */
	if (lapic_use_tscdeadline) {
		lapic_timer_tscfreq_shift = 0;
		while ((tsc_frequency >> lapic_timer_tscfreq_shift) > INT_MAX)
			lapic_timer_tscfreq_shift++;
		lapic_cputimer_intr.freq =
		    tsc_frequency >> lapic_timer_tscfreq_shift;
		kprintf(
		    "lapic: TSC Deadline Mode: shift %d, frequency %u Hz\n",
		    lapic_timer_tscfreq_shift, lapic_cputimer_intr.freq);
		return;
	}

	/* Try to calibrate the local APIC timer. */
	for (lapic_timer_divisor_idx = 0;
	     lapic_timer_divisor_idx < APIC_TIMER_NDIVISORS;
	     lapic_timer_divisor_idx++) {
		u_int cnt;

		if (sys_cputimer->freq >= 10000000)
			cnt = 500000;
		else
			cnt = 2000000;

		lapic_timer_set_divisor(lapic_timer_divisor_idx);
		lapic_timer_oneshot(APIC_TIMER_MAX_COUNT);
		DELAY(cnt);
		value = APIC_TIMER_MAX_COUNT - lapic->ccr_timer;
		if (value != APIC_TIMER_MAX_COUNT)
			break;
	}
	if (lapic_timer_divisor_idx >= APIC_TIMER_NDIVISORS)
		panic("lapic: no proper timer divisor?!");
	if (sys_cputimer->freq >= 10000000)
		lapic_cputimer_intr.freq = value * 2;
	else
		lapic_cputimer_intr.freq = value / 2;

	kprintf("lapic: divisor index %d, frequency %u Hz\n",
		lapic_timer_divisor_idx, lapic_cputimer_intr.freq);
}

static void
lapic_timer_tscdlt_reload(struct cputimer_intr *cti, sysclock_t reload)
{
	struct globaldata *gd = mycpu;
	uint64_t diff, now, val;

	if (reload > 1000*1000*1000)
		reload = 1000*1000*1000;
	diff = (uint64_t)reload * tsc_frequency / sys_cputimer->freq;
	if (diff < 4)
		diff = 4;
	if (cpu_vendor_id == CPU_VENDOR_INTEL)
		cpu_lfence();
	else
		cpu_mfence();
	now = rdtsc();
	val = now + diff;
	if (gd->gd_timer_running) {
		uint64_t deadline = tsc_deadlines[mycpuid].timestamp;
		if (deadline == 0 || now > deadline || val < deadline) {
			wrmsr(MSR_TSC_DEADLINE, val);
			tsc_deadlines[mycpuid].timestamp = val;
		}
	} else {
		gd->gd_timer_running = 1;
		wrmsr(MSR_TSC_DEADLINE, val);
		tsc_deadlines[mycpuid].timestamp = val;
	}
}

static void
lapic_timer_intr_reload(struct cputimer_intr *cti, sysclock_t reload)
{
	struct globaldata *gd = mycpu;

	reload = (int64_t)reload * cti->freq / sys_cputimer->freq;
	if (reload < 2)
		reload = 2;

	if (gd->gd_timer_running) {
		if (reload < lapic->ccr_timer)
			lapic_timer_oneshot_quick(reload);
	} else {
		gd->gd_timer_running = 1;
		lapic_timer_oneshot_quick(reload);
	}
}

static void
lapic_timer_intr_enable(struct cputimer_intr *cti __unused)
{
	uint32_t timer;

	timer = lapic->lvt_timer;
	timer &= ~(APIC_LVTT_MASKED | APIC_LVTT_PERIODIC | APIC_LVTT_TSCDLT);
	if (lapic_use_tscdeadline)
		timer |= APIC_LVTT_TSCDLT;
	lapic->lvt_timer = timer;
	if (lapic_use_tscdeadline)
		cpu_mfence();

	lapic_timer_fixup_handler(NULL);
}

static void
lapic_timer_fixup_handler(void *arg)
{
	int *started = arg;

	if (started != NULL)
		*started = 0;

	if (cpu_vendor_id == CPU_VENDOR_AMD) {
		/*
		 * Detect the presence of C1E capability mostly on latest
		 * dual-cores (or future) k8 family.  This feature renders
		 * the local APIC timer dead, so we disable it by reading
		 * the Interrupt Pending Message register and clearing both
		 * C1eOnCmpHalt (bit 28) and SmiOnCmpHalt (bit 27).
		 * 
		 * Reference:
		 *   "BIOS and Kernel Developer's Guide for AMD NPT
		 *    Family 0Fh Processors"
		 *   #32559 revision 3.00
		 */
		if ((cpu_id & 0x00000f00) == 0x00000f00 &&
		    (cpu_id & 0x0fff0000) >= 0x00040000) {
			uint64_t msr;

			msr = rdmsr(0xc0010055);
			if (msr & 0x18000000) {
				struct globaldata *gd = mycpu;

				kprintf("cpu%d: AMD C1E detected\n",
					gd->gd_cpuid);
				wrmsr(0xc0010055, msr & ~0x18000000ULL);

				/*
				 * We are kinda stalled;
				 * kick start again.
				 */
				gd->gd_timer_running = 1;
				if (lapic_use_tscdeadline) {
					/* Maybe reached in Virtual Machines? */
					lapic_timer_tscdeadline_quick(5000);
				} else {
					lapic_timer_oneshot_quick(2);
				}

				if (started != NULL)
					*started = 1;
			}
		}
	}
}

static void
lapic_timer_restart_handler(void *dummy __unused)
{
	int started;

	lapic_timer_fixup_handler(&started);
	if (!started) {
		struct globaldata *gd = mycpu;

		gd->gd_timer_running = 1;
		if (lapic_use_tscdeadline) {
			/* Maybe reached in Virtual Machines? */
			lapic_timer_tscdeadline_quick(5000);
		} else {
			lapic_timer_oneshot_quick(2);
		}
	}
}

/*
 * This function is called only by ACPICA code currently:
 * - AMD C1E fixup.  AMD C1E only seems to happen after ACPI
 *   module controls PM.  So once ACPICA is attached, we try
 *   to apply the fixup to prevent LAPIC timer from hanging.
 */
static void
lapic_timer_intr_pmfixup(struct cputimer_intr *cti __unused)
{
	lwkt_send_ipiq_mask(smp_active_mask,
			    lapic_timer_fixup_handler, NULL);
}

static void
lapic_timer_intr_restart(struct cputimer_intr *cti __unused)
{
	lwkt_send_ipiq_mask(smp_active_mask, lapic_timer_restart_handler, NULL);
}


/*
 * dump contents of local APIC registers
 */
void
apic_dump(char* str)
{
	kprintf("SMP: CPU%d %s:\n", mycpu->gd_cpuid, str);
	kprintf("     lint0: 0x%08x lint1: 0x%08x TPR: 0x%08x SVR: 0x%08x\n",
		lapic->lvt_lint0, lapic->lvt_lint1, lapic->tpr, lapic->svr);
}

/*
 * Inter Processor Interrupt functions.
 */

/*
 * Send APIC IPI 'vector' to 'destType' via 'deliveryMode'.
 *
 *  destType is 1 of: APIC_DEST_SELF, APIC_DEST_ALLISELF, APIC_DEST_ALLESELF
 *  vector is any valid SYSTEM INT vector
 *  delivery_mode is 1 of: APIC_DELMODE_FIXED, APIC_DELMODE_LOWPRIO
 *
 * WARNINGS!
 *
 * We now implement a per-cpu interlock (gd->gd_npoll) to prevent more than
 * one IPI from being sent to any given cpu at a time.  Thus we no longer
 * have to process incoming IPIs while waiting for the status to clear.
 * No deadlock should be possible.
 *
 * We now physically disable interrupts for the lapic ICR operation.  If
 * we do not do this then it looks like an EOI sent to the lapic (which
 * occurs even with a critical section) can interfere with the command
 * register ready status and cause an IPI to be lost.
 *
 * e.g. an interrupt can occur, issue the EOI, IRET, and cause the command
 * register to busy just before we write to icr_lo, resulting in a lost
 * issuance.  This only appears to occur on Intel cpus and is not
 * documented.  It could simply be that cpus are so fast these days that
 * it was always an issue, but is only now rearing its ugly head.  This
 * is conjecture.
 */
int
apic_ipi(int dest_type, int vector, int delivery_mode)
{
	uint32_t icr_hi;
	uint32_t icr_lo;
	int64_t tsc;
	int loops = 1;

	if ((lapic->icr_lo & APIC_DELSTAT_MASK) != 0) {
		tsc = rdtsc();
		while ((lapic->icr_lo & APIC_DELSTAT_MASK) != 0) {
			cpu_pause();
			if ((tsc_sclock_t)(rdtsc() -
					   (tsc + tsc_frequency)) > 0) {
				kprintf("apic_ipi stall cpu %d (sing)\n",
					mycpuid);
				tsc = rdtsc();
				if (++loops > 30)
					panic("apic stall");
			}
		}
	}
	icr_hi = lapic->icr_hi & ~APIC_ID_MASK;
	icr_lo = (lapic->icr_lo & APIC_ICRLO_RESV_MASK) | dest_type | 
		 APIC_LEVEL_ASSERT | delivery_mode | vector;
	lapic->icr_hi = icr_hi;
	lapic->icr_lo = icr_lo;

	return 0;
}

/*
 * Interrupts must be hard-disabled by caller
 */
void
single_apic_ipi(int cpu, int vector, int delivery_mode)
{
	uint32_t  icr_lo;
	uint32_t  icr_hi;
	int64_t tsc;
	int loops = 1;

	if ((lapic->icr_lo & APIC_DELSTAT_MASK) != 0) {
		tsc = rdtsc();
		while ((lapic->icr_lo & APIC_DELSTAT_MASK) != 0) {
			cpu_pause();
			if ((tsc_sclock_t)(rdtsc() -
					   (tsc + tsc_frequency)) > 0) {
				kprintf("single_apic_ipi stall cpu %d (sing)\n",
					mycpuid);
				tsc = rdtsc();
				if (++loops > 30)
					panic("apic stall");
			}
		}
	}
	icr_hi = lapic->icr_hi & ~APIC_ID_MASK;
	icr_hi |= (CPUID_TO_APICID(cpu) << 24);

	/* build ICR_LOW */
	icr_lo = (lapic->icr_lo & APIC_ICRLO_RESV_MASK) |
		 APIC_LEVEL_ASSERT | APIC_DEST_DESTFLD | delivery_mode | vector;

	/* write APIC ICR */
	lapic->icr_hi = icr_hi;
	lapic->icr_lo = icr_lo;
}

#if 0	

/*
 * Returns 0 if the apic is busy, 1 if we were able to queue the request.
 *
 * NOT WORKING YET!  The code as-is may end up not queueing an IPI at all
 * to the target, and the scheduler does not 'poll' for IPI messages.
 */
int
single_apic_ipi_passive(int cpu, int vector, int delivery_mode)
{
	u_long  icr_lo;
	u_long  icr_hi;
	unsigned long rflags;

	rflags = read_rflags();
	cpu_disable_intr();
	if ((lapic->icr_lo & APIC_DELSTAT_MASK) != 0) {
		write_rflags(rflags);
		return(0);
	}
	icr_hi = lapic->icr_hi & ~APIC_ID_MASK;
	icr_hi |= (CPUID_TO_APICID(cpu) << 24);
	lapic->icr_hi = icr_hi;

	/* build IRC_LOW */
	icr_lo = (lapic->icr_lo & APIC_RESV2_MASK) |
		 APIC_DEST_DESTFLD | delivery_mode | vector;

	/* write APIC ICR */
	lapic->icr_lo = icr_lo;
	write_rflags(rflags);

	return(1);
}

#endif

/*
 * Send APIC IPI 'vector' to 'target's via 'delivery_mode'.
 *
 * target is a bitmask of destination cpus.  Vector is any
 * valid system INT vector.  Delivery mode may be either
 * APIC_DELMODE_FIXED or APIC_DELMODE_LOWPRIO.
 *
 * Interrupts must be hard-disabled by caller
 */
void
selected_apic_ipi(cpumask_t target, int vector, int delivery_mode)
{
	while (CPUMASK_TESTNZERO(target)) {
		int n = BSFCPUMASK(target);
		CPUMASK_NANDBIT(target, n);
		single_apic_ipi(n, vector, delivery_mode);
	}
}

/*
 * Load a 'downcount time' in uSeconds.
 */
void
set_apic_timer(int us)
{
	u_int count;

	if (lapic_use_tscdeadline) {
		uint64_t val;

		val = lapic_scale_to_tsc(us, 1000000);
		val += rdtsc();
		/* No need to arm the lapic here, just track the timeout. */
		tsc_deadlines[mycpuid].downcount_time = val;
		return;
	}

	/*
	 * When we reach here, lapic timer's frequency
	 * must have been calculated as well as the
	 * divisor (lapic->dcr_timer is setup during the
	 * divisor calculation).
	 */
	KKASSERT(lapic_cputimer_intr.freq != 0 &&
		 lapic_timer_divisor_idx >= 0);

	count = ((us * (int64_t)lapic_cputimer_intr.freq) + 999999) / 1000000;
	lapic_timer_oneshot(count);
}


/*
 * Read remaining time in timer, in microseconds (rounded up).
 */
int
read_apic_timer(void)
{
	uint64_t val;

	if (lapic_use_tscdeadline) {
		uint64_t now;

		val = tsc_deadlines[mycpuid].downcount_time;
		now = rdtsc();
		if (val == 0 || now > val) {
			return 0;
		} else {
			val -= now;
			val *= 1000000;
			val += (tsc_frequency - 1);
			val /= tsc_frequency;
			if (val > INT_MAX)
				val = INT_MAX;
			return val;
		}
	}

	val = lapic->ccr_timer;
	if (val == 0)
		return 0;

	KKASSERT(lapic_cputimer_intr.freq > 0);
	val *= 1000000;
	val += (lapic_cputimer_intr.freq - 1);
	val /= lapic_cputimer_intr.freq;
	if (val > INT_MAX)
		val = INT_MAX;
	return val;
}


/*
 * Spin-style delay, set delay time in uS, spin till it drains.
 */
void
u_sleep(int count)
{
	set_apic_timer(count);
	while (read_apic_timer())
		 /* spin */ ;
}

int
lapic_unused_apic_id(int start)
{
	int i;

	for (i = start; i < APICID_MAX; ++i) {
		if (APICID_TO_CPUID(i) == -1)
			return i;
	}
	return NAPICID;
}

void
lapic_map(vm_paddr_t lapic_addr)
{
	lapic = pmap_mapdev_uncacheable(lapic_addr, sizeof(struct LAPIC));
}

static TAILQ_HEAD(, lapic_enumerator) lapic_enumerators =
	TAILQ_HEAD_INITIALIZER(lapic_enumerators);

int
lapic_config(void)
{
	struct lapic_enumerator *e;
	int error, i, ap_max;

	KKASSERT(lapic_enable);

	for (i = 0; i < NAPICID; ++i)
		APICID_TO_CPUID(i) = -1;

	TAILQ_FOREACH(e, &lapic_enumerators, lapic_link) {
		error = e->lapic_probe(e);
		if (!error)
			break;
	}
	if (e == NULL) {
		kprintf("LAPIC: Can't find LAPIC\n");
		return ENXIO;
	}

	error = e->lapic_enumerate(e);
	if (error) {
		kprintf("LAPIC: enumeration failed\n");
		return ENXIO;
	}

	ap_max = MAXCPU - 1;
	TUNABLE_INT_FETCH("hw.ap_max", &ap_max);
	if (ap_max > MAXCPU - 1)
		ap_max = MAXCPU - 1;

	if (naps > ap_max) {
		kprintf("LAPIC: Warning use only %d out of %d "
			"available APs\n",
			ap_max, naps);
		naps = ap_max;
	}

	return 0;
}

void
lapic_enumerator_register(struct lapic_enumerator *ne)
{
	struct lapic_enumerator *e;

	TAILQ_FOREACH(e, &lapic_enumerators, lapic_link) {
		if (e->lapic_prio < ne->lapic_prio) {
			TAILQ_INSERT_BEFORE(e, ne, lapic_link);
			return;
		}
	}
	TAILQ_INSERT_TAIL(&lapic_enumerators, ne, lapic_link);
}

void
lapic_set_cpuid(int cpu_id, int apic_id)
{
	CPUID_TO_APICID(cpu_id) = apic_id;
	APICID_TO_CPUID(apic_id) = cpu_id;
}

void
lapic_fixup_noioapic(void)
{
	u_int   temp;

	/* Only allowed on BSP */
	KKASSERT(mycpuid == 0);
	KKASSERT(!ioapic_enable);

	temp = lapic->lvt_lint0;
	temp &= ~APIC_LVT_MASKED;
	lapic->lvt_lint0 = temp;

	temp = lapic->lvt_lint1;
	temp |= APIC_LVT_MASKED;
	lapic->lvt_lint1 = temp;
}

static void
lapic_sysinit(void *dummy __unused)
{
	if (lapic_enable) {
		int error;

		error = lapic_config();
		if (error)
			lapic_enable = 0;
	}

	if (lapic_enable) {
		/* Initialize BSP's local APIC */
		lapic_init(TRUE);
	} else if (ioapic_enable) {
		ioapic_enable = 0;
		icu_reinit_noioapic();
	}
}
SYSINIT(lapic, SI_BOOT2_LAPIC, SI_ORDER_FIRST, lapic_sysinit, NULL);
