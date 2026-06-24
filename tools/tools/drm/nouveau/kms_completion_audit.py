#!/usr/local/bin/python3
# SPDX-License-Identifier: BSD-2-Clause
"""Aggregate nvkm KMS completion evidence.

This script is intentionally read-only.  It consumes evidence already collected
by kms_smoke.py and writes one machine-readable completion summary.
"""

import argparse
import datetime as dt
import json
import pathlib
import re
import shlex
import subprocess


MODULES = ("drm", "nvgsp_570", "nvkm")
PREFLIGHT_LOAD_MODULE_MARKERS = (
    "sys/dev/drm/drm/drm.ko",
    "sys/dev/drm/nouveau/fw/tu102/nvgsp_570.ko",
    "sys/dev/drm/nouveau/nvkm.ko",
)
PREFLIGHT_STANDALONE_REPORT_MARKERS = (
    ("preflight", "kms_completion_preflight.py", "--output",
     "/var/tmp/nvkm-kms-completion-preflight.json"),
    ("static audit", "kms_static_audit.py", "--output",
     "/var/tmp/nvkm-kms-static-audit.json"),
    ("syncobj_transfer", "kms_smoke.py syncobj_transfer"),
    ("syncobj_pending_exec", "kms_smoke.py syncobj_pending_exec"),
    ("wayland_info", "kms_smoke.py wayland_info"),
    ("wayland_hpd_smoke", "kms_smoke.py wayland_hpd_smoke"),
    ("wayland_glmark", "kms_smoke.py wayland_glmark"),
    ("xwayland", "kms_smoke.py xwayland"),
)
PREFLIGHT_COMPLETION_TEMPLATE_ARGS = (
    "--static-audit /var/tmp/nvkm-kms-static-audit.json",
    "--preflight /var/tmp/nvkm-kms-completion-preflight.json",
    "--full-report <full-report-dir>",
    "--syncobj-transfer <syncobj-transfer-dir>",
    "--syncobj-pending-exec <syncobj-pending-exec-dir>",
    "--wayland-info <wayland-info-dir>",
    "--wayland-hpd-smoke <wayland-hpd-smoke-dir>",
    "--wayland-glmark <wayland-glmark-dir>",
    "--xwayland <xwayland-dir>",
)
WAYLAND_REPORTS = (
    ("wayland_info", "wayland-info"),
    ("wayland_hpd_smoke", "Wayland HPD smoke"),
    ("wayland_glmark", "Wayland glmark2"),
    ("xwayland", "XWayland"),
)
WAYLAND_REQUIRED_CHECKS = {
    "wayland_info": (
        ("nouveau DRM backend", r"^Wayland log uses nouveau DRM backend$"),
        ("atomic DRM", r"^Wayland log uses atomic DRM interface$"),
        ("zink/NVK renderer", r"^Wayland log uses zink/NVK renderer$"),
        ("wayland-info command", r"^wayland-info rc=0$"),
        ("wl_compositor global", r"^wayland-info sees wl_compositor$"),
        ("xdg_wm_base global", r"^wayland-info sees xdg_wm_base$"),
        ("presentation clock", r"^wayland-info reports presentation clock$"),
        ("DRM lease device", r"^wayland-info exposes DRM lease device card0$"),
        ("HDMI output", r"^wayland-info sees HDMI-A-1 output$"),
        ("wayland-info clean output", r"^wayland-info output has no errors$"),
    ),
    "wayland_hpd_smoke": (
        ("nouveau DRM backend", r"^Wayland log uses nouveau DRM backend$"),
        ("atomic DRM", r"^Wayland log uses atomic DRM interface$"),
        ("Wayland compositor clean exit", r"^Wayland compositor rc=0$"),
        ("HPD inject command", r"^kms-hpd-inject\.wayland rc=0$"),
        ("devd hotplug event", r"^wayland devd client received DRM HOTPLUG event$"),
        ("notify-only HPD", r"^wayland hotplug_notify_only_count HPD delta=[1-9][0-9]*$"),
        ("no auto-KMS while Wayland owns master",
         r"^wayland hotplug_auto_kms_count HPD delta=0$"),
        ("user scanout preserved", r"^wayland HPD kept user scanout=1$"),
    ),
    "wayland_glmark": (
        ("nouveau DRM backend", r"^Wayland log uses nouveau DRM backend$"),
        ("atomic DRM", r"^Wayland log uses atomic DRM interface$"),
        ("Wayland compositor clean exit", r"^Wayland compositor rc=0$"),
        ("glmark2 command",
         r"^glmark2-wayland controlled shutdown rc=(0|137|143)$"),
        ("glmark2 started", r"^glmark2-wayland started$"),
        ("glmark2 zink/NVK renderer", r"^glmark2-wayland uses zink/NVK$"),
        ("glmark2 did not crash", r"^glmark2-wayland did not crash$"),
        ("glmark2 present succeeds",
         r"^glmark2-wayland does not fail QueuePresentKHR$"),
        ("Wayland buffer attach",
         r"^glmark2-wayland attaches a Wayland buffer$"),
        ("Wayland damage", r"^glmark2-wayland damages a Wayland surface$"),
        ("Wayland commit", r"^glmark2-wayland commits a Wayland surface$"),
        ("Wayland acquire or implicit release",
         r"^(glmark2-wayland sets explicit acquire point|glmark2-wayland receives wl_buffer release)$"),
        ("Wayland release or implicit release",
         r"^(glmark2-wayland sets explicit release point|glmark2-wayland receives wl_buffer release)$"),
    ),
    "xwayland": (
        ("nouveau DRM backend", r"^Wayland log uses nouveau DRM backend$"),
        ("atomic DRM", r"^Wayland log uses atomic DRM interface$"),
        ("Wayland compositor clean exit", r"^Wayland compositor rc=0$"),
        ("Xwayland server started", r"^Xwayland server started$"),
        ("Xwayland server ready", r"^Xwayland server became ready$"),
        ("X11 surface", r"^Xwayland created an X11 surface$"),
        ("GLX direct rendering", r"^Xwayland glxinfo has direct rendering$"),
        ("GLX zink/NVK renderer", r"^Xwayland glxinfo uses zink/NVK renderer$"),
        ("GLX accelerated", r"^Xwayland glxinfo is accelerated$"),
        ("glxgears output",
         r"^Xwayland glxgears produced renderer/frame/marker output$"),
    ),
}
SYNC_REQUIRED_CHECKS = {
    "syncobj_transfer": (
        ("binary WAIT_DEADLINE",
         r"^PASS SYNCOBJ_WAIT accepts WAIT_DEADLINE$"),
        ("timeline WAIT_DEADLINE",
         r"^PASS SYNCOBJ_TIMELINE_WAIT accepts WAIT_DEADLINE$"),
        ("LAST_SUBMITTED query flag",
         r"^PASS SYNCOBJ_QUERY accepts LAST_SUBMITTED flag$"),
        ("LAST_SUBMITTED point",
         r"^PASS SYNCOBJ_QUERY LAST_SUBMITTED returns submitted point$"),
        ("binary source to timeline",
         r"^PASS SYNCOBJ_TRANSFER binary source to timeline point succeeds$"),
        ("WAIT_FOR_SUBMIT source wait",
         r"^PASS SYNCOBJ_TRANSFER WAIT_FOR_SUBMIT waits for source point$"),
        ("timeline source to binary",
         r"^PASS SYNCOBJ_TRANSFER timeline source to binary succeeds$"),
        ("temporary chain transfer",
         r"^PASS SYNCOBJ_TRANSFER whole temporary chain to timeline succeeds$"),
        ("missing source rejection",
         r"^PASS SYNCOBJ_TRANSFER rejects source with no fence$"),
    ),
    "syncobj_pending_exec": (
        ("nvkm channel allocation",
         r"^PASS nouveau channel alloc succeeds for SYNCOBJ_TRANSFER pending probe$"),
        ("pending source remains unsignaled",
         r"^PASS SYNCOBJ_TRANSFER pending EXEC source remains unsignaled before transfer$"),
        ("WAIT_FOR_SUBMIT copies pending fence",
         r"^PASS SYNCOBJ_TRANSFER WAIT_FOR_SUBMIT copies pending EXEC fence$"),
        ("destination remains pending",
         r"^PASS SYNCOBJ_TRANSFER WAIT_FOR_SUBMIT destination fence remains pending$"),
        ("pending probe fd closes",
         r"^PASS close succeeds for pending EXEC syncobj transfer DRM fd$"),
    ),
}
FULL_REPORT_REQUIRED_CHECKS = (
    ("X11 xrandr", r"^xrandr\.x11 rc=0$"),
    ("X11 glxinfo", r"^glxinfo-B\.x11 rc=0$"),
    ("X11 glxgears",
     r"^glxgears\.x11 rc=(0|124)$"),
    ("X11 zink/NVK renderer",
     r"^glxinfo shows zink/NVK/Vulkan$"),
    ("X11 hardware renderer",
     r"^glxinfo is not software rasterizer$"),
    ("X11 hardware cursor",
     r"^x11 hardware cursor is enabled with fb/bo$"),
    ("X11 async cursor move",
     r"^cursor move used async cursor update delta=[1-9][0-9]*$"),
    ("X11 cursor move avoids primary updates",
     r"^cursor move did not trigger repeated primary plane updates delta=[0-2]$"),
    ("X11 no software cursor",
     r"^Xorg log does not use software cursor$"),
    ("X11 glamor zink/NVK",
     r"^Xorg log has glamor acceleration on zink/NVK$"),
    ("X11 clean exit",
     r"^Xorg terminated successfully$"),
    ("X11 HPD command",
     r"^kms-hpd-inject\.x11 rc=0$"),
    ("X11 devd hotplug event",
     r"^x11 devd client received DRM HOTPLUG event$"),
    ("X11 notify-only HPD",
     r"^x11 hotplug_notify_only_count HPD delta=[1-9][0-9]*$"),
    ("X11 no auto-KMS while master owns display",
     r"^x11 hotplug_auto_kms_count HPD delta=0$"),
    ("X11 panning",
     r"^xrandr-panning\.x11 rc=0$"),
    ("DRM CRTC-in-vblank-event cap",
     r"^drmtest\.after has DRM cap CRTC_IN_VBLANK_EVENT is 1$"),
    ("DRM ADDFB2 modifier cap",
     r"^drmtest\.after has DRM cap ADDFB2_MODIFIERS is 1$"),
    ("DRM PRIME import-export cap",
     r"^drmtest\.after has DRM cap PRIME is 3$"),
    ("legacy primary/cursor planes hidden",
     r"^drmtest\.after has legacy plane resources expose no primary or cursor planes$"),
    ("atomic CRTC properties hidden before cap",
     r"^drmtest\.after has CRTC OUT_FENCE_PTR is hidden before ATOMIC client cap$"),
    ("atomic plane properties hidden before cap",
     r"^drmtest\.after has plane IN_FENCE_FD is hidden before ATOMIC client cap$"),
    ("atomic ioctl unknown flags rejected",
     r"^drmtest\.after has DRM atomic ioctl unknown flags fail with EINVAL$"),
    ("atomic ioctl async flag rejected",
     r"^drmtest\.after has DRM atomic ioctl ASYNC flag fails with EINVAL$"),
    ("atomic ioctl TEST_ONLY event rejected",
     r"^drmtest\.after has DRM atomic ioctl TEST_ONLY event fails with EINVAL$"),
    ("wait-vblank invalid flags rejected",
     r"^drmtest\.after has WAIT_VBLANK unknown type bits fail with EINVAL$"),
    ("page-flip invalid target flags rejected",
     r"^drmtest\.after has PAGE_FLIP both target flags fail with EINVAL$"),
    ("CRTC queue sequence unknown flags rejected",
     r"^drmtest\.after has drmCrtcQueueSequence unknown flags fail with EINVAL$"),
    ("display owner lastclose restore",
     r"^display owner lastclose restore delta=[1-9][0-9]*$"),
    ("display owner lastclose restore clean",
     r"^display owner lastclose restore error delta=0$"),
    ("lastclose last error clean",
     r"^lastclose_restore_last_error=0$"),
    ("DRM lease creates lease",
     r"^drmtest\.after has DRM lease CREATE_LEASE succeeds$"),
    ("DRM lease leased atomic test",
     r"^drmtest\.after has DRM lease atomic TEST_ONLY on leased objects succeeds$"),
    ("DRM lease revoke",
     r"^drmtest\.after has DRM lease REVOKE_LEASE succeeds$"),
    ("DRM lease unknown flags rejected",
     r"^drmtest\.after has DRM lease CREATE_LEASE unknown flags fail with EINVAL$"),
    ("DRM lease unknown flags returns no fd",
     r"^drmtest\.after has DRM lease CREATE_LEASE unknown flags return no lease fd$"),
    ("DRM lease owner full object set",
     r"^drmtest\.after has DRM lease owner GET_LEASE returns full mode object set$"),
    ("DRM lease owner returns every plane",
     r"^drmtest\.after has DRM lease owner GET_LEASE returns every plane$"),
    ("DRM lease list stride",
     r"^drmtest\.after has DRM lease LIST_LESSEES uses u32 lessee id stride$"),
    ("DRM lease implicit cursor plane",
     r"^drmtest\.after has DRM non-universal lease GET_LEASE returns implicit cursor plane$"),
    ("DRM lease lessee revoke denied",
     r"^drmtest\.after has DRM lease lessee REVOKE_LEASE fails with EACCES$"),
    ("DRM lease duplicate live object rejected",
     r"^drmtest\.after has DRM lease duplicate live object is rejected with EBUSY$"),
    ("DRM lease revoked connector hidden",
     r"^drmtest\.after has DRM lease fd has no connector after revoke$"),
    ("DRM lease re-lease after close",
     r"^drmtest\.after has DRM lease object can be re-leased after close$"),
    ("DRM lease closed revoke rejected",
     r"^drmtest\.after has DRM lease closed third lessee revoke fails with ENOENT$"),
    ("connected modes are atomic-checkable",
     r"^drmtest\.after has connected connector mode list passes atomic TEST_ONLY$"),
    ("master connector reprobe triggers fill_modes",
     r"^drmtest\.after has master GETCONNECTOR count_modes=0 triggers connector fill_modes$"),
    ("non-master connector reprobe does not fill_modes",
     r"^drmtest\.after has non-master GETCONNECTOR count_modes=0 does not trigger connector fill_modes$"),
    ("connected EDID checksum",
     r"^drmtest\.after has connected connector EDID block checksums are valid$"),
    ("active MODE_ID matches legacy mode",
     r"^drmtest\.after has active CRTC MODE_ID blob matches legacy mode timings$"),
    ("writeback client cap after atomic",
     r"^drmtest\.after has DRM client cap WRITEBACK_CONNECTORS is accepted$"),
    ("no writeback connector exposed",
     r"^drmtest\.after has .* is not writeback$"),
    ("dithering mode default",
     r"^drmtest\.after has dithering mode defaults to auto$"),
    ("dithering depth default",
     r"^drmtest\.after has dithering depth defaults to auto$"),
    ("fixed max bpc",
     r"^drmtest\.after has max bpc value is 8$"),
    ("underscan default",
     r"^drmtest\.after has underscan defaults to off$"),
    ("underscan hborder default",
     r"^drmtest\.after has underscan hborder value is 0$"),
    ("underscan vborder default",
     r"^drmtest\.after has underscan vborder value is 0$"),
    ("no HDR metadata property",
     r"^drmtest\.after has does not expose unsupported connector property HDR_OUTPUT_METADATA$"),
    ("no Colorspace property",
     r"^drmtest\.after has does not expose unsupported connector property Colorspace$"),
    ("no content type property",
     r"^drmtest\.after has does not expose unsupported connector property content type$"),
    ("no vrr_capable property",
     r"^drmtest\.after has does not expose unsupported connector property vrr_capable$"),
    ("no CRTC VRR_ENABLED property",
     r"^drmtest\.after has does not expose unsupported CRTC property VRR_ENABLED$"),
    ("connector scaling-only commit",
     r"^drmtest\.after has atomic connector scaling-only probe commit succeeds$"),
    ("connector scaling-only preserves underscan",
     r"^drmtest\.after has atomic connector scaling-only probe preserves underscan$"),
    ("connector scaling-only restore",
     r"^drmtest\.after has atomic connector scaling-only restore restores scaling mode$"),
    ("connector underscan-only commit",
     r"^drmtest\.after has atomic connector underscan-only probe commit succeeds$"),
    ("connector underscan-only preserves scaling",
     r"^drmtest\.after has atomic connector underscan-only probe preserves scaling mode$"),
    ("connector underscan-only updates underscan",
     r"^drmtest\.after has atomic connector underscan-only probe updates underscan$"),
    ("connector underscan-only updates border",
     r"^drmtest\.after has atomic connector underscan-only probe updates underscan hborder$"),
    ("connector underscan-only updates vborder",
     r"^drmtest\.after has atomic connector underscan-only probe updates underscan vborder$"),
    ("IN_FORMATS blocklinear",
     r"^drmtest\.after has primary IN_FORMATS has XRGB8888 NVIDIA blocklinear$"),
    ("IN_FORMATS rejects unsupported blocklinear",
     r"^drmtest\.after has primary IN_FORMATS excludes RGB565 NVIDIA blocklinear$"),
    ("modifier negative probe",
     r"^drmtest\.after has ADDFB2 modifier without flag fails with EINVAL$"),
    ("ADDFB2 zero-height rejection",
     r"^drmtest\.after has ADDFB2 zero height fails with EINVAL$"),
    ("ADDFB2 unknown-format rejection",
     r"^drmtest\.after has ADDFB2 unknown pixel format fails with EINVAL$"),
    ("ADDFB2 missing-handle rejection",
     r"^drmtest\.after has ADDFB2 missing plane handle fails with EINVAL$"),
    ("ADDFB2 unused-modifier rejection",
     r"^drmtest\.after has ADDFB2 unused plane modifier fails with EINVAL$"),
    ("legacy ADDFB undersized-pitch rejection",
     r"^drmtest\.after has legacy ADDFB undersized pitch fails with EINVAL$"),
    ("property blob foreign destroy denied",
     r"^drmtest\.after has DRM property blob foreign fd destroy fails with EPERM$"),
    ("property blob destroyed id unreadable",
     r"^drmtest\.after has DRM property blob destroyed id read fails with ENOENT$"),
    ("DUMB unknown flags rejected",
     r"^drmtest\.after has CREATE_DUMB unknown flags fail with EINVAL$"),
    ("DUMB foreign map denied",
     r"^drmtest\.after has MAP_DUMB foreign fd handle fails with ENOENT$"),
    ("DUMB destroyed handle rejected",
     r"^drmtest\.after has DESTROY_DUMB destroyed handle fails with EINVAL$"),
    ("PRIME_HANDLE_TO_FD flags rejected",
     r"^drmtest\.after has PRIME_HANDLE_TO_FD unknown flags fail with EINVAL$"),
    ("vblank wait sequence",
     r"^drmtest\.after has WAIT_VBLANK advances active CRTC sequence$"),
    ("vblank event CRTC id",
     r"^drmtest\.after has drmWaitVBlank event reports active CRTC id$"),
    ("legacy pageflip event",
     r"^drmtest\.after has legacy pageflip to temporary FB pageflip event arrives$"),
    ("legacy pageflip CRTC id",
     r"^drmtest\.after has legacy pageflip to temporary FB pageflip event reports CRTC id$"),
    ("OUT_FENCE_PTR returns sync_file",
     r"^drmtest\.after has OUT_FENCE_PTR returns a sync_file fd$"),
    ("OUT_FENCE_PTR signals",
     r"^drmtest\.after has OUT_FENCE_PTR sync_file becomes readable$"),
    ("IN_FENCE_FD pending source",
     r"^drmtest\.after has IN_FENCE_FD sync_file is pending before atomic commit$"),
    ("IN_FENCE_FD waits",
     r"^drmtest\.after has IN_FENCE_FD sync_file is readable after commit$"),
    ("framebuffer PRIME import",
     r"^drmtest\.after has ADDFB2 accepts PRIME-imported XRGB8888 framebuffer$"),
    ("GETFB2 modifier",
     r"^drmtest\.after has GETFB2 reports linear modifier$"),
    ("non-master mutation denied",
     r"^drmtest\.after has non-master atomic TEST_ONLY commit fails with EACCES$"),
    ("legacy DPMS off",
     r"^drmtest\.after has legacy DPMS OFF commit succeeds$"),
    ("legacy DPMS restore",
     r"^drmtest\.after has legacy DPMS ON restore succeeds$"),
    ("legacy SetCrtc disable",
     r"^drmtest\.after has legacy SetCrtc disable succeeds$"),
    ("legacy SetCrtc restore",
     r"^drmtest\.after has legacy SetCrtc restore succeeds$"),
    ("CRTC color apply",
     r"^drmtest\.after has atomic CRTC color runtime commit succeeds$"),
    ("CRTC color restore",
     r"^drmtest\.after has atomic CRTC color runtime restore commit succeeds$"),
    ("CRTC color invalid LUT rejected",
     r"^drmtest\.after has atomic TEST_ONLY rejects invalid CRTC LUT size with EINVAL$"),
    ("CRTC color programs degamma",
     r"^drmtest\.after has atomic CRTC color runtime commit programs degamma LUT$"),
    ("legacy cursor async move",
     r"^drmtest\.after has legacy cursor MOVE increments cursor_async_update_count$"),
    ("legacy cursor move avoids primary update",
     r"^drmtest\.after has legacy cursor MOVE does not update primary plane$"),
    ("legacy cursor hide clears BO",
     r"^drmtest\.after has legacy cursor hide clears cursor BO$"),
    ("primary panning accepts integer source",
     r"^drmtest\.after has atomic TEST_ONLY accepts integer primary source panning$"),
    ("primary panning rejects fractional source",
     r"^drmtest\.after has atomic TEST_ONLY rejects fractional primary source$"),
    ("cursor TEST_ONLY rejects RGB565",
     r"^drmtest\.after has atomic TEST_ONLY rejects RGB565 linear cursor with EINVAL$"),
    ("console dark-down force-disconnect",
     r"^kms-detect-force-disconnect\.console rc=0$"),
    ("console dark-down HPD unplug",
     r"^kms-hpd-unplug\.console rc=0$"),
    ("console dark-down metadata probe",
     r"^drmtest\.console_dark rc=0$"),
    ("console dark-down force-disconnect clear",
     r"^kms-detect-force-disconnect-clear\.console rc=0$"),
    ("console dark-down HPD plug restore",
     r"^kms-hpd-plug-restore\.console rc=0$"),
    ("console dark-down light-up restore",
     r"^kms-lightup-restore\.console rc=0$"),
    ("console dark-down before state",
     r"^dark-before state captured$"),
    ("console dark-down active state",
     r"^dark-after state captured$"),
    ("console dark-down restore state",
     r"^dark-restore state captured$"),
    ("console dark-down ran",
     r"^dark_down_count delta=[1-9][0-9]*$"),
    ("console dark-down clean",
     r"^dark_down_error_count delta=0$"),
    ("console unplug auto-KMS",
     r"^console unplug auto-KMS delta=[1-9][0-9]*$"),
    ("console restore auto-KMS",
     r"^console restore auto-KMS delta=[1-9][0-9]*$"),
    ("console dark-down no connected connector",
     r"^drmtest\.console_dark has no connected connector exposed when requested$"),
    ("console dark-down no stale modes",
     r"^drmtest\.console_dark has disconnected connector exposes no modes$"),
    ("console dark-down no stale EDID",
     r"^drmtest\.console_dark has disconnected connector EDID is 0$"),
    ("console dark-down no stale CRTC route",
     r"^drmtest\.console_dark has disconnected connector CRTC_ID is 0$"),
    ("console dark-down metadata only",
     r"^drmtest\.console_dark has SKIP active CRTC runtime probes by NVKM_DRMTEST_METADATA_ONLY$"),
    ("console dark-down restore no commit error",
     r"^restore commit_error_count=0$"),
    ("console dark-down restore no HPD enqueue error",
     r"^restore hotplug_enqueue_error_count=0$"),
    ("console dark-down restore no pending audit",
     r"^restore display_audit_pending_valid=0$"),
    ("console dark-down restore scanout",
     r"^console restore scanout_user=0$"),
    ("no new dmesg faults",
     r"^no new dmesg fault lines$"),
    ("after drmtest passed",
     r"^drmtest\.after rc=0$"),
)
STATIC_AUDIT_REQUIRED_FILES = (
    "sys/dev/drm/drm_lease.c",
    "sys/dev/drm/include/drm/drm_lease.h",
    "sys/dev/drm/nouveau/nvkm_drm_kms.c",
    "sys/dev/drm/nouveau/nvkm_gsp_disp.c",
    "sys/dev/drm/nouveau/core/object.c",
    "sys/dev/drm/nouveau/dispnv50/nvkm_dispnv50_bridge.c",
    "sys/dev/drm/nouveau/nvhw/drf.h",
    "sys/dev/drm/nouveau/nvhw/class/clc57d.h",
    "sys/dev/drm/nouveau/nvif/class.h",
    "sys/dev/drm/nouveau/rm/rm.h",
    "sys/dev/drm/nouveau/subdev/gsp.h",
    "tools/tools/drm/nouveau/drmtest.c",
    "tools/tools/drm/nouveau/kms_smoke.py",
    "tools/tools/drm/nouveau/kms_completion_audit.py",
    "tools/tools/drm/nouveau/kms_completion_preflight.py",
    "tools/tools/drm/nouveau/kms_static_audit.py",
)


def read_text(path: pathlib.Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(errors="replace")


def command_return_code(path: pathlib.Path) -> int | None:
    match = re.search(r"^### rc=([0-9]+)$", read_text(path), re.M)
    if match is None:
        return None
    return int(match.group(1))


def load_json(path: pathlib.Path) -> dict:
    with path.open() as json_file:
        return json.load(json_file)


def module_entries(out_dir: pathlib.Path, phase: str) -> list[dict]:
    path = out_dir / f"module_files.{phase}"
    text = read_text(path)
    pattern = re.compile(
        r"^(?P<name>\S+)\s+(?P<path>\S+)\s+size=(?P<size>[0-9]+)\s+"
        r"mtime_utc=(?P<mtime>\S+)\s+sha256=(?P<sha256>[0-9a-f]{64})$",
        re.M,
    )
    entries: list[dict] = []
    for match in pattern.finditer(text):
        entries.append({
            "name": match.group("name"),
            "path": match.group("path"),
            "size": int(match.group("size")),
            "mtime_utc": match.group("mtime"),
            "sha256": match.group("sha256"),
        })
    return entries


def loaded_module_entries(out_dir: pathlib.Path, phase: str) -> dict[str, dict]:
    text = read_text(out_dir / f"kldstat.{phase}")
    pattern = re.compile(
        r"^\s*(?P<id>[0-9]+)\s+(?P<refs>[0-9]+)\s+"
        r"(?P<address>0x[0-9a-fA-F]+)\s+"
        r"(?P<size>[0-9a-fA-F]+)\s+"
        r"(?P<name>\S+)\.ko$",
        re.M,
    )
    entries: dict[str, dict] = {}
    for match in pattern.finditer(text):
        name = match.group("name")
        if name not in MODULES:
            continue
        size_hex = match.group("size")
        entries[name] = {
            "id": int(match.group("id")),
            "refs": int(match.group("refs")),
            "address": match.group("address"),
            "size_hex": size_hex,
            "size_bytes": int(size_hex, 16),
        }
    return entries


def module_git(out_dir: pathlib.Path, phase: str) -> dict:
    text = read_text(out_dir / f"module_files.{phase}")
    git: dict[str, object] = {}
    head = re.search(r"^### git_head=([0-9a-f]{40})$", text, re.M)
    if head is not None:
        git["head"] = head.group(1)
        git["head_short"] = head.group(1)[:10]
    dirty = re.search(r"^### git_tracked_dirty=([01])$", text, re.M)
    if dirty is not None:
        git["tracked_dirty"] = dirty.group(1) == "1"
    git["status_short"] = re.findall(r"^### git_status_short=(.*)$", text, re.M)
    return git


def module_entry_map(entries: list[dict]) -> dict[str, dict]:
    return {
        entry["name"]: entry
        for entry in entries
        if isinstance(entry.get("name"), str)
    }


def module_identity(entry: dict) -> dict:
    return {
        "path": entry.get("path"),
        "size": entry.get("size"),
        "sha256": entry.get("sha256"),
    }


def module_identity_map(entries: list[dict]) -> dict[str, dict]:
    by_name = module_entry_map(entries)
    return {
        name: module_identity(by_name[name])
        for name in MODULES
        if name in by_name
    }


def check(ok: bool, text: str, checks: list[dict]) -> None:
    checks.append({"ok": ok, "text": text})
    print(("PASS " if ok else "FAIL ") + text)


def check_module_set(out_dir: pathlib.Path, phase: str,
                     checks: list[dict]) -> list[dict]:
    entries = module_entries(out_dir, phase)
    by_name = module_entry_map(entries)
    check(bool(entries), f"{phase} module snapshot exists", checks)
    for name in MODULES:
        entry = by_name.get(name)
        check(entry is not None, f"{phase} records {name} module", checks)
        if entry is not None:
            check(entry["size"] > 0, f"{phase} {name} module has size", checks)
            check(bool(re.fullmatch(r"[0-9a-f]{64}", entry["sha256"])),
                  f"{phase} {name} module has sha256", checks)
    return entries


def check_full_report(out_dir: pathlib.Path, checks: list[dict]) -> dict:
    summary_path = out_dir / "report_summary.json"
    check(summary_path.exists(), "full report_summary.json exists", checks)
    if not summary_path.exists():
        return {}

    try:
        summary = load_json(summary_path)
    except (OSError, json.JSONDecodeError) as err:
        check(False, f"full report_summary.json is readable: {err}", checks)
        return {}

    check(summary.get("passed") is True, "full KMS report passed", checks)
    check(summary.get("fail_count") == 0, "full KMS report fail_count is zero",
          checks)
    check(summary.get("allow_missing_x11") is False,
          "full KMS report required X11 phase", checks)

    report_checks = summary.get("checks")
    check(isinstance(report_checks, list), "full report has check list", checks)
    if isinstance(report_checks, list):
        for check_name, pattern in FULL_REPORT_REQUIRED_CHECKS:
            found = any(
                isinstance(item, dict) and
                item.get("ok") is True and
                isinstance(item.get("text"), str) and
                re.search(pattern, item["text"])
                for item in report_checks
            )
            check(found,
                  f"full report proves {check_name}",
                  checks)

    modules = summary.get("modules")
    check(isinstance(modules, dict), "full report has module summary", checks)
    if isinstance(modules, dict):
        for phase in ("before", "x11", "after"):
            entries = modules.get(phase)
            check(isinstance(entries, list),
                  f"full report summary has {phase} modules", checks)
            if isinstance(entries, list):
                by_name = module_entry_map(entries)
                for name in MODULES:
                    check(name in by_name,
                          f"full report summary {phase} has {name}", checks)

    return summary


def check_standalone_report(out_dir: pathlib.Path, label: str,
                            description: str, checks: list[dict]) -> dict:
    check(out_dir.exists(), f"{label} evidence directory exists", checks)
    summary_path = out_dir / "report_summary.json"
    check(summary_path.exists(), f"{label} report_summary.json exists", checks)
    if not summary_path.exists():
        return {}

    try:
        summary = load_json(summary_path)
    except (OSError, json.JSONDecodeError) as err:
        check(False, f"{label} report_summary.json is readable: {err}", checks)
        return {}

    check(summary.get("passed") is True, f"{description} report passed", checks)
    check(summary.get("fail_count") == 0,
          f"{description} report fail_count is zero", checks)
    check(summary.get("allow_missing_x11") is True,
          f"{description} report is standalone", checks)

    modules = summary.get("modules")
    check(isinstance(modules, dict), f"{label} report has module summary", checks)
    if isinstance(modules, dict):
        for phase in ("before", "after"):
            entries = modules.get(phase)
            check(isinstance(entries, list),
                  f"{label} report summary has {phase} modules", checks)
            if isinstance(entries, list):
                by_name = module_entry_map(entries)
                for name in MODULES:
                    check(name in by_name,
                          f"{label} report summary {phase} has {name}", checks)

    report_checks = summary.get("checks")
    check(isinstance(report_checks, list), f"{label} report has check list", checks)
    if isinstance(report_checks, list):
        for check_name, pattern in WAYLAND_REQUIRED_CHECKS.get(label, ()):
            found = any(
                isinstance(item, dict) and
                item.get("ok") is True and
                isinstance(item.get("text"), str) and
                re.search(pattern, item["text"])
                for item in report_checks
            )
            check(found,
                  f"{label} report proves {check_name}",
                  checks)

    return summary


def check_sync_gate(out_dir: pathlib.Path, phase: str,
                    checks: list[dict]) -> dict:
    check(out_dir.exists(), f"{phase} evidence directory exists", checks)
    check_module_set(out_dir, phase, checks)

    drmtest_path = out_dir / f"drmtest.{phase}"
    check(drmtest_path.exists(), f"{phase} drmtest output exists", checks)
    rc = command_return_code(drmtest_path)
    check(rc == 0, f"{phase} drmtest rc={rc}", checks)
    drmtest_text = read_text(drmtest_path)
    for check_name, pattern in SYNC_REQUIRED_CHECKS.get(phase, ()):
        check(bool(re.search(pattern, drmtest_text, re.M)),
              f"{phase} proves {check_name}",
              checks)

    return {
        "out_dir": str(out_dir),
        "phase": phase,
        "drmtest_rc": rc,
        "modules": module_entries(out_dir, phase),
        "module_git": module_git(out_dir, phase),
    }


def check_static_audit(path: pathlib.Path, checks: list[dict]) -> dict:
    check(path.exists(), "static audit summary exists", checks)
    if not path.exists():
        return {"path": str(path)}

    try:
        summary = load_json(path)
    except (OSError, json.JSONDecodeError) as err:
        check(False, f"static audit summary is readable: {err}", checks)
        return {"path": str(path)}

    check(summary.get("passed") is True, "static audit passed", checks)
    check(summary.get("fail_count") == 0,
          "static audit fail_count is zero", checks)
    git = summary.get("git")
    check(isinstance(git, dict), "static audit has git identity", checks)
    if isinstance(git, dict):
        head = git.get("head")
        check(isinstance(head, str) and bool(re.fullmatch(r"[0-9a-f]{40}", head)),
              "static audit git head is a full SHA1", checks)
        check(git.get("tracked_dirty") is False,
              "static audit git tracked worktree is clean", checks)

    files = summary.get("files")
    check(isinstance(files, list) and bool(files),
          "static audit file summary is non-empty", checks)
    if isinstance(files, list):
        by_path = {
            item.get("path"): item
            for item in files
            if isinstance(item, dict)
        }
        for required_path in STATIC_AUDIT_REQUIRED_FILES:
            check(required_path in by_path,
                  f"static audit covers {required_path}", checks)

    return {
        "path": str(path),
        "summary": summary,
    }


def run_git(source_tree: pathlib.Path, args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(source_tree), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def check_current_source_tree(static_summary: dict, checks: list[dict]) -> dict:
    summary = static_summary.get("summary")
    if not isinstance(summary, dict):
        check(False, "current source tree has static audit summary", checks)
        return {}

    source_tree_text = summary.get("source_tree")
    check(isinstance(source_tree_text, str) and bool(source_tree_text),
          "static audit records source tree path", checks)
    if not isinstance(source_tree_text, str) or not source_tree_text:
        return {}

    source_tree = pathlib.Path(source_tree_text)
    check(source_tree.exists(), "static audit source tree path exists", checks)

    head_result = run_git(source_tree, ["rev-parse", "HEAD"])
    check(head_result.returncode == 0, "current source tree git HEAD is readable",
          checks)
    head = head_result.stdout.strip()
    check(bool(re.fullmatch(r"[0-9a-f]{40}", head)),
          "current source tree git head is a full SHA1", checks)

    status_result = run_git(
        source_tree,
        ["status", "--short", "--untracked-files=no"],
    )
    check(status_result.returncode == 0,
          "current source tree git tracked status is readable", checks)
    status_lines = [
        line
        for line in status_result.stdout.splitlines()
        if line.strip()
    ]
    check(not status_lines, "current source tree tracked worktree is clean",
          checks)

    static_git = summary.get("git")
    static_head = static_git.get("head") if isinstance(static_git, dict) else None
    check(isinstance(static_head, str) and head == static_head,
          "current source tree git head matches static audit", checks)

    return {
        "source_tree": str(source_tree),
        "git": {
            "head": head,
            "head_short": head[:10] if re.fullmatch(r"[0-9a-f]{40}", head) else "",
            "tracked_dirty": bool(status_lines),
            "status_short": status_lines,
        },
    }


def command_contains(command: object, *markers: str) -> bool:
    return isinstance(command, str) and all(marker in command for marker in markers)


def check_preflight_command_templates(commands: dict,
                                      checks: list[dict]) -> None:
    load_modules = commands.get("load_modules_after_reboot")
    check(isinstance(load_modules, list),
          "preflight has load module command list", checks)
    if isinstance(load_modules, list):
        check(len(load_modules) == len(PREFLIGHT_LOAD_MODULE_MARKERS),
              "preflight load module command count matches module set", checks)
        for marker in PREFLIGHT_LOAD_MODULE_MARKERS:
            check(
                any(command_contains(command, "doas kldload", marker)
                    for command in load_modules),
                f"preflight load command uses project module {marker}",
                checks,
            )

    full_x11 = commands.get("full_x11_report")
    check(isinstance(full_x11, list),
          "preflight has full X11 report command list", checks)
    if isinstance(full_x11, list):
        check(len(full_x11) == 6,
              "preflight full X11 report has six steps", checks)
        full_x11_steps = (
            ("before phase", ("kms_smoke.py before",)),
            ("startx", ("startx",)),
            ("X11 panning and HPD phase",
             ("DISPLAY=:0", "kms_smoke.py x11", "--run-panning",
              "--run-hpd-inject")),
            ("manual logout", ("logout from X11",)),
            ("after phase with console dark-down",
             ("kms_smoke.py after", "--run-console-dark-down",
              "--dark-down-display-id 0x400")),
            ("report phase", ("kms_smoke.py report",)),
        )
        for index, (label, markers) in enumerate(full_x11_steps):
            command = full_x11[index] if index < len(full_x11) else None
            check(command_contains(command, *markers),
                  f"preflight full X11 step {index + 1} is {label}",
                  checks)

    standalone = commands.get("standalone_reports")
    check(isinstance(standalone, list),
          "preflight has standalone report command list", checks)
    if isinstance(standalone, list):
        check(len(standalone) == len(PREFLIGHT_STANDALONE_REPORT_MARKERS),
              "preflight standalone report command count matches required set",
              checks)
        for label, *markers in PREFLIGHT_STANDALONE_REPORT_MARKERS:
            check(
                any(command_contains(command, *markers)
                    for command in standalone),
                f"preflight standalone command includes {label}",
                checks,
            )

    template = commands.get("completion_template")
    check(isinstance(template, str),
          "preflight has completion command template", checks)
    if isinstance(template, str):
        for marker in PREFLIGHT_COMPLETION_TEMPLATE_ARGS:
            check(marker in template,
                  f"preflight completion template includes {marker}",
                  checks)


def check_preflight_manifest(path: pathlib.Path, static_summary: dict,
                             current_source_tree: dict,
                             checks: list[dict]) -> dict:
    check(path.exists(), "preflight manifest exists", checks)
    if not path.exists():
        return {"path": str(path)}

    try:
        manifest = load_json(path)
    except (OSError, json.JSONDecodeError) as err:
        check(False, f"preflight manifest is readable: {err}", checks)
        return {"path": str(path)}

    check(manifest.get("passed") is True, "preflight manifest passed", checks)
    check(manifest.get("fail_count") == 0,
          "preflight manifest fail_count is zero", checks)

    source_tree = manifest.get("source_tree")
    static_source_tree = static_summary.get("summary", {}).get("source_tree")
    current_source_tree_path = current_source_tree.get("source_tree")
    check(isinstance(source_tree, str) and bool(source_tree),
          "preflight manifest records source tree", checks)
    if isinstance(static_source_tree, str):
        check(source_tree == static_source_tree,
              "preflight source tree matches static audit", checks)
    if isinstance(current_source_tree_path, str):
        check(source_tree == current_source_tree_path,
              "preflight source tree matches current source tree", checks)

    git = manifest.get("git")
    check(isinstance(git, dict), "preflight manifest has git identity", checks)
    if isinstance(git, dict):
        head = git.get("head")
        check(isinstance(head, str) and bool(re.fullmatch(r"[0-9a-f]{40}", head)),
              "preflight git head is a full SHA1", checks)
        check(git.get("tracked_dirty") is False,
              "preflight git tracked worktree is clean", checks)
        static_git = static_summary.get("summary", {}).get("git")
        static_head = static_git.get("head") if isinstance(static_git, dict) else None
        if isinstance(static_head, str):
            check(head == static_head,
                  "preflight git head matches static audit", checks)
        current_git = current_source_tree.get("git")
        current_head = (
            current_git.get("head") if isinstance(current_git, dict) else None
        )
        if isinstance(current_head, str):
            check(head == current_head,
                  "preflight git head matches current source tree", checks)

    project_modules = manifest.get("project_modules")
    check(isinstance(project_modules, list),
          "preflight manifest has project module list", checks)
    if isinstance(project_modules, list):
        by_name = module_entry_map(project_modules)
        for name in MODULES:
            entry = by_name.get(name)
            check(entry is not None,
                  f"preflight records project module {name}", checks)
            if entry is not None:
                check(bool(entry.get("path")),
                      f"preflight {name} module has path", checks)
                check(entry.get("size", 0) > 0,
                      f"preflight {name} module has size", checks)
                check(bool(re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256", ""))),
                      f"preflight {name} module has sha256", checks)

    loaded_modules = manifest.get("loaded_modules")
    check(isinstance(loaded_modules, dict),
          "preflight manifest has loaded module diagnostics", checks)
    if isinstance(loaded_modules, dict):
        for name in MODULES:
            check(name in loaded_modules,
                  f"preflight sees loaded module by name {name}", checks)

    commands = manifest.get("commands")
    check(isinstance(commands, dict),
          "preflight manifest has command templates", checks)
    if isinstance(commands, dict):
        check_preflight_command_templates(commands, checks)

    return {
        "path": str(path),
        "manifest": manifest,
    }


def collect_module_identity_sets(full_summary: dict, transfer_summary: dict,
                                 pending_summary: dict,
                                 wayland_summaries: dict[str, dict],
                                 preflight_summary: dict
                                 ) -> dict[str, dict]:
    identity_sets: dict[str, dict] = {}
    preflight_manifest = preflight_summary.get("manifest")
    if isinstance(preflight_manifest, dict):
        project_modules = preflight_manifest.get("project_modules")
        if isinstance(project_modules, list):
            identity_sets["preflight:project"] = module_identity_map(
                project_modules,
            )

    full_modules = full_summary.get("modules")
    if isinstance(full_modules, dict):
        for phase in ("before", "x11", "after"):
            entries = full_modules.get(phase)
            if isinstance(entries, list):
                identity_sets[f"full:{phase}"] = module_identity_map(entries)

    for label, summary in (
        ("syncobj_transfer", transfer_summary),
        ("syncobj_pending_exec", pending_summary),
    ):
        entries = summary.get("modules")
        if isinstance(entries, list):
            identity_sets[label] = module_identity_map(entries)

    for label, summary in wayland_summaries.items():
        modules = summary.get("modules")
        if not isinstance(modules, dict):
            continue
        for phase in ("before", "after"):
            entries = modules.get(phase)
            if isinstance(entries, list):
                identity_sets[f"{label}:{phase}"] = module_identity_map(entries)

    return identity_sets


def check_module_identity_consistency(identity_sets: dict[str, dict],
                                      checks: list[dict]) -> None:
    baseline = identity_sets.get("full:before")
    check(isinstance(baseline, dict) and len(baseline) == len(MODULES),
          "module identity baseline full:before exists", checks)
    if not isinstance(baseline, dict):
        return

    required_labels = (
        "preflight:project",
        "full:x11",
        "full:after",
        "syncobj_transfer",
        "syncobj_pending_exec",
    ) + tuple(
        f"{label}:{phase}"
        for label, _description in WAYLAND_REPORTS
        for phase in ("before", "after")
    )

    for label in required_labels:
        current = identity_sets.get(label)
        check(isinstance(current, dict) and len(current) == len(MODULES),
              f"module identity set {label} exists", checks)
        if not isinstance(current, dict):
            continue
        for name in MODULES:
            check(current.get(name) == baseline.get(name),
                  f"{label} {name} module identity matches full:before",
                  checks)


def collect_module_git_sets(full_summary: dict, transfer_summary: dict,
                            pending_summary: dict,
                            wayland_summaries: dict[str, dict]
                            ) -> dict[str, dict]:
    git_sets: dict[str, dict] = {}
    full_git = full_summary.get("module_git")
    if isinstance(full_git, dict):
        for phase in ("before", "x11", "after"):
            git = full_git.get(phase)
            if isinstance(git, dict) and git:
                git_sets[f"full:{phase}"] = git

    for label, summary in (
        ("syncobj_transfer", transfer_summary),
        ("syncobj_pending_exec", pending_summary),
    ):
        git = summary.get("module_git")
        if isinstance(git, dict) and git:
            git_sets[label] = git

    for label, summary in wayland_summaries.items():
        module_git_sets = summary.get("module_git")
        if not isinstance(module_git_sets, dict):
            continue
        for phase in ("before", "after"):
            git = module_git_sets.get(phase)
            if isinstance(git, dict) and git:
                git_sets[f"{label}:{phase}"] = git

    return git_sets


def collect_loaded_module_sets(full_dir: pathlib.Path, transfer_dir: pathlib.Path,
                               pending_dir: pathlib.Path,
                               wayland_dirs: dict[str, pathlib.Path]
                               ) -> dict[str, dict]:
    loaded_sets: dict[str, dict] = {}
    for phase in ("before", "x11", "after"):
        entries = loaded_module_entries(full_dir, phase)
        if entries:
            loaded_sets[f"full:{phase}"] = entries

    for label, out_dir, phase in (
        ("syncobj_transfer", transfer_dir, "syncobj_transfer"),
        ("syncobj_pending_exec", pending_dir, "syncobj_pending_exec"),
    ):
        entries = loaded_module_entries(out_dir, phase)
        if entries:
            loaded_sets[label] = entries

    for label, out_dir in wayland_dirs.items():
        for phase in ("before", "after"):
            entries = loaded_module_entries(out_dir, phase)
            if entries:
                loaded_sets[f"{label}:{phase}"] = entries

    return loaded_sets


def check_module_git_consistency(static_summary: dict, git_sets: dict[str, dict],
                                 checks: list[dict]) -> None:
    static_git = static_summary.get("summary", {}).get("git")
    static_head = None
    if isinstance(static_git, dict):
        static_head = static_git.get("head")
    check(isinstance(static_head, str) and bool(re.fullmatch(r"[0-9a-f]{40}", static_head)),
          "static audit git head is available for runtime comparison", checks)

    required_labels = (
        "full:before",
        "full:x11",
        "full:after",
        "syncobj_transfer",
        "syncobj_pending_exec",
    ) + tuple(
        f"{label}:{phase}"
        for label, _description in WAYLAND_REPORTS
        for phase in ("before", "after")
    )

    for label in required_labels:
        git = git_sets.get(label)
        check(isinstance(git, dict) and bool(git),
              f"module git set {label} exists", checks)
        if not isinstance(git, dict) or not git:
            continue
        head = git.get("head")
        check(isinstance(head, str) and bool(re.fullmatch(r"[0-9a-f]{40}", head)),
              f"{label} module git head is a full SHA1", checks)
        check(git.get("tracked_dirty") is False,
              f"{label} module git tracked tree is clean", checks)
        if isinstance(static_head, str):
            check(head == static_head,
                  f"{label} module git head matches static audit", checks)


def write_summary(path: pathlib.Path, summary: dict) -> None:
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    tmp_path.replace(path)


def completion_inputs(script_path: pathlib.Path, static_audit_path: pathlib.Path,
                      preflight_path: pathlib.Path,
                      full_dir: pathlib.Path, transfer_dir: pathlib.Path,
                      pending_dir: pathlib.Path,
                      wayland_dirs: dict[str, pathlib.Path],
                      output: pathlib.Path) -> dict:
    argv = [
        str(script_path),
        "--static-audit", str(static_audit_path),
        "--preflight", str(preflight_path),
        "--full-report", str(full_dir),
        "--syncobj-transfer", str(transfer_dir),
        "--syncobj-pending-exec", str(pending_dir),
        "--wayland-info", str(wayland_dirs["wayland_info"]),
        "--wayland-hpd-smoke", str(wayland_dirs["wayland_hpd_smoke"]),
        "--wayland-glmark", str(wayland_dirs["wayland_glmark"]),
        "--xwayland", str(wayland_dirs["xwayland"]),
        "--output", str(output),
    ]
    return {
        "required_evidence": [
            "static_audit",
            "preflight",
            "full_report",
            "syncobj_transfer",
            "syncobj_pending_exec",
            "wayland_info",
            "wayland_hpd_smoke",
            "wayland_glmark",
            "xwayland",
        ],
        "paths": {
            "static_audit": str(static_audit_path),
            "preflight": str(preflight_path),
            "full_report": str(full_dir),
            "syncobj_transfer": str(transfer_dir),
            "syncobj_pending_exec": str(pending_dir),
            "wayland_info": str(wayland_dirs["wayland_info"]),
            "wayland_hpd_smoke": str(wayland_dirs["wayland_hpd_smoke"]),
            "wayland_glmark": str(wayland_dirs["wayland_glmark"]),
            "xwayland": str(wayland_dirs["xwayland"]),
            "output": str(output),
        },
        "argv": argv,
        "shell_command": " ".join(shlex.quote(item) for item in argv),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Aggregate nvkm KMS completion evidence",
    )
    parser.add_argument("--full-report", required=True,
                        help="Directory containing kms_smoke.py report output")
    parser.add_argument("--syncobj-transfer", required=True,
                        help="Directory from kms_smoke.py syncobj_transfer")
    parser.add_argument("--syncobj-pending-exec", required=True,
                        help="Directory from kms_smoke.py syncobj_pending_exec")
    parser.add_argument("--wayland-info", required=True,
                        help="Directory from kms_smoke.py wayland_info")
    parser.add_argument("--wayland-hpd-smoke", required=True,
                        help="Directory from kms_smoke.py wayland_hpd_smoke")
    parser.add_argument("--wayland-glmark", required=True,
                        help="Directory from kms_smoke.py wayland_glmark")
    parser.add_argument("--xwayland", required=True,
                        help="Directory from kms_smoke.py xwayland")
    parser.add_argument("--static-audit", required=True,
                        help="JSON summary from kms_static_audit.py --output")
    parser.add_argument("--preflight", required=True,
                        help="JSON manifest from kms_completion_preflight.py --output")
    parser.add_argument("--output", default=None,
                        help="Output JSON path; defaults to completion_summary.json in the full report directory")
    args = parser.parse_args()

    full_dir = pathlib.Path(args.full_report)
    transfer_dir = pathlib.Path(args.syncobj_transfer)
    pending_dir = pathlib.Path(args.syncobj_pending_exec)
    wayland_dirs = {
        "wayland_info": pathlib.Path(args.wayland_info),
        "wayland_hpd_smoke": pathlib.Path(args.wayland_hpd_smoke),
        "wayland_glmark": pathlib.Path(args.wayland_glmark),
        "xwayland": pathlib.Path(args.xwayland),
    }
    static_audit_path = pathlib.Path(args.static_audit)
    preflight_path = pathlib.Path(args.preflight)
    output = (
        pathlib.Path(args.output)
        if args.output
        else full_dir / "completion_summary.json"
    )

    checks: list[dict] = []
    static_audit_summary = check_static_audit(static_audit_path, checks)
    current_source_tree = check_current_source_tree(static_audit_summary, checks)
    preflight_summary = check_preflight_manifest(
        preflight_path,
        static_audit_summary,
        current_source_tree,
        checks,
    )
    full_summary = check_full_report(full_dir, checks)
    transfer_summary = check_sync_gate(transfer_dir, "syncobj_transfer", checks)
    pending_summary = check_sync_gate(pending_dir, "syncobj_pending_exec", checks)
    wayland_summaries = {
        label: check_standalone_report(
            wayland_dirs[label],
            label,
            description,
            checks,
        )
        for label, description in WAYLAND_REPORTS
    }
    identity_sets = collect_module_identity_sets(
        full_summary,
        transfer_summary,
        pending_summary,
        wayland_summaries,
        preflight_summary,
    )
    check_module_identity_consistency(identity_sets, checks)
    git_sets = collect_module_git_sets(
        full_summary,
        transfer_summary,
        pending_summary,
        wayland_summaries,
    )
    check_module_git_consistency(static_audit_summary, git_sets, checks)
    loaded_sets = collect_loaded_module_sets(
        full_dir,
        transfer_dir,
        pending_dir,
        wayland_dirs,
    )

    passed = all(bool(item["ok"]) for item in checks)
    summary = {
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "passed": passed,
        "pass_count": sum(1 for item in checks if item["ok"]),
        "fail_count": sum(1 for item in checks if not item["ok"]),
        "failures": [item["text"] for item in checks if not item["ok"]],
        "checks": checks,
        "completion_inputs": completion_inputs(
            pathlib.Path(__file__),
            static_audit_path,
            preflight_path,
            full_dir,
            transfer_dir,
            pending_dir,
            wayland_dirs,
            output,
        ),
        "static_audit": static_audit_summary,
        "preflight": preflight_summary,
        "current_source_tree": current_source_tree,
        "full_report": {
            "out_dir": str(full_dir),
            "report_summary": full_summary,
        },
        "syncobj_transfer": transfer_summary,
        "syncobj_pending_exec": pending_summary,
        "wayland_reports": {
            label: {
                "out_dir": str(wayland_dirs[label]),
                "report_summary": wayland_summaries[label],
            }
            for label, _description in WAYLAND_REPORTS
        },
        "module_identity_sets": identity_sets,
        "module_git_sets": git_sets,
        "loaded_module_sets": loaded_sets,
    }

    try:
        write_summary(output, summary)
    except OSError as err:
        print(f"FAIL write completion summary {output}: {err}")
        return 1

    print(f"INFO wrote {output}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
