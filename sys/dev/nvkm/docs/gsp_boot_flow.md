# Nouveau GSP-RM Boot Flow on TU102 — Full Source Trace

Reference paths (relative to /Users/lileding/src/dfly-gsp/):
- `linux/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/`   (Nouveau GSP subsystem)
- `linux/drivers/gpu/drm/nouveau/nvkm/falcon/`       (Falcon engine helpers)
- `refs/open-rm-570.144/src/nvidia/`                 (NVIDIA open-rm 570.144 reference)

All file refs without a tree prefix are nouveau. Open-rm refs are tagged `open-rm:`.

---

## 0. High-level boot phases

| Phase | nouveau entry | What happens                                                              | Engine(s) |
|------:|---------------|---------------------------------------------------------------------------|-----------|
| P0    | `nvkm_gsp_new_`      | Probe + ctor: load firmware blobs, init Falcon abstraction       | host CPU  |
| P1    | `tu102_gsp_oneinit`  | Allocate everything; run FwSec-FRTS; reset GSP-Falcon            | GSP-Falcon (HS) |
| P2    | `tu102_gsp_init`     | Run booter_load on SEC2 → GSP RISC-V boots                       | SEC2 (HS) → GSP RISC-V |
| P3    | `r535_gsp_init`      | Wait for `GSP_INIT_DONE` RPC; RM is alive                        | host ↔ GSP RPC |
| P4    | runtime              | Driver issues RPC, GSP runs the real RM                           | both |

The booter on **SEC2** is the agent that copies GSP-RM image from sysmem (via radix3) into VRAM WPR2 and ultimately fires GSP's RISC-V core. Everything in P1 is **preparation** for the booter; P2 is **the actual GSP launch**; P3 confirms.

---

## P0. Module probe — `nvkm_gsp_new_` (gsp/base.c:130)

```c
int nvkm_gsp_new_(const struct nvkm_gsp_fwif *fwif, ..., struct nvkm_gsp **pgsp)
{
    gsp = kzalloc_obj(*gsp);
    nvkm_subdev_ctor(&nvkm_gsp, device, type, inst, &gsp->subdev);

    fwif = nvkm_firmware_load(&gsp->subdev, fwif, "Gsp", gsp);
    //  iterates fwif[] table; for each version, calls fwif->load(gsp, ...)
    //  tu102_gsp_load_rm (tu102.c:398) loads:
    //    - "gsp/gsp-570.144"            -> gsp->fws.rm           (ELF, ~28 MiB after unxz)
    //    - "gsp/bootloader-570.144"     -> gsp->fws.bl           (4196 B)
    //  tu102_gsp_load (tu102.c:418) additionally loads:
    //    - "gsp/booter_load-570.144"    -> gsp->fws.booter.load  (59272 B)
    //    - "gsp/booter_unload-570.144"  -> gsp->fws.booter.unload (60440 B)

    gsp->func = fwif->func;       // = &tu102_gsp (tu102.c:376)
    gsp->rm   = kzalloc_obj();
    gsp->rm->wpr = fwif->rm->wpr; // = &r570_wpr_libos3_baremetal_tu102
    gsp->rm->api = fwif->rm->api; // = &r535_rm_gsp / r570_rm_gsp dispatch tables

    nvkm_falcon_ctor(gsp->func->flcn, ..., 0x110000, &gsp->falcon);
    //  GSP-Falcon at PRI base 0x110000; addr2 (RISC-V control) = +0x1000 = 0x111000;
    //  fbif = +0x600 = 0x110600
}
```

`gsp->rm->wpr` carries the heap-size parameters for this chip family:
```c
// rm/r570/rm.c
static const struct nvkm_rm_wpr r570_wpr_libos3_baremetal_tu102 = {
    .os_carveout_size = GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS3_BAREMETAL,   // 22 << 20 = 22 MiB
    .base_size        = GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X,         //  8 << 20 = 8  MiB
    .heap_size_min    = GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_BAREMETAL_MIN_MB, // 88 MiB
};
```

`gsp->func` (= `tu102_gsp`, tu102.c:376) is the dispatch table:
```c
.flcn      = &tu102_gsp_flcn,
.fwsec     = &tu102_gsp_fwsec,
.sig_section = ".fwsignature_tu10x",     // ELF section name we extract
.booter.ctor   = tu102_gsp_booter_ctor,
.fwsec_sb.ctor = tu102_gsp_fwsec_sb_ctor,
.fwsec_sb.dtor = tu102_gsp_fwsec_sb_dtor,
.dtor      = r535_gsp_dtor,
.oneinit   = tu102_gsp_oneinit,
.init      = tu102_gsp_init,
.fini      = tu102_gsp_fini,
.reset     = tu102_gsp_reset,
.rm.gpu    = &tu1xx_gpu,
```

End of P0: no hardware accessed yet (other than implicit BARs from probe). Memory state: firmware blobs loaded; gsp->falcon constructed; nothing executed on the GPU.

---

## P1. `tu102_gsp_oneinit` (tu102.c:295) — the long prep

```c
int tu102_gsp_oneinit(struct nvkm_gsp *gsp)
{
    /* 1. Read FB layout */
    gsp->fb.size = nvkm_fb_vidmem_size(device);
        // = gp102_fb_vidmem_size: decode 0x100ce0
        //   lmag = (val >> 4) & 0x3F; lsca = val & 0xF
        //   size = lmag << (lsca + 20)
        //   if bit 30 set: size = size / 16 * 15

    gsp->fb.bios.vga_workspace.addr = tu102_gsp_vga_workspace_addr(gsp, fb_size);
        // 1. base = fb_size - 0x100000
        // 2. addr = read 0x625f04 (NV_PDISP_VGA_CR)
        // 3. if !(addr & 0x8):  return base                  // VGA aperture not enabled
        // 4. else:
        //      addr = (addr & 0xffffff00) << 8
        //      if addr < base: return fb_size - 0x20000      // big-VRAM workaround
        //      else:           return addr
    gsp->fb.bios.vga_workspace.size = fb_size - vga_workspace.addr;
    gsp->fb.bios.addr = vga_workspace.addr;
    gsp->fb.bios.size = vga_workspace.size;

    /* 2. Build booter ucode objects (PARSE, no execution yet) */
    gsp->func->booter.ctor(gsp, "booter-load",   gsp->fws.booter.load,
                           &device->sec2->falcon, &gsp->booter.load);
    gsp->func->booter.ctor(gsp, "booter-unload", gsp->fws.booter.unload,
                           &device->sec2->falcon, &gsp->booter.unload);
        // = tu102_gsp_booter_ctor (see SECTION P1a)

    /* 3. r535_gsp_oneinit — all sysmem allocations + RM setup */
    r535_gsp_oneinit(gsp);  // SEE SECTION P1b

    /* 4. Compute FB / WPR2 layout (top-down) */
    gsp->fb.wpr2.frts.size = 0x100000;                                     // 1 MiB
    gsp->fb.wpr2.frts.addr = ALIGN_DOWN(bios.addr, 0x20000) - 0x100000;

    gsp->fb.wpr2.boot.size = gsp->boot.fw.size;                            // BL data section size
    gsp->fb.wpr2.boot.addr = ALIGN_DOWN(frts.addr - boot.size, 0x1000);    // 4 KiB align

    gsp->fb.wpr2.elf.size  = gsp->fw.len;                                  // .fwimage size
    gsp->fb.wpr2.elf.addr  = ALIGN_DOWN(boot.addr - elf.size, 0x10000);    // 64 KiB align

    gsp->fb.wpr2.heap.size = tu102_gsp_wpr_heap_size(gsp);
        // = os_carveout (22M) + base_size (8M)
        //   + ALIGN(96K/GB × fb_gb, 1M)        // 11 GB → 2 MiB
        //   + ALIGN(48K × 2048, 1M)             // 96 MiB
        //   clamped to heap_size_min (88M)
        // Result on 11 GiB TU102: 128 MiB
    gsp->fb.wpr2.heap.addr = ALIGN_DOWN(elf.addr - heap.size, 0x100000);   // 1 MiB align
    gsp->fb.wpr2.heap.size = ALIGN_DOWN(elf.addr - heap.addr, 0x100000);   // recompute aligned

    gsp->fb.wpr2.addr = ALIGN_DOWN(heap.addr - sizeof(GspFwWprMeta), 0x100000);
    gsp->fb.wpr2.size = frts.addr + frts.size - wpr2.addr;

    gsp->fb.heap.size = 0x100000;                                           // 1 MiB non-WPR heap
    gsp->fb.heap.addr = wpr2.addr - heap.size;

    /* 5. Fill the GspFwWprMeta struct (sysmem buffer) */
    tu102_gsp_wpr_meta_init(gsp);      // SEE SECTION P1c

    /* 6. Run FwSec-FRTS on GSP-Falcon (programs hardware WPR2 around FRTS) */
    nvkm_gsp_fwsec_frts(gsp);          // SEE SECTION P1d

    /* 7. Reset GSP-Falcon (comment says "into RISC-V mode" but it's
     *    just falcon->func->reset_eng — gp102_flcn_reset_eng:
     *      mask(falcon, 0x3c0, 0x1, 0x1)
     *      udelay(10)
     *      mask(falcon, 0x3c0, 0x1, 0x0)
     *      reset_wait_mem_scrubbing
     *    On TU102 there is NO explicit core switch: kflcnSwitchToFalcon
     *    just sets a software tristate. After reset the engine is in a
     *    state that can run either Falcon or RISC-V; mode is decided by
     *    who writes CPUCTL.STARTCPU next.
     */
    gsp->func->reset(gsp);

    /* 8. Seed GSP-Falcon MAILBOX0/1 with libos sysmem PA.
     *    Comment: "Booter does not read these. They are preserved through
     *    booter execution and are read by GSP-RM (running on GSP RISC-V)
     *    once the booter releases the core."
     *
     *    Open-rm equivalent: kgspProgramLibosBootArgsAddr_TU102 (line 325)
     *    does the same writes.
     */
    nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(gsp->libos.addr));
    nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(gsp->libos.addr));
    return 0;
}
```

### P1a. `tu102_gsp_booter_ctor` (tu102.c:77) — parsing the booter ucode

```c
hdr   = nvfw_bin_hdr(blob->data);
hshdr = nvfw_hs_header_v2(blob->data + hdr->header_offset);

loc = *(u32*)(blob->data + hshdr->patch_loc);    // → 0x8700 (offset into data)
sig = *(u32*)(blob->data + hshdr->patch_sig);    // → 0
cnt = *(u32*)(blob->data + hshdr->num_sig);      // → 1

nvkm_falcon_fw_ctor(&gm200_flcn_fw, "booter-load", device, /*dma=*/true,
                    blob->data + hdr->data_offset,   // src = data section
                    hdr->data_size,                  // = 59136
                    &device->sec2->falcon, fw);

// Stash signature(s) in fw->sigs[] (16 bytes each, 1 sig).
nvkm_falcon_fw_sign(fw, /*sig_base_img=*/loc, /*sig_size=*/16,
                    blob->data, cnt,
                    hshdr->sig_prod_offset + sig, /*nr_dbg=*/0, 0);

lhdr = nvfw_hs_load_header_v2(blob->data + hshdr->header_offset);

fw->nmem_base_img = 0;                              // src offset for NS code in fw.img
fw->nmem_base     = lhdr->os_code_offset;           // IMEM destination (= 0)
fw->nmem_size     = lhdr->os_code_size;             // 256 bytes
fw->imem_base_img = fw->nmem_size;                  // src offset for SEC code (= 256)
fw->imem_base     = lhdr->app[0].offset;            // IMEM destination (= 0x100)
fw->imem_size     = lhdr->app[0].size;              // 33792 bytes
fw->dmem_base_img = lhdr->os_data_offset;           // src offset for DMEM (= 0x8500)
fw->dmem_base     = 0;                              // DMEM destination
fw->dmem_size     = lhdr->os_data_size;             // 25088 bytes
fw->dmem_sign     = loc - fw->dmem_base_img;        // 0x8700 - 0x8500 = 0x200 = sig offset in DMEM
fw->boot_addr     = lhdr->os_code_offset;           // = 0
fw->boot          = NULL;                           // no separate BL stub — booter IS the HS image
```

Key: `fw->boot == NULL` ⇒ `gm200_flcn_fw_load` takes the **multi-PIO** path at run time (no BL DMA fetch).

### P1b. `r535_gsp_oneinit` (rm/r535/gsp.c:2132) — sysmem allocations

```c
mutex_init(&gsp->cmdq.mutex);
mutex_init(&gsp->msgq.mutex);

/* (a) GSP-RM .fwimage section → contiguous SG-table */
r535_gsp_elf_section(gsp, ".fwimage", &data, &size);   // 28528288 bytes for tu10x
nvkm_firmware_ctor(&r535_gsp_fw /* type = NVKM_FIRMWARE_IMG_SGT */,
                   "gsp-rm", device, data, size, &gsp->fw);
//  SG table (gsp->fw.mem.sgt) of however-many pages it took to back the image.
//  gsp->fw.len = size

/* (b) Per-arch signature blob */
r535_gsp_elf_section(gsp, gsp->func->sig_section, &data, &size);
                                              // ".fwsignature_tu10x", size = 4096
nvkm_gsp_mem_ctor(gsp, ALIGN(size, 256), &gsp->sig);  // contiguous 4 KiB sysmem
memcpy(gsp->sig.data, data, size);

/* (c) Radix3 page table over the SG-table image */
nvkm_gsp_radix3_sg(gsp, &gsp->fw.mem.sgt, gsp->fw.len, &gsp->radix3);
                                              // see SECTION P1b' below

/* (d) Register RPC message notification handlers */
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER, ...);
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_POST_EVENT, ...);
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_RC_TRIGGERED, ...);
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED, ...);
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_OS_ERROR_LOG, ...);
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_PERF_BRIDGELESS_INFO_UPDATE, NULL, NULL);
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT, NULL, NULL);

/* (e) Boot BL + FwSec-SB ucode allocation */
r535_gsp_rm_boot_ctor(gsp);    // SEE SECTION P1b''

/* (f) Free linux-firmware blobs (we've copied what we need) */
nvkm_gsp_dtor_fws(gsp);

/* (g) Libos logging buffers */
r535_gsp_libos_init(gsp);      // SEE SECTION P1b'''

/* (h) Fill GSP-RM init RPC args */
rmapi->gsp->set_system_info(gsp);     // ChipID, vendor, sub-vendor, system info...
r535_gsp_rpc_set_registry(gsp);       // GSP-RM "registry" key/values
```

### P1b'. `nvkm_gsp_radix3_sg` (rm/r535/gsp.c:1657) — radix3 page table

```c
nvkm_gsp_mem_ctor(gsp, GSP_PAGE_SIZE /*4 KiB*/, &rx3->lvl0);   // 1 page
nvkm_gsp_mem_ctor(gsp, GSP_PAGE_SIZE              , &rx3->lvl1);   // 1 page
bufsize = ALIGN((size/GSP_PAGE_SIZE) * sizeof(u64), GSP_PAGE_SIZE);
nvkm_gsp_sg(device, bufsize, &rx3->lvl2);                          // SG-allocated

// L0: single u64 = bus address of L1 page
*(u64 *)rx3->lvl0.data = rx3->lvl1.addr;

// L1: 512 u64 entries, each = bus address of one L2 page (walked via SG)
pte = rx3->lvl1.data;
for_each_sgtable_dma_page(&rx3->lvl2, &iter, 0)
    *pte++ = sg_page_iter_dma_address(&iter);

// L2 pages: each holds up to 512 u64 entries, each = bus address of one
//   page of the source SG (the image data)
for_each_sgtable_sg(&rx3->lvl2, sg, i) {
    pte = sg_virt(sg);
    for_each_sgtable_dma_page(sgt, &iter, page_idx) {
        *pte++ = sg_page_iter_dma_address(&iter);
        page_idx++;
        if ((void *)pte >= sgl_end) break;
    }
}
```

All entries are **raw 64-bit bus addresses**, not shifted. Layout:
```
L0:  [ &L1 ]
L1:  [ &L2[0], &L2[1], ..., &L2[N-1] ]                 N = ceil(npages/512)
L2[0]:  [ &data[0..0xFFF], &data[0x1000..0x1FFF], ... ]  (up to 512 entries)
L2[1]:  [ &data[0x200000..0x200FFF], ... ]
...
```

Max image: 512×512×4 KiB = 1 GiB.

### P1b''. `r535_gsp_rm_boot_ctor` (rm/r535/gsp.c:1814) — BL staging + FwSec-SB ctor

```c
nvkm_gsp_fwsec_sb_ctor(gsp);                    // construct gsp->fws.falcon.sb (boots at FINI)

hdr  = nvfw_bin_hdr(fw->data);
desc = (RM_RISCV_UCODE_DESC *)(fw->data + hdr->header_offset);

nvkm_gsp_mem_ctor(gsp, hdr->data_size, &gsp->boot.fw);    // 4096 bytes sysmem
memcpy(gsp->boot.fw.data, fw->data + hdr->data_offset, hdr->data_size);

gsp->boot.code_offset      = desc->monitorCodeOffset;     // = 0 in linux-firmware blob
gsp->boot.data_offset      = desc->monitorDataOffset;     // = 0
gsp->boot.manifest_offset  = desc->manifestOffset;        // = 0
gsp->boot.app_version      = desc->appVersion;            // = 0
```

### P1b'''. `r535_gsp_libos_init` (rm/r535/gsp.c:1508) — log buffer setup

```c
nvkm_gsp_mem_ctor(gsp, 0x1000, &gsp->libos);     // 4 KiB args page
args = gsp->libos.data;   // LibosMemoryRegionInitArgument[]

/* LOGINIT — early-init log buffer */
nvkm_gsp_mem_ctor(gsp, 0x10000 /* 64 KiB */, &gsp->loginit);
args[0].id8  = id8("LOGINIT");
args[0].pa   = gsp->loginit.addr;
args[0].size = gsp->loginit.size;
args[0].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
args[0].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
create_pte_array(gsp->loginit.data + 8, gsp->loginit.addr, gsp->loginit.size);

/* LOGINTR + LOGRM */ (same pattern, args[1] and args[2])

/* RM args (varies by ver) */
gsp->rm->api->gsp->set_rmargs(gsp, false);
```

The libos arg structure lives at `gsp->libos.addr` (sysmem PA). That PA gets written to GSP-Falcon MAILBOX0/1 in step 8 of `tu102_gsp_oneinit`, just before P2.

### P1c. `tu102_gsp_wpr_meta_init` (tu102.c:213) — wpr_meta byte layout

GspFwWprMeta is exactly **256 bytes** (`_Static_assert`-enforced in open-rm). Field-by-field:

| Byte off | Field | Value (TU102 11 GiB FB) | Source |
|---------:|-------|--------------------------|--------|
| 0x00 | `magic` (u64)  | 0xdc3aae21371a60b3 | fixed |
| 0x08 | `revision` (u64) | 1 | fixed |
| 0x10 | `sysmemAddrOfRadix3Elf` (u64) | sysmem PA of `gsp->radix3.lvl0` page | radix3 ctor |
| 0x18 | `sizeOfRadix3Elf` (u64) | `gsp->fw.len` = 28528288 | .fwimage size |
| 0x20 | `sysmemAddrOfBootloader` (u64) | sysmem PA of `gsp->boot.fw` | BL ctor |
| 0x28 | `sizeOfBootloader` (u64) | `hdr->data_size` = 4096 | BL hdr |
| 0x30 | `bootloaderCodeOffset` (u64) | 0 (`desc->monitorCodeOffset`) | BL desc |
| 0x38 | `bootloaderDataOffset` (u64) | 0 (`desc->monitorDataOffset`) | BL desc |
| 0x40 | `bootloaderManifestOffset` (u64) | 0 (`desc->manifestOffset`) | BL desc |
| 0x48 | `sysmemAddrOfSignature` (u64) | sysmem PA of `gsp->sig` | sig ctor |
| 0x50 | `sizeOfSignature` (u64) | 4096 | .fwsignature_tu10x size |
| 0x58 | `gspFwRsvdStart` (u64) | `fb.heap.addr` = wpr2.addr - 1 MiB | computed |
| 0x60 | `nonWprHeapOffset` (u64) | same as gspFwRsvdStart | computed |
| 0x68 | `nonWprHeapSize` (u64) | 0x100000 (1 MiB) | fixed |
| 0x70 | `gspFwWprStart` (u64) | `wpr2.addr` (1 MiB aligned below heap-meta) | computed |
| 0x78 | `gspFwHeapOffset` (u64) | `wpr2.heap.addr` (1 MiB aligned, ~128 MiB tall) | computed |
| 0x80 | `gspFwHeapSize` (u64) | `wpr2.heap.size` | computed |
| 0x88 | `gspFwOffset` (u64) | `wpr2.elf.addr` (64 KiB aligned) | computed |
| 0x90 | `bootBinOffset` (u64) | `wpr2.boot.addr` (4 KiB aligned) | computed |
| 0x98 | `frtsOffset` (u64) | `wpr2.frts.addr` | computed |
| 0xA0 | `frtsSize` (u64) | 0x100000 | fixed |
| 0xA8 | `gspFwWprEnd` (u64) | ALIGN_DOWN(bios.vga_workspace.addr, 0x20000) | computed |
| 0xB0 | `fbSize` (u64) | from PFB_PRI_MMU_LOCAL_MEMORY_RANGE decode | hardware |
| 0xB8 | `vgaWorkspaceOffset` (u64) | `bios.vga_workspace.addr` | computed |
| 0xC0 | `vgaWorkspaceSize` (u64) | `bios.vga_workspace.size` | computed |
| 0xC8 | `bootCount` (u64) | 0 | fixed |
| 0xD0 | `partitionRpcAddr` (u64) | 0 | fixed |
| 0xD8 | `partitionRpcRequestOffset` (u16) | 0 | fixed |
| 0xDA | `partitionRpcReplyOffset` (u16) | 0 | fixed |
| 0xDC | `elfCodeOffset` (u32) | 0 | union — used at suspend |
| 0xE0 | `elfDataOffset` (u32) | 0 | union |
| 0xE4 | `elfCodeSize`   (u32) | 0 | union |
| 0xE8 | `elfDataSize`   (u32) | 0 | union |
| 0xEC | `lsUcodeVersion`(u32) | 0 | union |
| 0xF0 | `gspFwHeapVfPartitionCount` (u8) | 0 | fixed |
| 0xF1 | `flags` (u8) | 0 | fixed |
| 0xF2 | `padding[2]` (u8×2) | 0 | fixed |
| 0xF4 | `pmuReservedSize` (u32) | 0 (not used on TU102) | fixed |
| 0xF8 | `verified` (u64) | 0 (booter sets to 0xa0a0a0a0a0a0a0a0 on success) | booter |

VRAM/WPR2 layout TOP-DOWN summary:

```
+------------------------------------------------+
| fbSize - X = end of FB                          |
+------------------------------------------------+ <- fbSize
| VGA WORKSPACE                                   |
+------------------------------------------------+ <- vgaWorkspaceOffset = bios.addr
| FRTS data       (1 MiB, owned by FwSec)         |
+------------------------------------------------+ <- frtsOffset           ┐
| BOOT BIN        (4 KiB, sysmem→VRAM by booter)  |                        │
+------------------------------------------------+ <- bootBinOffset        │
| GSP FW ELF      (.fwimage, ~28 MiB)              |                       │  WPR2
+------------------------------------------------+ <- gspFwOffset          │
| GSP FW HEAP     (~128 MiB)                       |                       │
+------------------------------------------------+ <- gspFwHeapOffset      │
| meta struct     (256 B, copied by booter)        |                       │
+------------------------------------------------+ <- gspFwWprStart        ┘
| non-WPR HEAP    (1 MiB)                         |
+------------------------------------------------+ <- nonWprHeapOffset = gspFwRsvdStart
```

`gspFwWprEnd` = top of WPR2 = `frts.addr + frts.size` = `ALIGN_DOWN(bios.addr, 0x20000)`.

### P1d. `nvkm_gsp_fwsec_frts` (rm/r535/.. via subdev/gsp/fwsec.c:347) — FwSec-FRTS

```c
nvkm_gsp_fwsec_init(gsp, &fw, "fwsec-frts", CMD_FRTS=0x15);
   // - Look up in VBIOS PMU table type 0x85 → FALCON_UCODE_DESC_V2
   // - nvkm_falcon_fw_ctor with body = desc+size, len = IMEMLoadSize+DMEMLoadSize
   // - fw->nmem_base = IMEMPhysBase, nmem_size = IMEMLoadSize - IMEMSecSize
   //   fw->imem_base = IMEMSecBase,  imem_size = IMEMSecSize
   //   fw->dmem_base = DMEMPhysBase, dmem_size = DMEMLoadSize, dmem_base_img = DMEMOffset
   //   fw->boot_addr = blStartTag << 8     ← uses generic acr/bl BL stub
   //   fw->boot      = acr/bl image (so gm200_flcn_fw_load takes BL+DMA path)
   //
   //   IMPORTANT: there are TWO V2 candidates with identical sizes in the
   //   VBIOS — one signed for DEBUG-fused chips, one for PROD. Retail 2080
   //   Ti is PROD; using the DBG one fails HS auth with a DEAD5EC3 IMEM
   //   scrub. (Empirical finding, not in nouveau directly — nouveau's
   //   nvbios_pmuEp iterates and takes the first; on dev chips/dbg blobs
   //   that ordering is reversed.)
   //
   // - nvkm_gsp_fwsec_patch:
   //     locate DMEMMAPPER_V3 entry in appif header
   //     write dmemmap->v3.init_cmd = 0x15 (FRTS)
   //     write FRTS cmd at dmemmap->v3.cmd_in_buffer_offset:
   //         read_vbios.ver=1, hdr=sizeof, addr=0, size=0, flags=2
   //         frts_region.ver=1, hdr=sizeof,
   //                     addr  = fb.wpr2.frts.addr >> 12,
   //                     size  = fb.wpr2.frts.size >> 12,
   //                     type  = 2 (FB)

nvkm_gsp_fwsec_boot(gsp, &fw);
   // = nvkm_falcon_fw_boot(fw, subdev, true, &mbox0=0, NULL, 0, 0)
   //
   // calls nvkm_falcon_fw_oneinit -> nvkm_falcon_fw_patch (sig)
   // then  gm200_flcn_fw_load (BL+DMA path):
   //   - mask(0x624, 0x80, 0x80)        // scheduler arb-on-noctx
   //   - wr32(0x10c=DMACTL, 0)          // (BUT also need FBIF_CTL — see "disable_ctx_req")
   //   - pio_wr(BL stub, IMEM dest = falcon.code.limit - boot_size, tag = boot_addr>>8, secure=false)
   //   - load_bld(fw) → tu102_gsp_fwsec_load_bld:
   //       desc = flcn_bl_dmem_desc_v2 {
   //           ctx_dma            = FALCON_DMAIDX_PHYS_SYS_NCOH (= 4),
   //           code_dma_base      = fw->fw.phys,
   //           non_sec_code_off   = fw->nmem_base,
   //           non_sec_code_size  = fw->nmem_size,
   //           sec_code_off       = fw->imem_base,
   //           sec_code_size      = fw->imem_size,
   //           code_entry_point   = 0,
   //           data_dma_base      = fw->fw.phys + fw->dmem_base_img,
   //           data_size          = fw->dmem_size,
   //           argc = 0, argv = 0,
   //       };
   //       mask(falcon, 0x600 + ctx_dma*4 /* FBIF_TRANSCFG[4] */, 0x7, 0x5)  // COH_SYS PHYS
   //       pio_wr(&desc, DMEM, dest=0, size=sizeof(desc))
   //   - then gm200_flcn_fw_boot:
   //       wr32(0x040, 0)                // pre-set mb0
   //       wr32(0x104=BOOTVEC, fw->boot_addr)
   //       wr32(0x100=CPUCTL, 0x2 STARTCPU)
   //       wait HALT (CPUCTL bit 4)
   //
   // Verify: scratch[0xE] @ 0x1438 -- bits 31:16 = FRTS_ERR_CODE; 0 = NONE
   //         WPR2_LO @ 0x1fa824, WPR2_HI @ 0x1fa828
   //         Encoded: VAL = (addr >> 12) << 4; so addr = (reg & 0xfffffff0) << 8
   //         If WPR2_HI == 0 -> failure regardless of scratch
   //
   // On success: hardware WPR2 is programmed around the FRTS region;
   //             scratch[0xE] FRTS_ERR_CODE = 0.
```

End of P1. The state of the world:

- **sysmem**: wpr_meta filled, radix3 built, GSP-RM .fwimage SG'd, .fwsignature_tu10x staged, BL data staged, libos + LOGINIT/LOGINTR/LOGRM allocated.
- **VRAM**: hardware WPR2 register programmed by FwSec to span the FRTS region only ([frts.addr, frts.addr + ~frts.size)).
- **GSP-Falcon**: reset; MAILBOX0/1 = sysmem PA of libos args.
- **SEC2**: untouched (FwSec ran on GSP-Falcon).
- **booter ucode**: parsed into `gsp->booter.load`/`unload` (function pointers + image buffer staged in sysmem via nvkm_firmware_ctor with dma=true).

---

## P2. `tu102_gsp_init` (tu102.c:188) — the booter execution

```c
int tu102_gsp_init(struct nvkm_gsp *gsp)
{
    u32 mbox0, mbox1;

    if (!gsp->sr.meta.data) {                       // normal boot path
        mbox0 = lower_32_bits(gsp->wpr_meta.addr);
        mbox1 = upper_32_bits(gsp->wpr_meta.addr);
    } else {                                        // suspend-resume path
        gsp->rm->api->gsp->set_rmargs(gsp, true);
        mbox0 = lower_32_bits(gsp->sr.meta.addr);
        mbox1 = upper_32_bits(gsp->sr.meta.addr);
    }

    /* Booter takes wpr_meta PA in SEC2's MAILBOX0/1 */
    tu102_gsp_booter_load(gsp, mbox0, mbox1);
        // = nvkm_falcon_fw_boot(&gsp->booter.load /* on SEC2 */,
        //                       subdev, /*release=*/true,
        //                       &mbox0, &mbox1, /*mbox0_ok=*/0, /*irqsclr=*/0);

    return r535_gsp_init(gsp);  // SEE SECTION P3
}
```

### P2a. `nvkm_falcon_fw_boot` (falcon/fw.c:74) — the actual booter run

```c
nvkm_falcon_get(falcon /* SEC2 */, user);            // claims engine (mutex etc.)

if (fw->sigs) nvkm_falcon_fw_patch(fw);              // copy fw->sigs[idx] into fw->fw.img[sig_base_img]

if (fw->func->reset_eng) fw->func->reset_eng(fw);    // SEC2 reset_eng — same 0x3c0 pulse

fw->func->load(fw)  /* = gm200_flcn_fw_load */;
//
// Because fw->boot == NULL (booter is an HS image, no BL stub), takes the
// no-boot branch (gm200.c:299-313):
//
//   if (fw->inst) {   /* booter has inst==NULL */ ... bind_inst path ... }
//   else {
//       nvkm_falcon_mask(falcon, 0x624, 0x80, 0x80);
//       nvkm_falcon_wr32(falcon,  0x10c /*DMACTL*/, 0x0);
//   }
//   pio_wr(falcon, fw.img + nmem_base_img, ... , IMEM,
//          dest = nmem_base, size = nmem_size,
//          tag  = nmem_base >> 8, secure = false);
//   pio_wr(falcon, fw.img + imem_base_img, ... , IMEM,
//          dest = imem_base, size = imem_size,
//          tag  = imem_base >> 8, secure = true);
//   pio_wr(falcon, fw.img + dmem_base_img, ... , DMEM,
//          dest = 0, size = dmem_size, tag = 0, secure = false);

fw->func->boot(fw, ...)  /* = gm200_flcn_fw_boot */;
//   wr32(0x040, *pmbox0 ?: 0xcafebeef);
//   wr32(0x044, *pmbox1);
//   wr32(0x104=BOOTVEC, fw->boot_addr /* = 0 */);
//   wr32(0x100=CPUCTL, 0x2 STARTCPU);
//   nvkm_msec(2000ms, until rd32(0x100) & 0x10 /* HALT */);
//   mbox0 = rd32(0x040); mbox1 = rd32(0x044);
//   if (mbox0 != mbox0_ok /* = 0 */) return -EIO;
//
// On success, mbox0 reads back 0.
```

What the booter does internally (NVIDIA-signed code; not documented in open-rm):

1. Read `mbox0:mbox1` = sysmem PA of GspFwWprMeta.
2. DMA-fetch the GspFwWprMeta (256 B) from sysmem.
3. Validate `magic == 0xdc3aae21371a60b3` and `revision == 1`.
4. DMA-walk the radix3 (lvl0 → lvl1 → lvl2 → data pages), copy `sizeOfRadix3Elf` bytes of `.fwimage` into VRAM at `gspFwOffset`.
5. DMA-fetch the BL data (`sizeOfBootloader` bytes) from `sysmemAddrOfBootloader`, copy into VRAM at `bootBinOffset`.
6. DMA-fetch the signature (`sizeOfSignature` bytes) from `sysmemAddrOfSignature`; verify GSP-RM image against it.
7. Program hardware WPR2 PRI registers to cover **the entire WPR2** (gspFwWprStart..gspFwWprEnd), locking the new VRAM image.
8. Write the meta struct itself into VRAM at gspFwWprStart, with `verified = 0xa0a0a0a0a0a0a0a0`.
9. **Configure GSP-Falcon to enter RISC-V mode and start GSP from `bootBinOffset`** ← this is the step that NEEDS to happen for r535_gsp_init to see riscv_active.
10. Halt SEC2; mb0 = 0 on success.

After return, `r535_gsp_init` asserts that GSP RISC-V is active.

---

## P3. `r535_gsp_init` (rm/r535/gsp.c:1782) — the handoff

```c
int r535_gsp_init(struct nvkm_gsp *gsp)
{
    nvkm_falcon_wr32(&gsp->falcon, 0x080 /* FALCON_OS */, gsp->boot.app_version);
    // Just records the running ucode version — informational, NOT a kicker.

    if (WARN_ON(!nvkm_falcon_riscv_active(&gsp->falcon)))    // = read 0x111240 bit 0
        return -EIO;

    /* From this point GSP-RM is running. cmdq/msgq are live. */
    r535_gsp_rpc_poll(gsp, NV_VGPU_MSG_EVENT_GSP_INIT_DONE);
        // - sends an RPC poll over msgq (sysmem-backed ring) waiting
        //   for GSP-RM to report it has finished its init.
    gsp->running = true;
    r535_gsp_postinit(gsp);
}
```

If RISC-V is NOT active at this point, the WARN_ON fires and the entire boot path fails. **Everything in nouveau after P2 assumes the booter started RISC-V.**

---

## P4. Runtime

- Host writes RPC requests to `gsp->cmdq` (sysmem ring); GSP-RM reads them.
- GSP-RM writes responses + events to `gsp->msgq`; host polls / handles via the notification handlers registered in P1b(d).
- LOGINIT/LOGINTR/LOGRM buffers fill with debug strings.

---

## Register surface used during boot (TU102)

### PMC scratch (host PRI)
| Reg | Use | Notes |
|-----|-----|-------|
| 0x001400 + 4·N | NV_PBUS_VBIOS_SCRATCH(N) | scratch[N], used for FwSec status |
| 0x001438 (N=0xE) | FRTS_ERR_CODE in bits 31:16 | 0 = success |
| 0x001454 (N=0x15) | SB_ERR_CODE in bits 15:0 | 0 = success |
| 0x100ce0 | NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE | encode lmag/lsca → fb_size |
| 0x1fa824 | NV_PFB_PRI_MMU_WPR2_ADDR_LO | bits 31:4 = (addr >> 12) |
| 0x1fa828 | NV_PFB_PRI_MMU_WPR2_ADDR_HI | bits 31:4 = (addr >> 12) |
| 0x625f04 | NV_PDISP_VGA_CR | bit 3 = enabled, bits 1:0 = target |

### GSP-Falcon registers (base 0x110000)
| Off (rel) | Name | Used at |
|----:|------|---------|
| 0x008 | IRQSTAT | post-halt diagnostic |
| 0x024 | EXCI | post-halt diagnostic |
| 0x040 | MAILBOX0 | libos args low 32 (set in P1 step 8); booter rewrites |
| 0x044 | MAILBOX1 | libos args high 32 |
| 0x080 | FALCON_OS | r535_gsp_init writes app_version |
| 0x100 | CPUCTL | bit 1 STARTCPU, bit 4 HALT, bit 6 ALIAS_EN |
| 0x104 | BOOTVEC | falcon-mode PC entry |
| 0x130 | CPUCTL_ALIAS | (used when ALIAS_EN) |
| 0x240 | SCTL | bits 13:12 UCODE_LEVEL (3 = HS) |
| 0x3c0 | FALCON_ENGINE | bit 0 = reset (write 1 then 0) |
| 0x600 + 4·i | FBIF_TRANSCFG[i] | bits 1:0 = target, bit 2 = phys |
| 0x624 | FBIF_CTL | bit 7 = ALLOW_PHYS_NO_CTX |
| 0x10c | DMACTL | bit 0 = REQUIRE_CTX (must be cleared for no-inst DMA) |
| 0x180/0x184 | IMEMC(0)/IMEMD(0) | PIO IMEM access |
| 0x1c0/0x1c4 | DMEMC(0)/DMEMD(0) | PIO DMEM access |
| 0x110/0x11c | DMATRFBASE/FBOFFS | Falcon DMA engine source addr |
| 0x114 | DMATRFMOFFS | Falcon DMA engine memory offset |
| 0x118 | DMATRFCMD | bit 1 = IDLE, bit 4 = IMEM, bit 5 = WRITE, 10:8 = SIZE, 14:12 = CTXDMA |

### GSP RISC-V control (base 0x111000)
| Off (rel) | Name |
|----:|------|
| 0x240 | RISCV_CORE_SWITCH_RISCV_STATUS (bit 0 = ACTIVE) |
| 0x2b4 | RISCV_IRQMASK |
| 0x2b8 | RISCV_IRQDEST |

**Note**: TU102 has NO PRISCV CPUCTL / BOOTVEC / BCR. The RISC-V core is controlled entirely via the GSP-Falcon register set. There is no host-issuable "start RISC-V" register — it is the booter's job.

### SEC2 (base 0x840000)
Same Falcon register layout at 0x840000 + same offsets. FBIF at 0x840600.

---

## The open question

After the booter returns mb0 = 0 and WPR2 has been expanded:

```
GSP-Falcon: CPUCTL = 0x10 (HALT), BOOTVEC = 0, SCTL = 0x3000 (HS halted)
GSP RISC-V: STATUS = 0 (not active)
```

According to nouveau (`r535_gsp_init` line 1788), GSP RISC-V should be active **immediately** after the booter returns; otherwise the WARN_ON fires and init fails. So either:

(a) The booter on this card is short-circuiting before step 9 of its internal flow (the RISC-V kick) despite reporting mb0 = 0, **or**
(b) Some wpr_meta input we're providing is silently steering the booter into a "stage but don't start" branch, **or**
(c) The chip has not been put into a state where the booter's RISC-V kick will take effect — e.g. some pre-FwSec PRI write we have not made.

What nouveau does *between* `nvkm_gsp_fwsec_frts` and the booter is:
1. `gsp->func->reset(gsp)` = GSP-Falcon engine reset (bit 0 of 0x3c0).
2. Write libos sysmem PA to GSP-Falcon MAILBOX0/1.
3. (No scrubber on TU102 since needed_size ≤ prescrubbed_size 256 MiB.)
4. Call the booter on SEC2 with wpr_meta sysmem PA.

We mirror this exactly. The booter is the same linux-firmware blob nouveau uses. Yet RISC-V doesn't come up. The behavior is consistent across reboots and across module reloads (after a fresh reboot).

The next investigations that would actually move this forward:
1. **Dump the post-booter VRAM** at gspFwOffset / bootBinOffset via PRAMIN — verify the booter actually copied .fwimage and the BL into WPR2. If they aren't there, the booter failed silently before the kick step.
2. **Trace what nouveau passes as wpr_meta on a Linux box with the SAME firmware** and byte-compare against ours — the meta is 256 bytes and entirely deterministic.
3. **Check the booter's DMEM[mbox1] output** — after halt, mb1 was rewritten to (0x7, 0xbba5a000). That's a sysmem PA inside our allocations. It might be a status pointer the booter is signaling for the host to inspect.
