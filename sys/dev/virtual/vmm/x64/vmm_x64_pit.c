/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reusable x86-64 8254 PIT state machine.
 */
#include <sys/callout.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/time.h>

#include "../vmm_machine.h"
#include "vmm_x64.h"
#include "vmm_x64_pit.h"

#define VMM_X64_PIT_PORT_CHANNEL0	0x40U
#define VMM_X64_PIT_PORT_CHANNEL2	0x42U
#define VMM_X64_PIT_PORT_CONTROL	0x43U
#define VMM_X64_PIT_MAX_COUNT		65536U
#define VMM_X64_PIT_NSEC_PER_SEC	1000000000ULL

struct vmm_x64_pit {
	/* token protects state and destroying. */
	struct lwkt_token token;
	struct vmm_machine *machine;
	struct callout callout;
	struct vmm_pit_state state;
	bool destroying;
};

static void vmm_x64_pit_timeout(void *);
static uint64_t vmm_x64_pit_now(void);
static uint64_t vmm_x64_pit_elapsed(uint64_t, int64_t);
static uint64_t vmm_x64_pit_count_ns(uint32_t);
static uint32_t vmm_x64_pit_current_count(
	const struct vmm_pit_channel_state *, uint64_t);
static uint8_t vmm_x64_pit_output(const struct vmm_pit_channel_state *,
	uint64_t);
static uint8_t vmm_x64_pit_status(const struct vmm_pit_channel_state *,
	uint64_t);
static void vmm_x64_pit_latch_count(struct vmm_pit_channel_state *,
	uint64_t);
static void vmm_x64_pit_load_count(struct vmm_pit_channel_state *,
	uint16_t, uint64_t);
static void vmm_x64_pit_arm(struct vmm_x64_pit *, uint64_t);
static int vmm_x64_pit_io_locked(struct vmm_x64_pit *,
	struct vmm_cpustate *, const struct vmm_cpuexit_io *);
static int vmm_x64_pit_read(struct vmm_x64_pit *, unsigned int, uint8_t *);
static int vmm_x64_pit_write(struct vmm_x64_pit *, unsigned int, uint8_t);

int
vmm_x64_pit_create(struct vmm_machine *machine)
{
	struct vmm_x64_pit *pit;
	uint64_t now;
	int error;

	if (machine == NULL)
		return EINVAL;
	pit = kmalloc(sizeof(*pit), M_VMM, M_WAITOK | M_ZERO);
	if (pit == NULL)
		return ENOMEM;
	lwkt_token_init(&pit->token, "vmmpit");
	callout_init_mp(&pit->callout);
	pit->machine = machine;
	now = vmm_x64_pit_now();
	pit->state.channels[0].count = VMM_X64_PIT_MAX_COUNT;
	pit->state.channels[0].rw_mode = 3;
	pit->state.channels[0].mode = 3;
	pit->state.channels[0].gate = 1;
	pit->state.channels[0].count_load_time = now;
	pit->state.channels[1].count = VMM_X64_PIT_MAX_COUNT;
	pit->state.channels[1].rw_mode = 3;
	pit->state.channels[1].mode = 3;
	pit->state.channels[1].gate = 1;
	pit->state.channels[1].count_load_time = now;
	pit->state.channels[2].count = VMM_X64_PIT_MAX_COUNT;
	pit->state.channels[2].rw_mode = 3;
	pit->state.channels[2].mode = 3;
	pit->state.channels[2].count_load_time = now;

	lwkt_gettoken(&machine->token);
	if (machine->destroying || !machine->irqchip) {
		error = ENXIO;
	} else if (machine->run_count != 0) {
		error = EBUSY;
	} else if (machine->pit != NULL) {
		error = EEXIST;
	} else {
		machine->pit = pit;
		error = 0;
	}
	lwkt_reltoken(&machine->token);
	if (error == 0)
		return 0;
	callout_terminate(&pit->callout);
	kfree(pit, M_VMM);
	return error;
}

void
vmm_x64_pit_destroy(struct vmm_machine *machine)
{
	struct vmm_x64_pit *pit;

	if (machine == NULL)
		return;
	lwkt_gettoken(&machine->token);
	pit = machine->pit;
	if (pit == NULL) {
		lwkt_reltoken(&machine->token);
		return;
	}
	lwkt_gettoken(&pit->token);
	machine->pit = NULL;
	pit->destroying = true;
	lwkt_reltoken(&pit->token);
	lwkt_reltoken(&machine->token);
	callout_drain(&pit->callout);
	callout_terminate(&pit->callout);
	kfree(pit, M_VMM);
}

int
vmm_x64_pit_get_state(struct vmm_machine *machine,
	struct vmm_pit_state *state)
{
	struct vmm_x64_pit *pit;
	uint64_t now;
	unsigned int channel;

	if (machine == NULL || state == NULL)
		return EINVAL;
	now = vmm_x64_pit_now();
	lwkt_gettoken(&machine->token);
	if (machine->destroying) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	pit = machine->pit;
	if (pit == NULL) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	lwkt_gettoken(&pit->token);
	if (pit->destroying) {
		lwkt_reltoken(&pit->token);
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	for (channel = 0; channel < VMM_X64_PIT_CHANNELS; ++channel)
		pit->state.channels[channel].status =
		    vmm_x64_pit_status(&pit->state.channels[channel], now);
	*state = pit->state;
	lwkt_reltoken(&pit->token);
	lwkt_reltoken(&machine->token);
	return 0;
}

int
vmm_x64_pit_set_state(struct vmm_machine *machine,
	const struct vmm_pit_state *state)
{
	struct vmm_x64_pit *pit;
	unsigned int channel;
	uint64_t now;

	if (machine == NULL || state == NULL ||
	    (state->flags & ~(VMM_PIT_FLAG_HPET_LEGACY |
	    VMM_PIT_FLAG_SPEAKER_DATA_ON)) != 0)
		return EINVAL;
	for (channel = 0; channel < VMM_X64_PIT_CHANNELS; ++channel) {
		if (state->channels[channel].count > VMM_X64_PIT_MAX_COUNT ||
		    state->channels[channel].rw_mode > 3 ||
		    state->channels[channel].mode > 7 ||
		    state->channels[channel].bcd > 1 ||
		    state->channels[channel].gate > 1)
			return EINVAL;
	}
	now = vmm_x64_pit_now();
	lwkt_gettoken(&machine->token);
	if (machine->destroying) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	pit = machine->pit;
	if (pit == NULL) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	lwkt_gettoken(&pit->token);
	if (pit->destroying) {
		lwkt_reltoken(&pit->token);
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	pit->state = *state;
	for (channel = 0; channel < VMM_X64_PIT_CHANNELS; ++channel) {
		if (pit->state.channels[channel].mode == 6)
			pit->state.channels[channel].mode = 2;
		else if (pit->state.channels[channel].mode == 7)
			pit->state.channels[channel].mode = 3;
		pit->state.channels[channel].status =
		    vmm_x64_pit_status(&pit->state.channels[channel], now);
	}
	vmm_x64_pit_arm(pit, now);
	lwkt_reltoken(&pit->token);
	lwkt_reltoken(&machine->token);
	return 0;
}

int
vmm_x64_pit_io(struct vmm_machine *machine, struct vmm_cpustate *state,
	const struct vmm_cpuexit_io *exit)
{
	struct vmm_x64_pit *pit;
	int error;

	if (machine == NULL || state == NULL || exit == NULL)
		return EINVAL;
	lwkt_gettoken(&machine->token);
	if (machine->destroying) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	pit = machine->pit;
	if (pit == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOENT;
	}
	error = vmm_x64_pit_io_locked(pit, state, exit);
	lwkt_reltoken(&machine->token);
	return error;
}

static int
vmm_x64_pit_io_locked(struct vmm_x64_pit *pit,
	struct vmm_cpustate *state,
	const struct vmm_cpuexit_io *exit)
{
	uint8_t value;
	int error;

	if (pit == NULL || state == NULL || exit == NULL)
		return EOPNOTSUPP;
	if (exit->port < VMM_X64_PIT_PORT_CHANNEL0 ||
	    exit->port > VMM_X64_PIT_PORT_CONTROL)
		return ENOENT;
	if (exit->str || exit->rep || exit->operand_size != 1)
		return EOPNOTSUPP;
	if (exit->in) {
		error = vmm_x64_pit_read(pit,
		    exit->port - VMM_X64_PIT_PORT_CHANNEL0, &value);
		if (error != 0)
			return error;
		state->gprs[VMM_X64_GPR_RAX] =
		    (state->gprs[VMM_X64_GPR_RAX] & ~0xffULL) | value;
	} else {
		value = state->gprs[VMM_X64_GPR_RAX] & 0xffU;
		error = vmm_x64_pit_write(pit,
		    exit->port - VMM_X64_PIT_PORT_CHANNEL0, value);
		if (error != 0)
			return error;
	}
	state->gprs[VMM_X64_GPR_RIP] = exit->npc;
	return 0;
}

static void
vmm_x64_pit_timeout(void *arg)
{
	struct vmm_x64_pit *pit = arg;
	uint64_t now;
	int raise_irq;

	now = vmm_x64_pit_now();
	lwkt_gettoken(&pit->token);
	raise_irq = !pit->destroying &&
	    (pit->state.flags & VMM_PIT_FLAG_HPET_LEGACY) == 0 &&
	    pit->state.channels[0].gate != 0 &&
	    pit->state.channels[0].count != 0;
	if (!pit->destroying)
		vmm_x64_pit_arm(pit, now);
	lwkt_reltoken(&pit->token);
	if (raise_irq)
		(void)vmm_machine_raise_irq(pit->machine, 0);
}

static uint64_t
vmm_x64_pit_now(void)
{
	struct timespec now;

	nanouptime(&now);
	return (uint64_t)now.tv_sec * VMM_X64_PIT_NSEC_PER_SEC + now.tv_nsec;
}

static uint64_t
vmm_x64_pit_elapsed(uint64_t now, int64_t start)
{
	uint64_t delta;

	if (start <= 0 || now <= (uint64_t)start)
		return 0;
	delta = now - (uint64_t)start;
	return (delta / VMM_X64_PIT_NSEC_PER_SEC) * VMM_X64_PIT_FREQUENCY +
	    ((delta % VMM_X64_PIT_NSEC_PER_SEC) * VMM_X64_PIT_FREQUENCY) /
	    VMM_X64_PIT_NSEC_PER_SEC;
}

static uint64_t
vmm_x64_pit_count_ns(uint32_t count)
{

	return ((uint64_t)count / VMM_X64_PIT_FREQUENCY) *
	    VMM_X64_PIT_NSEC_PER_SEC +
	    (((uint64_t)count % VMM_X64_PIT_FREQUENCY) *
	    VMM_X64_PIT_NSEC_PER_SEC) / VMM_X64_PIT_FREQUENCY;
}

static uint32_t
vmm_x64_pit_current_count(const struct vmm_pit_channel_state *channel,
	uint64_t now)
{
	uint64_t elapsed;
	uint32_t count;

	count = channel->count;
	if (count == 0 || channel->count_load_time <= 0 || channel->gate == 0)
		return count;
	elapsed = vmm_x64_pit_elapsed(now, channel->count_load_time);
	switch (channel->mode) {
	case 2:
	case 3:
		elapsed %= count;
		return elapsed == 0 ? count : count - (uint32_t)elapsed;
	default:
		return elapsed >= count ? 0 : count - (uint32_t)elapsed;
	}
}

static uint8_t
vmm_x64_pit_output(const struct vmm_pit_channel_state *channel,
	uint64_t now)
{
	uint64_t elapsed;
	uint32_t count;

	count = channel->count;
	if (count == 0 || channel->gate == 0)
		return 0;
	elapsed = vmm_x64_pit_elapsed(now, channel->count_load_time);
	switch (channel->mode) {
	case 0:
	case 4:
	case 5:
		return elapsed >= count;
	case 2:
		return elapsed % count != count - 1;
	case 3:
		return elapsed % count < (count + 1) / 2;
	default:
		return 0;
	}
}

static uint8_t
vmm_x64_pit_status(const struct vmm_pit_channel_state *channel,
	uint64_t now)
{

	return (vmm_x64_pit_output(channel, now) << 7) |
	    ((channel->rw_mode & 3U) << 4) |
	    ((channel->mode & 7U) << 1) | (channel->bcd & 1U);
}

static void
vmm_x64_pit_latch_count(struct vmm_pit_channel_state *channel,
	uint64_t now)
{

	if (channel->count_latched == 0) {
		channel->latched_count = vmm_x64_pit_current_count(channel, now);
		channel->count_latched = 1;
		channel->read_state = 0;
	}
}

static void
vmm_x64_pit_load_count(struct vmm_pit_channel_state *channel,
	uint16_t value, uint64_t now)
{

	channel->count = value == 0 ? VMM_X64_PIT_MAX_COUNT : value;
	channel->count_load_time = now;
	channel->read_state = 0;
	channel->write_state = 0;
	channel->count_latched = 0;
	channel->status_latched = 0;
}

static void
vmm_x64_pit_arm(struct vmm_x64_pit *pit, uint64_t now)
{
	struct vmm_pit_channel_state *channel;
	uint32_t remaining;
	uint64_t elapsed;
	uint64_t delay;
	uint64_t callout_ticks;

	channel = &pit->state.channels[0];
	if (pit->destroying ||
	    (pit->state.flags & VMM_PIT_FLAG_HPET_LEGACY) != 0 ||
	    channel->gate == 0 || channel->count == 0 ||
	    channel->count_load_time <= 0) {
		callout_stop_async(&pit->callout);
		return;
	}
	elapsed = vmm_x64_pit_elapsed(now, channel->count_load_time);
	if (channel->mode == 2 || channel->mode == 3) {
		elapsed %= channel->count;
		remaining = elapsed == 0 ? channel->count : channel->count - elapsed;
	} else {
		if (elapsed >= channel->count) {
			callout_stop_async(&pit->callout);
			return;
		}
		remaining = channel->count - elapsed;
	}
	delay = vmm_x64_pit_count_ns(remaining);
	callout_ticks = (delay * hz + VMM_X64_PIT_NSEC_PER_SEC - 1) /
	    VMM_X64_PIT_NSEC_PER_SEC;
	callout_reset(&pit->callout, (int)MAX(callout_ticks, 1),
	    vmm_x64_pit_timeout, pit);
}

static int
vmm_x64_pit_read(struct vmm_x64_pit *pit, unsigned int port, uint8_t *value)
{
	struct vmm_pit_channel_state *channel;
	uint32_t count;
	uint64_t now;

	if (value == NULL)
		return EINVAL;
	if (port == VMM_X64_PIT_PORT_CONTROL - VMM_X64_PIT_PORT_CHANNEL0) {
		*value = 0xff;
		return 0;
	}
	if (port >= VMM_X64_PIT_CHANNELS)
		return EOPNOTSUPP;
	now = vmm_x64_pit_now();
	lwkt_gettoken(&pit->token);
	if (pit->destroying) {
		lwkt_reltoken(&pit->token);
		return ENXIO;
	}
	channel = &pit->state.channels[port];
	if (channel->status_latched) {
		*value = channel->status;
		channel->status_latched = 0;
		lwkt_reltoken(&pit->token);
		return 0;
	}
	count = channel->count_latched ? channel->latched_count :
	    vmm_x64_pit_current_count(channel, now);
	switch (channel->rw_mode) {
	case 1:
		*value = count;
		channel->count_latched = 0;
		break;
	case 2:
		*value = count >> 8;
		channel->count_latched = 0;
		break;
	case 3:
		if (channel->read_state == 0) {
			*value = count;
			channel->read_state = 1;
		} else {
			*value = count >> 8;
			channel->read_state = 0;
			channel->count_latched = 0;
		}
		break;
	default:
		*value = 0;
		break;
	}
	lwkt_reltoken(&pit->token);
	return 0;
}

static int
vmm_x64_pit_write(struct vmm_x64_pit *pit, unsigned int port, uint8_t value)
{
	struct vmm_pit_channel_state *channel;
	unsigned int index;
	uint64_t now;

	now = vmm_x64_pit_now();
	lwkt_gettoken(&pit->token);
	if (pit->destroying) {
		lwkt_reltoken(&pit->token);
		return ENXIO;
	}
	if (port == VMM_X64_PIT_PORT_CONTROL - VMM_X64_PIT_PORT_CHANNEL0) {
		index = value >> 6;
		if (index == 3) {
			for (index = 0; index < VMM_X64_PIT_CHANNELS; ++index) {
				if ((value & (1U << (index + 1))) != 0)
					continue;
				channel = &pit->state.channels[index];
				if ((value & 0x20U) == 0)
					vmm_x64_pit_latch_count(channel, now);
				if ((value & 0x10U) == 0) {
					channel->status =
					    vmm_x64_pit_status(channel, now);
					channel->status_latched = 1;
				}
			}
		} else {
			channel = &pit->state.channels[index];
			if ((value & 0x30U) == 0) {
				vmm_x64_pit_latch_count(channel, now);
			} else {
				channel->rw_mode = (value >> 4) & 3U;
				channel->mode = (value >> 1) & 7U;
				if (channel->mode == 6)
					channel->mode = 2;
				else if (channel->mode == 7)
					channel->mode = 3;
				channel->bcd = value & 1U;
				channel->read_state = 0;
				channel->write_state = 0;
				channel->count_latched = 0;
				channel->status_latched = 0;
			}
		}
		vmm_x64_pit_arm(pit, now);
		lwkt_reltoken(&pit->token);
		return 0;
	}
	if (port >= VMM_X64_PIT_CHANNELS) {
		lwkt_reltoken(&pit->token);
		return EOPNOTSUPP;
	}
	channel = &pit->state.channels[port];
	switch (channel->rw_mode) {
	case 1:
		vmm_x64_pit_load_count(channel, value, now);
		break;
	case 2:
		vmm_x64_pit_load_count(channel, (uint16_t)value << 8, now);
		break;
	case 3:
		if (channel->write_state == 0) {
			channel->write_latch = value;
			channel->write_state = 1;
		} else {
			vmm_x64_pit_load_count(channel,
			    channel->write_latch | ((uint16_t)value << 8), now);
		}
		break;
	default:
		break;
	}
	vmm_x64_pit_arm(pit, now);
	lwkt_reltoken(&pit->token);
	return 0;
}
