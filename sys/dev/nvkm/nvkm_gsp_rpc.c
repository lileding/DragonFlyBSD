/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * GSP-RM RPC mechanism (r570; uses r535-shared layout).
 *
 * Mirrors nouveau drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c.
 * r570 inherits this verbatim via .rpc = &r535_rpc in r570/rm.c.
 *
 * Layering:
 *   public API (declared in nvkm_priv.h):
 *     nvkm_gsp_rpc_get(fn, argc)     — alloc buffer, init headers, return params ptr
 *     nvkm_gsp_rpc_push(params, policy, repc) — send + handle reply per policy
 *     nvkm_gsp_rpc_done(buf)         — kfree the buffer
 *     nvkm_gsp_msg_ntfy_add(fn, h, p) — register event handler
 *   inline convenience helpers (in nvkm_priv.h):
 *     nvkm_gsp_rpc_rd(fn, argc) = get + push(RECV, argc)
 *     nvkm_gsp_rpc_wr(params, policy) = push(policy, 0) + done
 *
 * Buffer lifecycle (matches nouveau):
 *   [r535_gsp_msg outer hdr 48B][nvfw_gsp_rpc inner hdr 32B][params argc B]
 *   rpc_get   → alloc + init hdrs, return ptr to params (offset 80)
 *   caller    → fills params
 *   rpc_push  → cmdq_push (consumes/frees the buffer), then handle_reply
 *   for RECV: handle_reply → msg_recv allocates NEW buffer, copies reply,
 *             returns params ptr of reply buffer
 *   rpc_done → kfree the reply buffer
 *
 * Doorbell decision: rung iff sc->gsp_running is true (i.e. GSP_INIT_DONE
 * already seen). Pre-init RPCs sit in cmdq and GSP-RM polls them itself.
 */

#include "nvkm_priv.h"
#include "nvkm_falcon.h"
#include "nvkm_gsp_abi.h"

#include <sys/libkern.h>
#include <bus/pci/pcireg.h>
#include <bus/pci/pcivar.h>

#define NVKM_GSP_PAGE_SIZE	4096u
#define NVKM_GSP_MSG_HDR_SIZE	48		/* r535_gsp_msg outer */
#define NVKM_GSP_RPC_HDR_SIZE	32		/* nvfw_gsp_rpc inner */
#define NVKM_GSP_HDR_TOTAL	(NVKM_GSP_MSG_HDR_SIZE + NVKM_GSP_RPC_HDR_SIZE)
#define NVKM_GSP_MSGCOUNT	63u		/* fixed: (256K - 4K) / 4K */
#define NVKM_GSP_MAX_PAYLOAD	(NVKM_GSP_PAGE_SIZE - NVKM_GSP_HDR_TOTAL)
#define NVKM_GSP_SIGNATURE	0x43505256u	/* 'C''P''R''V' LE */

struct nvkm_gsp_msg_env {
	uint8_t  auth_tag_buffer[16];
	uint8_t  aad_buffer[16];
	uint32_t checksum;
	uint32_t sequence;
	uint32_t elem_count;
	uint32_t pad;
	uint8_t  data[];
} __packed;

struct nvkm_nvfw_gsp_rpc {
	uint32_t header_version;
	uint32_t signature;
	uint32_t length;
	uint32_t function;
	uint32_t rpc_result;
	uint32_t rpc_result_private;
	uint32_t sequence;
	uint32_t spare;
	uint8_t  data[];
} __packed;

/* container_of-style: from params ptr (after the two headers) walk back. */
static inline struct nvkm_nvfw_gsp_rpc *
params_to_rpc(void *params)
{
	return (struct nvkm_nvfw_gsp_rpc *)((uint8_t *)params -
	    NVKM_GSP_RPC_HDR_SIZE);
}
static inline struct nvkm_gsp_msg_env *
rpc_to_msg(struct nvkm_nvfw_gsp_rpc *rpc)
{
	return (struct nvkm_gsp_msg_env *)((uint8_t *)rpc -
	    NVKM_GSP_MSG_HDR_SIZE);
}

/* ===================================================================
 * Notify (event handler) registry. Fixed-size table.
 * =================================================================== */

void
nvkm_gsp_msg_ntfy_init(struct nvkm_softc *sc)
{
	sc->gsp_ntfy.cnt = 0;
}

int
nvkm_gsp_msg_ntfy_add(struct nvkm_softc *sc, uint32_t fn,
    nvkm_gsp_msg_ntfy_func handler, void *priv)
{
	uint32_t i;

	if (sc->gsp_ntfy.cnt >= NVKM_GSP_NTFY_MAX)
		return (ENOSPC);
	for (i = 0; i < sc->gsp_ntfy.cnt; i++) {
		if (sc->gsp_ntfy.tab[i].fn == fn)
			return (EEXIST);
	}
	sc->gsp_ntfy.tab[sc->gsp_ntfy.cnt].fn   = fn;
	sc->gsp_ntfy.tab[sc->gsp_ntfy.cnt].func = handler;
	sc->gsp_ntfy.tab[sc->gsp_ntfy.cnt].priv = priv;
	sc->gsp_ntfy.cnt++;
	return (0);
}

static int
nvkm_gsp_msg_handle(struct nvkm_softc *sc, uint32_t fn,
    void *repv, uint32_t repc)
{
	uint32_t i;

	for (i = 0; i < sc->gsp_ntfy.cnt; i++) {
		if (sc->gsp_ntfy.tab[i].fn != fn)
			continue;
		if (sc->gsp_ntfy.tab[i].func == NULL)
			return (0);	/* stub: drain silently */
		return sc->gsp_ntfy.tab[i].func(
		    sc->gsp_ntfy.tab[i].priv, fn, repv, repc);
	}
	device_printf(sc->dev, "gsp_rpc: unhandled event fn=0x%x len=%u\n",
	    fn, repc);
	return (0);
}

/* ===================================================================
 * Layer 1: cmdq write + msgq read primitives.
 * =================================================================== */

static int
nvkm_gsp_cmdq_push(struct nvkm_softc *sc, void *params)
{
	struct nvkm_nvfw_gsp_rpc *rpc = params_to_rpc(params);
	struct nvkm_gsp_msg_env *msg = rpc_to_msg(rpc);
	uint8_t *cmdq, *msgq;
	uint32_t rpc_len, hdr_total, padded;
	uint32_t wptr, rptr, free_slots;
	uint64_t csum;
	const uint64_t *cp;
	uint32_t nu64, i;
	int retries;

	if (sc->gsp_shm.kva == NULL) {
		kfree(msg, M_TEMP);
		return (ENXIO);
	}

	rpc_len   = rpc->length;	/* hdr + payload */
	hdr_total = sizeof(*msg) + rpc_len;
	padded    = roundup(hdr_total, NVKM_GSP_PAGE_SIZE);
	if (padded > NVKM_GSP_PAGE_SIZE) {
		device_printf(sc->dev,
		    "cmdq_push: fn=%u len=%u exceeds single page (no continuation yet)\n",
		    rpc->function, rpc_len);
		kfree(msg, M_TEMP);
		return (EINVAL);
	}

	msg->sequence   = sc->gsp_cmdq_seq++;
	msg->elem_count = padded / NVKM_GSP_PAGE_SIZE;
	msg->checksum   = 0;
	msg->pad        = 0;

	nu64 = padded / sizeof(uint64_t);
	cp = (const uint64_t *)msg;
	csum = 0;
	for (i = 0; i < nu64; i++)
		csum ^= cp[i];
	msg->checksum = (uint32_t)(csum >> 32) ^ (uint32_t)(csum & 0xffffffffu);

	cmdq = (uint8_t *)sc->gsp_shm.kva + sc->gsp_shm_cmdq_off;
	msgq = (uint8_t *)sc->gsp_shm.kva + sc->gsp_shm_msgq_off;

	wptr = *(volatile uint32_t *)(cmdq + 0x10);
	for (retries = 1000; retries > 0; retries--) {
		/* GSP's cmdq read cursor lives in msgq.rx (nouveau crossover) */
		rptr = *(volatile uint32_t *)(msgq + 32);
		free_slots = rptr + NVKM_GSP_MSGCOUNT - wptr - 1;
		if (free_slots >= NVKM_GSP_MSGCOUNT)
			free_slots -= NVKM_GSP_MSGCOUNT;
		if (free_slots >= 1)
			break;
		DELAY(10);
	}
	if (retries == 0) {
		device_printf(sc->dev,
		    "cmdq_push: timeout waiting for slot\n");
		kfree(msg, M_TEMP);
		return (ETIMEDOUT);
	}

	memcpy(cmdq + NVKM_GSP_PAGE_SIZE + wptr * NVKM_GSP_PAGE_SIZE,
	    msg, padded);

	wptr += msg->elem_count;
	if (wptr >= NVKM_GSP_MSGCOUNT)
		wptr -= NVKM_GSP_MSGCOUNT;
	*(volatile uint32_t *)(cmdq + 0x10) = wptr;

	/* Doorbell iff GSP-RM has come up and is event-driven. */
	if (sc->gsp_running)
		nvkm_wr32(sc, NVKM_TU102_GSP_BASE + 0xc00, 0);

#ifdef NVKM_DEBUG_RPC_TRACE
	device_printf(sc->dev,
	    "cmdq_push: fn=%u len=%u wptr=%u seq=%u ring=%d\n",
	    rpc->function, rpc_len, wptr, msg->sequence, sc->gsp_running);
#endif
	kfree(msg, M_TEMP);
	return (0);
}

/*
 * Read one msgq slot into a fresh kmalloc buffer (sized to fit at least
 * gsp_rpc_len bytes). Returns kvbuf on success (caller frees via
 * nvkm_gsp_rpc_done), NULL if no message available, or ERR_PTR-style
 * negative-errno cast as void *.
 *
 * For the common single-element case this is just a memcpy from the
 * cmdq slot. Multi-page continuation is not yet supported; we'll
 * extend later.
 */
static void *
nvkm_gsp_msgq_recv_one_elem(struct nvkm_softc *sc, uint32_t want_len,
    uint32_t *out_fn, uint32_t *out_len)
{
	uint8_t *msgq, *slot;
	struct nvkm_nvfw_gsp_rpc *rpc;
	uint32_t wptr;
	uint32_t fn, len, sig;
	uint32_t alloc_sz;
	uint8_t *buf;

	msgq = (uint8_t *)sc->gsp_shm.kva + sc->gsp_shm_msgq_off;
	wptr = *(volatile uint32_t *)(msgq + 0x10);
	if (sc->gsp_msgq_rptr == wptr)
		return (NULL);

	slot = msgq + NVKM_GSP_PAGE_SIZE +
	    sc->gsp_msgq_rptr * NVKM_GSP_PAGE_SIZE;
	rpc = (struct nvkm_nvfw_gsp_rpc *)(slot + NVKM_GSP_MSG_HDR_SIZE);
	sig = rpc->signature;
	len = rpc->length;
	fn  = rpc->function;

	if (sig != NVKM_GSP_SIGNATURE) {
		device_printf(sc->dev,
		    "msgq[%u]: bad signature 0x%08x (slot=%p) - skipping\n",
		    sc->gsp_msgq_rptr, sig, slot);
		/* Dump first 96 bytes of slot to identify format. */
		for (int i = 0; i < 96; i += 16) {
			device_printf(sc->dev,
			    "  slot[+%02d]: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
			    i,
			    slot[i+0], slot[i+1], slot[i+2], slot[i+3],
			    slot[i+4], slot[i+5], slot[i+6], slot[i+7],
			    slot[i+8], slot[i+9], slot[i+10], slot[i+11],
			    slot[i+12], slot[i+13], slot[i+14], slot[i+15]);
		}
		/* Skip slot. Per r535: advance + mfence before publishing rptr. */
		sc->gsp_msgq_rptr = (sc->gsp_msgq_rptr + 1) % NVKM_GSP_MSGCOUNT;
		cpu_mfence();
		{
			uint8_t *cmdq = (uint8_t *)sc->gsp_shm.kva + sc->gsp_shm_cmdq_off;
			*(volatile uint32_t *)(cmdq + 32) = sc->gsp_msgq_rptr;
		}
		return (NULL);
	}

	/* Allocate buffer sized max(rpc->length, want_len) so caller can
	 * see at least the requested length even if reply is short. */
	alloc_sz = (len > want_len) ? len : want_len;
	if (alloc_sz < NVKM_GSP_RPC_HDR_SIZE)
		alloc_sz = NVKM_GSP_RPC_HDR_SIZE;
	buf = kmalloc(alloc_sz, M_TEMP, M_WAITOK | M_ZERO);
	memcpy(buf, rpc, (len > alloc_sz) ? alloc_sz : len);

	/* Per nouveau r535_gsp_msgq_recv_one_elem: page count comes from
	 * DIV_ROUND_UP(GSP_MSG_HDR_SIZE + rpc->length, GSP_PAGE_SIZE), NOT
	 * from elemCount header field. Clamp to 16 (GSP_MSG_MAX_SIZE / PAGE)
	 * to defend against corrupt rpc->length. */
	uint32_t total_bytes = NVKM_GSP_MSG_HDR_SIZE + len;
	uint32_t pages = (total_bytes + NVKM_GSP_PAGE_SIZE - 1) /
	    NVKM_GSP_PAGE_SIZE;
	if (pages == 0) pages = 1;
	if (pages > 16) pages = 16;
	sc->gsp_msgq_rptr = (sc->gsp_msgq_rptr + pages) % NVKM_GSP_MSGCOUNT;
	cpu_mfence();
	{
		uint8_t *cmdq = (uint8_t *)sc->gsp_shm.kva +
		    sc->gsp_shm_cmdq_off;
		*(volatile uint32_t *)(cmdq + 32) = sc->gsp_msgq_rptr;
	}

	if (out_fn)  *out_fn = fn;
	if (out_len) *out_len = len;
	return (buf);
}

/* Drain the msgq while gsp_tok is held. For each message:
 *   - Function < 0x1000 + matches a pending->seq -> set pending->done + wakeup.
 *   - Otherwise treat as an unsolicited event and dispatch via ntfy table.
 *
 * The pending->reply_buf takes ownership of the kmalloc'd buffer returned
 * by nvkm_gsp_msgq_recv_one_elem.
 */
static void
nvkm_gsp_msgq_drain_locked(struct nvkm_softc *sc)
{
	for (;;) {
		uint32_t fn = 0, len = 0;
		void *buf = nvkm_gsp_msgq_recv_one_elem(sc, 0, &fn, &len);
		if (buf == NULL)
			return;

		if (fn < 0x1000) {
			struct nvkm_nvfw_gsp_rpc *r =
			    (struct nvkm_nvfw_gsp_rpc *)buf;
			struct nvkm_gsp_pending *p;
			bool matched = false;

			LIST_FOREACH(p, &sc->gsp_pending, link) {
				if (p->seq == r->sequence) {
					p->reply_buf = buf;
					p->reply_len = len;
					/* release: reply_buf/_len stores precede done
					 * publication on all architectures. */
					atomic_store_rel_int(&p->done, 1);
					wakeup(p);
					matched = true;
					break;
				}
			}
			if (!matched) {
				device_printf(sc->dev,
				    "gsp_rpc: stale reply fn=%u seq=%u (dropped)\n",
				    fn, r->sequence);
				kfree(buf, M_TEMP);
			}
			continue;
		}

		/* Event >= 0x1000: dispatch via ntfy table, then free. */
		{
			uint32_t plen = (len > NVKM_GSP_RPC_HDR_SIZE) ?
			    len - NVKM_GSP_RPC_HDR_SIZE : 0;
			uint8_t *params = (uint8_t *)buf + NVKM_GSP_RPC_HDR_SIZE;
			(void)nvkm_gsp_msg_handle(sc, fn, params, plen);
		}
		kfree(buf, M_TEMP);
	}
}

/* Public drain entry: takes gsp_tok and runs msgq_drain_locked. Safe to
 * call from any lwkt (ISR or ioctl). */
int
nvkm_gsp_msg_dispatch_all(struct nvkm_softc *sc)
{
	lwkt_gettoken(&sc->gsp_tok);
	nvkm_gsp_msgq_drain_locked(sc);
	lwkt_reltoken(&sc->gsp_tok);
	return (0);
}

/* ===================================================================
 * Layer 2/3: public RPC API.
 * =================================================================== */

/* Allocate request buffer (hdr+argc), init headers. Returns ptr to
 * params area (caller fills in argc bytes). */
void *
nvkm_gsp_rpc_get(struct nvkm_softc *sc, uint32_t fn, uint32_t argc)
{
	struct nvkm_gsp_msg_env *msg;
	struct nvkm_nvfw_gsp_rpc *rpc;
	uint32_t alloc_sz;

	if (argc > NVKM_GSP_MAX_PAYLOAD) {
		device_printf(sc->dev,
		    "rpc_get: fn=%u argc=%u too large (no continuation yet)\n",
		    fn, argc);
		return (NULL);
	}
	alloc_sz = roundup(NVKM_GSP_HDR_TOTAL + argc, NVKM_GSP_PAGE_SIZE);
	msg = kmalloc(alloc_sz, M_TEMP, M_WAITOK | M_ZERO);
	rpc = (struct nvkm_nvfw_gsp_rpc *)msg->data;

	rpc->header_version     = 0x03000000;
	rpc->signature          = NVKM_GSP_SIGNATURE;
	rpc->length             = NVKM_GSP_RPC_HDR_SIZE + argc;
	rpc->function           = fn;
	rpc->rpc_result         = 0xffffffffu;
	rpc->rpc_result_private = 0xffffffffu;
	rpc->sequence           = 0;
	rpc->spare              = 0;
	return (rpc->data);
}

/* Send the request buffer. Returns reply params ptr (for RECV) or
 * non-NULL sentinel (for NOWAIT/NOSEQ — buffer freed by cmdq_push), or
 * NULL on error. */
void *
nvkm_gsp_rpc_push(struct nvkm_softc *sc, void *params, int policy,
    uint32_t repc)
{
	struct nvkm_nvfw_gsp_rpc *rpc = params_to_rpc(params);
	uint32_t fn = rpc->function;
	int err;

	/* Assign inner sequence iff policy expects a reply. */
	if (policy != NVKM_GSP_RPC_REPLY_NOSEQ)
		rpc->sequence = sc->gsp_rpc_seq++;

	/* Wrap cmdq_push with gsp_tok so concurrent senders serialise. For
	 * REPLY_RECV we'll re-take the token below; lwkt tokens are
	 * refcount-recursive so this is safe. */
	lwkt_gettoken(&sc->gsp_tok);
	err = nvkm_gsp_cmdq_push(sc, params);	/* frees the buffer */
	lwkt_reltoken(&sc->gsp_tok);
	if (err != 0)
		return (NULL);

	switch (policy) {
	case NVKM_GSP_RPC_REPLY_NOWAIT:
	case NVKM_GSP_RPC_REPLY_NOSEQ:
		/* Caller passes NULL params in their rpc_wr wrapper.
		 * Return a non-NULL sentinel so the wrapper sees success.
		 * The pointer is never dereferenced. */
		return ((void *)(uintptr_t)1);

	case NVKM_GSP_RPC_REPLY_RECV: {
		struct nvkm_gsp_pending p = { .seq = rpc->sequence };
		int ticks_to_wait;
		int timeout_ticks = 5 * hz;

		/* sc->gsp_tok is already held by caller? No --- caller does
		 * NOT hold it. Take it here and surround the wait. The token
		 * is auto-released by tsleep() and reacquired on wake, so a
		 * concurrent drainer (ISR or another lwkt) can run during
		 * the wait and signal our pending entry. */
		lwkt_gettoken(&sc->gsp_tok);
		LIST_INSERT_HEAD(&sc->gsp_pending, &p, link);

		while (!atomic_load_acq_int(&p.done) && timeout_ticks > 0) {
			/* Drain whatever's already in msgq; may complete us. */
			nvkm_gsp_msgq_drain_locked(sc);
			if (atomic_load_acq_int(&p.done))
				break;

			/* Sleep up to 1 tick, then re-drain. Bound the total
			 * wait at 5s. */
			ticks_to_wait = (timeout_ticks > hz/10) ? hz/10 : timeout_ticks;
			(void)tsleep(&p, 0, "gsprpc", ticks_to_wait);
			timeout_ticks -= ticks_to_wait;
		}

		LIST_REMOVE(&p, link);
		lwkt_reltoken(&sc->gsp_tok);

		if (!atomic_load_acq_int(&p.done)) {
			device_printf(sc->dev,
			    "rpc_push: timeout waiting for fn=%u seq=%u reply\n",
			    fn, p.seq);
			return (NULL);
		}

		return ((uint8_t *)p.reply_buf + NVKM_GSP_RPC_HDR_SIZE);
	}
	}
	return (NULL);
}

void
nvkm_gsp_rpc_done(struct nvkm_softc *sc, void *params)
{
	(void)sc;
	if (params == NULL || params == (void *)(uintptr_t)1)
		return;
	/* Reply buffer was kmalloc'd in msgq_recv_one_elem and given to
	 * caller offset by NVKM_GSP_RPC_HDR_SIZE. Walk back to base. */
	kfree((uint8_t *)params - NVKM_GSP_RPC_HDR_SIZE, M_TEMP);
}

/* ===================================================================
 * Convenience: set_system_info, set_registry (NOSEQ pre-boot).
 * =================================================================== */

int
nvkm_gsp_rpc_set_system_info(struct nvkm_softc *sc)
{
	GspSystemInfo *info;

	if (sc->bar_res[0] == NULL || sc->bar_res[1] == NULL ||
	    sc->bar_res[3] == NULL) {
		device_printf(sc->dev,
		    "set_system_info: skipped (BARs not allocated)\n");
		return (ENXIO);
	}

	info = nvkm_gsp_rpc_get(sc,
	    NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, sizeof(*info));
	if (info == NULL)
		return (ENOMEM);

	info->gpuPhysAddr     = rman_get_start(sc->bar_res[0]);
	info->gpuPhysFbAddr   = rman_get_start(sc->bar_res[1]);
	info->gpuPhysInstAddr = rman_get_start(sc->bar_res[3]);
	info->gpuPhysIoAddr   = 0;
	{
		/* Open-rm encoding (g_gpu_nvoc.h:476-478):
		 *   bits 63:32 DOMAIN, bits 15:8 BUS, bits 7:0 DEVICE (devfn).
		 * Matches Linux pci_dev_id() = (bus<<8) | devfn,
		 * where devfn = (slot<<3) | func. */
		uint32_t bus    = pci_get_bus(sc->dev);
		uint32_t slot   = pci_get_slot(sc->dev);
		uint32_t func   = pci_get_function(sc->dev);
		uint32_t dom    = pci_get_domain(sc->dev);
		uint32_t devfn  = ((slot & 0x1f) << 3) | (func & 0x7);
		info->nvDomainBusDeviceFunc =
		    ((uint64_t)dom << 32) | ((bus & 0xff) << 8) | (devfn & 0xff);
	}
	info->maxUserVa          = (1ULL << 47);
	info->pciConfigMirrorBase= 0x88000;
	info->pciConfigMirrorSize= 0x1000;
	info->PCIDeviceID        =
	    ((uint32_t)pci_get_device(sc->dev) << 16) |
	     (uint32_t)pci_get_vendor(sc->dev);
	info->PCISubDeviceID     =
	    ((uint32_t)pci_get_subdevice(sc->dev) << 16) |
	     (uint32_t)pci_get_subvendor(sc->dev);
	info->PCIRevisionID      = pci_get_revid(sc->dev);

	return (nvkm_gsp_rpc_wr(sc, info, NVKM_GSP_RPC_REPLY_NOSEQ));
}

int
nvkm_gsp_rpc_set_registry(struct nvkm_softc *sc)
{
	PACKED_REGISTRY_TABLE *reg;

	reg = nvkm_gsp_rpc_get(sc,
	    NV_VGPU_MSG_FUNCTION_SET_REGISTRY, sizeof(*reg));
	if (reg == NULL)
		return (ENOMEM);
	reg->size = sizeof(*reg);
	reg->numEntries = 0;
	return (nvkm_gsp_rpc_wr(sc, reg, NVKM_GSP_RPC_REPLY_NOSEQ));
}
