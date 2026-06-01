/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP_RUN_CPU_SEQUENCER (NV_VGPU_MSG_EVENT 4098) handler.
 *
 * GSP-RM sends a sequence of I/O commands the host must execute on its
 * behalf (PRI register reads/writes/polls, micro-sleeps, GSP-Falcon
 * core reset/start/halt-wait/resume). Each command is opCode(u32) +
 * payload[N u32].
 *
 * Wire format: rpc_run_cpu_sequencer_v17_00 (r570 nvrm/gsp.h):
 *   u32 bufferSizeDWord
 *   u32 cmdIndex           -- count of dwords (commands) in commandBuffer
 *   u32 regSaveArea[8]     -- written by REG_STORE for use by REG_POLL/MODIFY
 *   u32 commandBuffer[]    -- variable-length stream of (opCode, payload...)
 *
 * Reference: r535_gsp_msg_run_cpu_sequencer in linux/.../rm/r535/gsp.c
 * (r570 inherits r535's handler verbatim — same wire format).
 */

#include "nvkm_priv.h"
#include "nvkm_falcon.h"

#define NVKM_SEQ_OP_REG_WRITE		0
#define NVKM_SEQ_OP_REG_MODIFY		1
#define NVKM_SEQ_OP_REG_POLL		2
#define NVKM_SEQ_OP_DELAY_US		3
#define NVKM_SEQ_OP_REG_STORE		4
#define NVKM_SEQ_OP_CORE_RESET		5
#define NVKM_SEQ_OP_CORE_START		6
#define NVKM_SEQ_OP_CORE_WAIT_FOR_HALT	7
#define NVKM_SEQ_OP_CORE_RESUME		8

/* Payload sizes in dwords (NOT counting the opCode dword itself) -- matches
 * GSP_SEQUENCER_PAYLOAD_SIZE_DWORDS in nouveau. */
static const uint8_t nvkm_seq_payload_dw[] = {
	[NVKM_SEQ_OP_REG_WRITE]		= 2,
	[NVKM_SEQ_OP_REG_MODIFY]	= 3,
	[NVKM_SEQ_OP_REG_POLL]		= 5,
	[NVKM_SEQ_OP_DELAY_US]		= 1,
	[NVKM_SEQ_OP_REG_STORE]		= 2,
	[NVKM_SEQ_OP_CORE_RESET]	= 0,
	[NVKM_SEQ_OP_CORE_START]	= 0,
	[NVKM_SEQ_OP_CORE_WAIT_FOR_HALT]= 0,
	[NVKM_SEQ_OP_CORE_RESUME]	= 0,
};

int
nvkm_gsp_seq_msg_handler(void *priv, uint32_t fn, void *repv, uint32_t repc)
{
	struct nvkm_softc *sc = priv;
	const uint8_t *payload = repv;
	uint32_t payload_size = repc;
	(void)fn;

	const uint32_t *p;
	uint32_t buf_size_dw, cmd_index;
	uint32_t *reg_save;
	const uint32_t *cmdbuf;
	uint32_t ptr = 0;
	uint32_t op_count = 0;

	if (payload_size < (2 + 8) * 4) {
		device_printf(sc->dev,
		    "seq: payload too small (%u bytes)\n", payload_size);
		return (0);
	}
	p = (const uint32_t *)payload;
	buf_size_dw = p[0];
	cmd_index   = p[1];
	/* regSaveArea[8] lives at p[2..9]; commandBuffer[] starts at p[10]. */
	reg_save = (uint32_t *)(uintptr_t)&p[2];
	cmdbuf   = &p[2 + 8];

	device_printf(sc->dev,
	    "seq: start (bufSizeDW=%u cmdIndex=%u)\n",
	    buf_size_dw, cmd_index);

	while (ptr < cmd_index) {
		uint32_t opcode;
		uint32_t pl_dw;

		opcode = cmdbuf[ptr++];
		if (opcode >= sizeof(nvkm_seq_payload_dw)) {
			device_printf(sc->dev,
			    "seq: unknown opcode %u at idx %u, stop\n",
			    opcode, ptr - 1);
			break;
		}
		pl_dw = nvkm_seq_payload_dw[opcode];
		if (ptr + pl_dw > cmd_index) {
			device_printf(sc->dev,
			    "seq: truncated cmd at idx %u (need %u, have %u)\n",
			    ptr - 1, pl_dw, cmd_index - ptr);
			break;
		}

		switch (opcode) {
		case NVKM_SEQ_OP_REG_WRITE: {
			uint32_t addr = cmdbuf[ptr];
			uint32_t val  = cmdbuf[ptr + 1];
			nvkm_wr32(sc, addr, val);
			break;
		}
		case NVKM_SEQ_OP_REG_MODIFY: {
			uint32_t addr = cmdbuf[ptr];
			uint32_t mask = cmdbuf[ptr + 1];
			uint32_t val  = cmdbuf[ptr + 2];
			uint32_t x    = nvkm_rd32(sc, addr);
			nvkm_wr32(sc, addr, (x & ~mask) | (val & mask));
			break;
		}
		case NVKM_SEQ_OP_REG_POLL: {
			uint32_t addr    = cmdbuf[ptr];
			uint32_t mask    = cmdbuf[ptr + 1];
			uint32_t val     = cmdbuf[ptr + 2];
			uint32_t timeout = cmdbuf[ptr + 3] ? cmdbuf[ptr + 3]
			                                  : 4000000u;
			uint32_t elapsed = 0;
			while (elapsed < timeout) {
				if ((nvkm_rd32(sc, addr) & mask) == val)
					break;
				DELAY(10);
				elapsed += 10;
			}
			if (elapsed >= timeout)
				device_printf(sc->dev,
				    "seq: poll timeout on 0x%06x\n", addr);
			break;
		}
		case NVKM_SEQ_OP_DELAY_US: {
			uint32_t usec = cmdbuf[ptr];
			DELAY(usec);
			break;
		}
		case NVKM_SEQ_OP_REG_STORE: {
			uint32_t addr = cmdbuf[ptr];
			uint32_t slot = cmdbuf[ptr + 1];
			if (slot < 8)
				reg_save[slot] = nvkm_rd32(sc, addr);
			break;
		}
		case NVKM_SEQ_OP_CORE_RESET:
			if (sc->gsp != NULL)
				(void)nvkm_falcon_reset_eng(sc->gsp);
			{
				uint32_t v = nvkm_rd32(sc,
				    NVKM_TU102_GSP_BASE + 0x624);
				nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x624,
				    v | 0x80);
				nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x10c, 0);
			}
			break;
		case NVKM_SEQ_OP_CORE_START: {
			uint32_t v = nvkm_rd32(sc,
			    NVKM_TU102_GSP_BASE + 0x100);
			if (v & 0x40)
				nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x130, 2);
			else
				nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x100, 2);
			break;
		}
		case NVKM_SEQ_OP_CORE_WAIT_FOR_HALT: {
			int n;
			for (n = 0; n < 200; n++) {
				if (nvkm_rd32(sc, NVKM_TU102_GSP_BASE + 0x100)
				    & 0x10)
					break;
				DELAY(10000);	/* 10 ms */
			}
			if (n >= 200)
				device_printf(sc->dev,
				    "seq: core wait-halt timeout\n");
			break;
		}
		case NVKM_SEQ_OP_CORE_RESUME: {
			/* Strictly per nouveau r535_gsp_msg_run_cpu_sequencer
			 * CORE_RESUME case (rm/r535/gsp.c:1092):
			 *   1. reset GSP-Falcon
			 *   2. write libos.addr to GSP-Falcon MB0/1
			 *   3. start SEC2 (CPUCTL=2)
			 *   4. poll global 0x1180f8 bit 26 (0x04000000) for 2s
			 *   5. read SEC2 MB0, expect 0
			 *   6. write FALCON_OS (0x080) = app_version (=0 for us)
			 *   7. verify RISC-V active
			 */
			device_printf(sc->dev, "seq: CORE_RESUME\n");
			if (sc->gsp != NULL)
				(void)nvkm_falcon_reset_eng(sc->gsp);
			{
				uint64_t la = sc->gsp_libos.paddr;
				nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x040,
				    (uint32_t)la);
				nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x044,
				    (uint32_t)(la >> 32));
			}
			device_printf(sc->dev,
			    "seq: SEC2 pre-kick CPUCTL=0x%x DMACTL=0x%x MB0=0x%x BOOTVEC=0x%x SCRATCH14=0x%x\n",
			    nvkm_rd32(sc, 0x840100), nvkm_rd32(sc, 0x84010c),
			    nvkm_rd32(sc, 0x840040), nvkm_rd32(sc, 0x840104),
			    nvkm_rd32(sc, 0x1180f8));
			if (sc->sec2 != NULL)
				nvkm_falcon_start(sc->sec2);
			DELAY(100);
			device_printf(sc->dev,
			    "seq: SEC2 post-kick CPUCTL=0x%x DMACTL=0x%x MB0=0x%x SCRATCH14=0x%x\n",
			    nvkm_rd32(sc, 0x840100), nvkm_rd32(sc, 0x84010c),
			    nvkm_rd32(sc, 0x840040), nvkm_rd32(sc, 0x1180f8));
			{
				int n;
				for (n = 0; n < 200; n++) {
					if (nvkm_rd32(sc, 0x1180f8) &
					    0x04000000u)
						break;
					DELAY(10000);
				}
				if (n >= 200) {
					device_printf(sc->dev,
					    "seq: CORE_RESUME timeout waiting for SEC2\n");
					return (0);
				}
			}
			{
				uint32_t sec2_mb0 = nvkm_rd32(sc,
				    0x840040);
				if (sec2_mb0 != 0) {
					device_printf(sc->dev,
					    "seq: CORE_RESUME SEC2 MB0=0x%x\n",
					    sec2_mb0);
					return (0);
				}
			}
			nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0x080, 0);
			{
				uint32_t rsv = nvkm_rd32(sc, 0x111240);
				if (!(rsv & 1)) {
					device_printf(sc->dev,
					    "seq: CORE_RESUME failed (RISCV_STATUS=0x%x)\n",
					    rsv);
					return (0);
				}
			}
			device_printf(sc->dev,
			    "seq: CORE_RESUME ok, RISC-V active again\n");
			break;
		}
		}
		ptr += pl_dw;
		op_count++;
	}
	device_printf(sc->dev, "seq: done (%u ops processed)\n", op_count);
	return (0);
}
