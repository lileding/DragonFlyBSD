/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reusable x86-64 8259 PIC state machine.
 */
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include "../vmm_backend.h"
#include "../vmm_machine.h"
#include "vmm_x64.h"
#include "vmm_x64_pic.h"

#define VMM_X64_PIC_MASTER_COMMAND	0x20U
#define VMM_X64_PIC_MASTER_DATA		0x21U
#define VMM_X64_PIC_SLAVE_COMMAND	0xa0U
#define VMM_X64_PIC_SLAVE_DATA		0xa1U
#define VMM_X64_PIC_MASTER_ELCR		0x4d0U
#define VMM_X64_PIC_SLAVE_ELCR		0x4d1U

#define VMM_X64_PIC_ICW1_INIT		0x10U
#define VMM_X64_PIC_ICW1_ICW4		0x01U
#define VMM_X64_PIC_ICW4_AUTO_EOI	0x02U
#define VMM_X64_PIC_ICW4_SFNM		0x10U
#define VMM_X64_PIC_OCW2_EOI		0x20U
#define VMM_X64_PIC_OCW2_SPECIFIC	0x40U
#define VMM_X64_PIC_OCW2_ROTATE		0x80U
#define VMM_X64_PIC_OCW3_SELECT		0x08U
#define VMM_X64_PIC_OCW3_POLL		0x04U
#define VMM_X64_PIC_OCW3_READ_ISR	0x02U
#define VMM_X64_PIC_OCW3_SPECIAL	0x40U

struct vmm_x64_pic {
	/* token protects state. */
	struct lwkt_token token;
	struct vmm_pic_state state;
};

static bool vmm_x64_pic_state_valid(const struct vmm_pic_state *);
static int vmm_x64_pic_chip_rank(const struct vmm_pic_chip_state *, int);
static int vmm_x64_pic_chip_pending(const struct vmm_pic_chip_state *);
static int vmm_x64_pic_chip_in_service(const struct vmm_pic_chip_state *);
static int vmm_x64_pic_chip_next(const struct vmm_pic_chip_state *);
static void vmm_x64_pic_chip_ack(struct vmm_pic_chip_state *, int);
static void vmm_x64_pic_chip_eoi(struct vmm_pic_chip_state *, int, bool);
static void vmm_x64_pic_chip_reassert(struct vmm_pic_chip_state *);
static int vmm_x64_pic_select_locked(struct vmm_x64_pic *, int *, int *);
static int vmm_x64_pic_peek(struct vmm_x64_pic *);
static int vmm_x64_pic_ack_locked(struct vmm_x64_pic *);
static int vmm_x64_pic_pio_locked(struct vmm_x64_pic *,
	struct vmm_cpustate *, const struct vmm_cpuexit_io *, int *);
static int vmm_x64_pic_read_locked(struct vmm_x64_pic *, uint16_t,
	uint8_t *);
static int vmm_x64_pic_write_locked(struct vmm_x64_pic *, uint16_t,
	uint8_t, int *);
static int vmm_x64_pic_command_locked(struct vmm_x64_pic *,
	struct vmm_pic_chip_state *, uint8_t, int *);
static int vmm_x64_pic_data_locked(struct vmm_x64_pic *,
	struct vmm_pic_chip_state *, uint8_t, int *);

struct vmm_x64_pic *
vmm_x64_pic_alloc(void)
{
	struct vmm_x64_pic *pic;

	pic = kmalloc(sizeof(*pic), M_VMM, M_WAITOK | M_ZERO);
	if (pic == NULL)
		return NULL;
	lwkt_token_init(&pic->token, "vmmpic");
	pic->state.master.irq_base = 0x08;
	pic->state.master.imr = 0xff;
	pic->state.master.elcr_mask = 0xf8;
	pic->state.slave.irq_base = 0x70;
	pic->state.slave.imr = 0xff;
	pic->state.slave.elcr_mask = 0xde;
	return pic;
}

void
vmm_x64_pic_free(struct vmm_x64_pic *pic)
{

	if (pic != NULL)
		kfree(pic, M_VMM);
}

void
vmm_x64_pic_destroy(struct vmm_machine *machine)
{
	struct vmm_x64_pic *pic;

	if (machine == NULL)
		return;
	lwkt_gettoken(&machine->token);
	pic = machine->pic;
	machine->pic = NULL;
	lwkt_reltoken(&machine->token);
	vmm_x64_pic_free(pic);
}

int
vmm_x64_pic_get_state(struct vmm_machine *machine,
	struct vmm_pic_state *state)
{
	struct vmm_x64_pic *pic;

	if (machine == NULL || state == NULL)
		return EINVAL;
	lwkt_gettoken(&machine->token);
	if (machine->destroying || machine->pic == NULL) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	pic = machine->pic;
	lwkt_gettoken(&pic->token);
	*state = pic->state;
	lwkt_reltoken(&pic->token);
	lwkt_reltoken(&machine->token);
	return 0;
}

int
vmm_x64_pic_set_state(struct vmm_machine *machine,
	const struct vmm_pic_state *state)
{
	struct vmm_x64_pic *pic;
	int vector;

	if (machine == NULL || !vmm_x64_pic_state_valid(state))
		return EINVAL;
	lwkt_gettoken(&machine->token);
	if (machine->destroying || machine->pic == NULL) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	pic = machine->pic;
	lwkt_gettoken(&pic->token);
	pic->state = *state;
	vector = vmm_x64_pic_peek(pic);
	lwkt_reltoken(&pic->token);
	if (vector >= 0)
		(void)machine->backend->machine_raise_legacy(machine,
		    (uint8_t)vector);
	lwkt_reltoken(&machine->token);
	return 0;
}

int
vmm_x64_pic_set_irq_locked(struct vmm_machine *machine, uint32_t gsi,
	bool level, int *vector)
{
	struct vmm_x64_pic *pic;
	struct vmm_pic_chip_state *chip;
	uint8_t bit;

	if (machine == NULL || vector == NULL || gsi >= 16)
		return EINVAL;
	*vector = -1;
	pic = machine->pic;
	if (pic == NULL)
		return ENXIO;
	chip = gsi < 8 ? &pic->state.master : &pic->state.slave;
	bit = 1U << (gsi & 7);
	lwkt_gettoken(&pic->token);
	if (level) {
		if ((chip->last_irr & bit) == 0) {
			chip->last_irr |= bit;
			chip->irr |= bit;
		}
	} else {
		chip->last_irr &= ~bit;
	}
	*vector = vmm_x64_pic_peek(pic);
	lwkt_reltoken(&pic->token);
	return 0;
}

int
vmm_x64_pic_peek_locked(struct vmm_machine *machine, uint8_t *vector)
{
	struct vmm_x64_pic *pic;
	int value;

	if (machine == NULL || vector == NULL || machine->pic == NULL)
		return ENOENT;
	pic = machine->pic;
	lwkt_gettoken(&pic->token);
	value = vmm_x64_pic_peek(pic);
	lwkt_reltoken(&pic->token);
	if (value < 0)
		return ENOENT;
	*vector = (uint8_t)value;
	return 0;
}

int
vmm_x64_pic_accept_locked(struct vmm_machine *machine, uint8_t *vector)
{
	struct vmm_x64_pic *pic;
	int value;

	if (machine == NULL || vector == NULL || machine->pic == NULL)
		return ENOENT;
	pic = machine->pic;
	lwkt_gettoken(&pic->token);
	value = vmm_x64_pic_ack_locked(pic);
	lwkt_reltoken(&pic->token);
	if (value < 0)
		return ENOENT;
	*vector = (uint8_t)value;
	return 0;
}

int
vmm_x64_pic_io(struct vmm_machine *machine, struct vmm_cpustate *state,
	const struct vmm_cpuexit_io *exit)
{
	struct vmm_x64_pic *pic;
	int error;
	int vector;

	if (machine == NULL || state == NULL || exit == NULL)
		return EINVAL;
	lwkt_gettoken(&machine->token);
	if (machine->destroying) {
		lwkt_reltoken(&machine->token);
		return ENXIO;
	}
	pic = machine->pic;
	if (pic == NULL) {
		lwkt_reltoken(&machine->token);
		return ENOENT;
	}
	lwkt_gettoken(&pic->token);
	error = vmm_x64_pic_pio_locked(pic, state, exit, &vector);
	lwkt_reltoken(&pic->token);
	if (error == 0 && vector >= 0)
		error = machine->backend->machine_raise_legacy(machine,
		    (uint8_t)vector);
	lwkt_reltoken(&machine->token);
	return error;
}

static bool
vmm_x64_pic_state_valid(const struct vmm_pic_state *state)
{
	const struct vmm_pic_chip_state *chips[2];
	unsigned int index;

	if (state == NULL)
		return false;
	chips[0] = &state->master;
	chips[1] = &state->slave;
	for (index = 0; index < nitems(chips); ++index) {
		if (chips[index]->priority_add > 7 ||
		    (chips[index]->irq_base & 7) != 0 ||
		    chips[index]->read_reg_select > 1 ||
		    chips[index]->poll > 1 || chips[index]->special_mask > 1 ||
		    chips[index]->init_state > 3 || chips[index]->auto_eoi > 1 ||
		    chips[index]->rotate_on_auto_eoi > 1 ||
		    chips[index]->special_fully_nested_mode > 1 ||
		    chips[index]->init4 > 1 ||
		    (chips[index]->elcr & ~chips[index]->elcr_mask) != 0)
			return false;
	}
	return true;
}

static int
vmm_x64_pic_chip_rank(const struct vmm_pic_chip_state *chip, int irq)
{

	return (irq - chip->priority_add + 8) & 7;
}

static int
vmm_x64_pic_chip_pending(const struct vmm_pic_chip_state *chip)
{
	uint8_t pending;
	int offset;
	int irq;

	pending = chip->irr & ~chip->imr;
	for (offset = 0; offset < 8; ++offset) {
		irq = (chip->priority_add + offset) & 7;
		if ((pending & (1U << irq)) != 0)
			return irq;
	}
	return -1;
}

static int
vmm_x64_pic_chip_in_service(const struct vmm_pic_chip_state *chip)
{
	uint8_t in_service;
	int offset;
	int irq;

	in_service = chip->isr;
	if (chip->special_mask)
		in_service &= ~chip->imr;
	for (offset = 0; offset < 8; ++offset) {
		irq = (chip->priority_add + offset) & 7;
		if ((in_service & (1U << irq)) != 0)
			return irq;
	}
	return -1;
}

static int
vmm_x64_pic_chip_next(const struct vmm_pic_chip_state *chip)
{
	int pending;
	int in_service;

	pending = vmm_x64_pic_chip_pending(chip);
	if (pending < 0)
		return -1;
	in_service = vmm_x64_pic_chip_in_service(chip);
	if (in_service >= 0 && vmm_x64_pic_chip_rank(chip, pending) >=
	    vmm_x64_pic_chip_rank(chip, in_service))
		return -1;
	return pending;
}

static void
vmm_x64_pic_chip_ack(struct vmm_pic_chip_state *chip, int irq)
{

	chip->irr &= ~(1U << irq);
	if (chip->auto_eoi) {
		if (chip->rotate_on_auto_eoi)
			chip->priority_add = (irq + 1) & 7;
	} else {
		chip->isr |= 1U << irq;
	}
}

static void
vmm_x64_pic_chip_eoi(struct vmm_pic_chip_state *chip, int irq, bool rotate)
{

	if (irq < 0)
		irq = vmm_x64_pic_chip_in_service(chip);
	if (irq < 0)
		return;
	chip->isr &= ~(1U << irq);
	if (rotate)
		chip->priority_add = (irq + 1) & 7;
	vmm_x64_pic_chip_reassert(chip);
}

static void
vmm_x64_pic_chip_reassert(struct vmm_pic_chip_state *chip)
{

	chip->irr |= chip->last_irr & chip->elcr;
}

static int
vmm_x64_pic_select_locked(struct vmm_x64_pic *pic, int *master_irq,
    int *slave_irq)
{
	struct vmm_pic_chip_state *master;
	struct vmm_pic_chip_state *slave;

	master = &pic->state.master;
	slave = &pic->state.slave;
	if (master->init_state != 0 || slave->init_state != 0)
		return -1;
	*slave_irq = vmm_x64_pic_chip_next(slave);
	if (*slave_irq >= 0)
		master->irr |= 1U << 2;
	*master_irq = vmm_x64_pic_chip_next(master);
	if (*master_irq < 0)
		return -1;
	if (*master_irq != 2 || *slave_irq < 0)
		return master->irq_base + *master_irq;
	return slave->irq_base + *slave_irq;
}

static int
vmm_x64_pic_peek(struct vmm_x64_pic *pic)
{
	int master_irq;
	int slave_irq;

	return vmm_x64_pic_select_locked(pic, &master_irq, &slave_irq);
}

static int
vmm_x64_pic_ack_locked(struct vmm_x64_pic *pic)
{
	struct vmm_pic_chip_state *master;
	struct vmm_pic_chip_state *slave;
	int master_irq;
	int slave_irq;
	int vector;

	vector = vmm_x64_pic_select_locked(pic, &master_irq, &slave_irq);
	if (vector < 0)
		return vector;
	master = &pic->state.master;
	slave = &pic->state.slave;
	vmm_x64_pic_chip_ack(master, master_irq);
	if (master_irq == 2 && slave_irq >= 0)
		vmm_x64_pic_chip_ack(slave, slave_irq);
	return vector;
}

static int
vmm_x64_pic_pio_locked(struct vmm_x64_pic *pic,
	struct vmm_cpustate *state, const struct vmm_cpuexit_io *exit,
	int *vector)
{
	uint8_t value;
	int error;

	if (exit->port != VMM_X64_PIC_MASTER_COMMAND &&
	    exit->port != VMM_X64_PIC_MASTER_DATA &&
	    exit->port != VMM_X64_PIC_SLAVE_COMMAND &&
	    exit->port != VMM_X64_PIC_SLAVE_DATA &&
	    exit->port != VMM_X64_PIC_MASTER_ELCR &&
	    exit->port != VMM_X64_PIC_SLAVE_ELCR)
		return ENOENT;
	if (exit->str || exit->rep || exit->operand_size != 1)
		return EOPNOTSUPP;
	*vector = -1;
	if (exit->in) {
		error = vmm_x64_pic_read_locked(pic, exit->port, &value);
		if (error != 0)
			return error;
		state->gprs[VMM_X64_GPR_RAX] =
		    (state->gprs[VMM_X64_GPR_RAX] & ~0xffULL) | value;
	} else {
		value = state->gprs[VMM_X64_GPR_RAX] & 0xffU;
		error = vmm_x64_pic_write_locked(pic, exit->port, value, vector);
		if (error != 0)
			return error;
	}
	state->gprs[VMM_X64_GPR_RIP] = exit->npc;
	return 0;
}

static int
vmm_x64_pic_read_locked(struct vmm_x64_pic *pic, uint16_t port,
	uint8_t *value)
{
	struct vmm_pic_chip_state *chip;
	int vector;

	switch (port) {
	case VMM_X64_PIC_MASTER_COMMAND:
		chip = &pic->state.master;
		break;
	case VMM_X64_PIC_SLAVE_COMMAND:
		chip = &pic->state.slave;
		break;
	case VMM_X64_PIC_MASTER_DATA:
		*value = pic->state.master.imr;
		return 0;
	case VMM_X64_PIC_SLAVE_DATA:
		*value = pic->state.slave.imr;
		return 0;
	case VMM_X64_PIC_MASTER_ELCR:
		*value = pic->state.master.elcr;
		return 0;
	case VMM_X64_PIC_SLAVE_ELCR:
		*value = pic->state.slave.elcr;
		return 0;
	default:
		return ENOENT;
	}
	if (chip->poll) {
		chip->poll = 0;
		vector = vmm_x64_pic_ack_locked(pic);
		if (vector < 0)
			*value = 0;
		else if (chip == &pic->state.master &&
		    vector >= pic->state.slave.irq_base &&
		    vector < pic->state.slave.irq_base + 8)
			*value = 0x82;
		else
			*value = 0x80 | ((vector - chip->irq_base) & 7);
		return 0;
	}
	*value = chip->read_reg_select ? chip->isr : chip->irr;
	return 0;
}

static int
vmm_x64_pic_write_locked(struct vmm_x64_pic *pic, uint16_t port,
	uint8_t value, int *vector)
{
	switch (port) {
	case VMM_X64_PIC_MASTER_COMMAND:
		return vmm_x64_pic_command_locked(pic, &pic->state.master, value,
		    vector);
	case VMM_X64_PIC_SLAVE_COMMAND:
		return vmm_x64_pic_command_locked(pic, &pic->state.slave, value,
		    vector);
	case VMM_X64_PIC_MASTER_DATA:
		return vmm_x64_pic_data_locked(pic, &pic->state.master, value,
		    vector);
	case VMM_X64_PIC_SLAVE_DATA:
		return vmm_x64_pic_data_locked(pic, &pic->state.slave, value,
		    vector);
	case VMM_X64_PIC_MASTER_ELCR:
		pic->state.master.elcr = value & pic->state.master.elcr_mask;
		*vector = vmm_x64_pic_peek(pic);
		return 0;
	case VMM_X64_PIC_SLAVE_ELCR:
		pic->state.slave.elcr = value & pic->state.slave.elcr_mask;
		*vector = vmm_x64_pic_peek(pic);
		return 0;
	default:
		return ENOENT;
	}
}

static int
vmm_x64_pic_command_locked(struct vmm_x64_pic *pic,
	struct vmm_pic_chip_state *chip, uint8_t value, int *vector)
{
	int irq;

	if ((value & VMM_X64_PIC_ICW1_INIT) != 0) {
		chip->last_irr = 0;
		chip->irr = 0;
		chip->imr = 0;
		chip->isr = 0;
		chip->priority_add = 0;
		chip->special_mask = 0;
		chip->init_state = 1;
		chip->auto_eoi = 0;
		chip->rotate_on_auto_eoi = 0;
		chip->special_fully_nested_mode = 0;
		chip->init4 = (value & VMM_X64_PIC_ICW1_ICW4) != 0;
		return 0;
	}
	if ((value & 0x18U) == VMM_X64_PIC_OCW3_SELECT) {
		if ((value & VMM_X64_PIC_OCW3_POLL) != 0)
			chip->poll = 1;
		if ((value & 0x03U) != 0)
			chip->read_reg_select =
			    (value & VMM_X64_PIC_OCW3_READ_ISR) != 0;
		if ((value & VMM_X64_PIC_OCW3_SPECIAL) != 0)
			chip->special_mask = (value & 0x20U) != 0;
		return 0;
	}
	if ((value & VMM_X64_PIC_OCW2_EOI) != 0) {
		irq = (value & VMM_X64_PIC_OCW2_SPECIFIC) != 0 ? value & 7 : -1;
		vmm_x64_pic_chip_eoi(chip, irq,
		    (value & VMM_X64_PIC_OCW2_ROTATE) != 0);
		*vector = vmm_x64_pic_peek(pic);
		return 0;
	}
	if ((value & (VMM_X64_PIC_OCW2_ROTATE | VMM_X64_PIC_OCW2_SPECIFIC)) ==
	    (VMM_X64_PIC_OCW2_ROTATE | VMM_X64_PIC_OCW2_SPECIFIC))
		chip->priority_add = ((value & 7) + 1) & 7;
	return 0;
}

static int
vmm_x64_pic_data_locked(struct vmm_x64_pic *pic,
	struct vmm_pic_chip_state *chip, uint8_t value, int *vector)
{
	switch (chip->init_state) {
	case 0:
		chip->imr = value;
		*vector = vmm_x64_pic_peek(pic);
		return 0;
	case 1:
		chip->irq_base = value & 0xf8U;
		chip->init_state = 2;
		return 0;
	case 2:
		chip->init_state = chip->init4 ? 3 : 0;
		return 0;
	case 3:
		chip->auto_eoi = (value & VMM_X64_PIC_ICW4_AUTO_EOI) != 0;
		chip->special_fully_nested_mode =
		    (value & VMM_X64_PIC_ICW4_SFNM) != 0;
		chip->init_state = 0;
		*vector = vmm_x64_pic_peek(pic);
		return 0;
	default:
		return EINVAL;
	}
}
