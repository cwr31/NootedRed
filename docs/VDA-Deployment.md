# VCN media deployment and validation

Hardware media is enabled by default. `-NRedNoVDA` disables the bridge and its
codec properties; `-NRedNoHEVC` keeps AVC only; `-NRedNoHEVC10` disables Main10
encode and decode through the Main-only policy (which also omits 8-bit Main Still
Picture decode). The old `-NRedVDA` argument is not required. Optional VP9 still requires
`-NRedVP9` and the matching plugin cdhash.

This fork's hardened VA/GVA path currently has one checked-in manifest:
macOS 26.7.2 / **25G332**, cache UUID
`21FB8708-FC8E-3765-BE8C-65D612CDF049`, VA image UUID
`1181E2CA-31B3-3DF0-90E2-3AE70552D753`, AppleGVA image UUID
`CC4FCC4C-C670-3359-A682-BCBAEA945214`.
The early boot identity is kernel UUID `28267FD5-3E1E-3686-8A1C-E73E2466FCFB`.
Other OS builds use graphics without this media bridge until a reviewed manifest
is added. Historical Ventura/Sonoma/Sequoia resolver signatures are derivation
inputs, not permission to patch an unknown image at runtime.

## Build and preflight

```sh
tools/build-branch.sh -y
tools/vda_preflight.sh
```

The artifact is
`build/branches/<branch>/macosx/x86_64/release/NootedRed.kext`.
Preflight checks the current cache and both image UUIDs, all 18 required VA/GVA
sites, eleven page fingerprints and the generated manifest. It submits no GPU work.

`tools/media_patch_manifest.py` reads only image metadata, named VA functions,
AppleGVA's bounded `__text`, and the eleven target pages. It never scans/maps the
entire cache. Site windows accept only their declared original or replacement;
only those windows are zeroed for the page SHA-256. UUIDs are verified in preflight.
Runtime pins the kernel Mach-O UUID before enabling media. `kern.osversion` can
still be empty before launchd publishes it; an empty string alone grants nothing.
Once populated, the build must also match or media is latched off. Runtime checks
the exact cache basename/file offset, every site's
bytes and the masked whole-page SHA-256. No filesystem reads occur in pager hooks.

The checked-in fingerprints were derived from live pages on 2026-11-16, with
the old kext active. They describe the observed bytes outside declared patch
windows; they are **not independent stock-cache evidence**. The generator never
silently reconstructs unknown bytes. For a new build, generate a candidate to a
file outside the repository, review the identities/semantic sites and run host
and post-boot validation before adopting it. Prefer an offline cache or a clean
`-NRedNoVDA` boot for independent stock comparison.

## Install and rollback

Back up the complete working EFI outside the repository before installing.
`tools/deploy_vda.sh --esp /Volumes/EFI` also backs up the previous kext under
ignored `build/efi-backup/` and verifies the installed executable's SHA-256.
It does not reboot or change boot arguments.

To disable media for troubleshooting, add **`-NRedNoVDA`**, then reboot.
Removing `-NRedVDA` does not disable the default-on bridge. The helper supports
`tools/set_vda_bootarg.sh --enable` and `--disable` (with `--esp` as needed).
Use the backup EFI or `-NRedOff` if graphics cannot start, then restore the
previous kext. Keep the paired VP9 plugin when restoring a kext that expects
its exact hash; rebuilding the plugin requires a new paired allowlist.

## Strict post-boot test

```sh
tools/vda_selftest.sh
VDA_TEST_PYTHON=/path/to/python-with-pyav-and-numpy tools/vda_hwtest.sh
```

The diagnostic script does not report a hardware pass. The strict test compiles
`vda_codec_probe.m` into a temporary directory, requires hardware in both
VideoToolbox session specifications and queries both actual `UsingHardware`
properties. Missing dependencies or disabled media return **77 / incomplete**,
never PASS.

The default matrix is 30 frames each:

- H.264 High and HEVC Main 8-bit encode/decode at 1920×1080,
  1918×1080 and 3840×2160;
- independent software-encoded H.264 / HEVC Main / HEVC Main10 1080p input
  with B frames, decoded by hardware;
- HEVC Main10 encode/decode at all three dimensions when
  `NRedHEVC10EncodeAdvertised=1`. Use `tools/vda_hwtest.sh --require-main10`
  to fail before submitting GPU work if this feature is missing after reboot.

The encoder uses its NV12 IOSurface pixel-buffer pool and actual row pitches.
Independent PyAV/FFmpeg software decoding is compared with the hardware output
over effective NV12/P010 pixels, excluding row padding. B frames are ordered by
PTS; duplicate timestamps, dropped/missing frames, unexpected dimensions/format
and any pixel mismatch fail the test. A sampled Y/U/V source reconstruction
check (minimum 30 dB PSNR on this synthetic pattern) also catches pitch/chroma
errors that matching decoders of the same bad encode would miss. Main10 source
samples vary all ten bits; source quality is measured directly in
`yuv420p10le`, without reducing the result to eight bits first.

No generated binaries, media or logs are written into the source checkout.
`--work-dir` selects an external report directory. `results.json` records which
cases passed, failed or were skipped and the running kext identity. A failure
stops the matrix. The test checks engine readiness/resource failures afterward;
with the new kext it also requires full manifest page coverage.

This is bounded correctness validation, not 4K60 throughput, real-content
compression quality, HDR metadata, sleep/wake, concurrent recording/playback or
long-term stability acceptance. CPU generation/readback/hash work is included in
the native timings. Do not call those timings codec FPS benchmarks.

## Readiness and failure behavior

Personality injection requires a supported device, reviewed media manifest,
installed page backend and no latched failure. It intentionally does not require
an allocated engine: injection precedes accelerator start. Page writes require
both prepared drivers, native load completion and engine allocation, but not a
successful first `initHardware`, which may depend on the first userspace session.

Each target page is checked completely before any of its sites is written.
Reclaimed clean pages are always checked/patched again. Global counters and the
observed-page mask are telemetry only. All-pages coverage is not an atomic
transaction across demand-paged images: preflight checks the complete set before
deployment, runtime validates each page when it arrives and latches off new media
work on a mismatch. A page not yet faulted cannot be declared runtime validated.

On kernel preparation/load/allocation/init failure or a rejected target page,
`NRedVCNMediaFailure` records the first reason and new media page writes stop.
Published codec properties are removed from the active accelerator. Existing
bridged objects keep their ABI routing after a late page failure; disabling an
advertisement must not reinterpret their object layouts.

Already patched shared-cache pages and existing clients cannot be made stock or
forced into software merely by changing a boolean. Exit/recreate sessions and
reboot with `-NRedNoVDA` or restore the backup to obtain a clean disabled boot.
This is containment, not a promise that every client can recover without reboot.

Useful properties:

| Property | Meaning |
| --- | --- |
| `NRedUserPatchRevision=4` | bounded page backend with the P010/Main10 encode fixes |
| `NRedMediaKernelValidated=1` | exact kernel Mach-O UUID matched |
| `NRedMediaPatchSupported=1` | exact build and page hook available |
| `NRedMediaPatchPagesExpected=11` | required VA/GVA pages |
| `NRedMediaPatchSitesExpected=18` | required sites |
| `NRedHEVC10EncodeAdvertised=1` | validated Renoir Main10 encode capability published |
| `NRedMediaPatchPagesObserved` | bitmask of validated page indices |
| `NRedMediaPatchComplete=1` | every required page observed at least once |
| `NRedVCNUserPatchesAllowed=1` | allocated bridge may patch media pages |
| `NRedVCNMediaFailure` | first failure; absent/zero on a healthy boot |

Main10 **encode and decode** passed bounded process-local hardware validation on
Renoir 4750U / PCI `0x1636`, using this manifest's exact instruction replacements.
The XML retains conservative Main-only encoding defaults. Runtime adds encode
profile 2 and capability level `"2"` only for that device, the matching media
backend/build and enabled HEVC/Main10 policy.

The encode changes select P010 from the actual source texture's two-byte
elements, set both input bit depth and packing, and signal zero per-slice chroma
QP deltas to match the tested VCN2 output. NV12 and P010 share the same internal
texture enum, so enum-only detection is insufficient. Native GPU source pitch,
10-bit reference-buffer allocation, bitrate and luma QP logic are retained.
Independent per-slice chroma QP deltas are not exposed by this compatibility
path. Main10 support does not imply HDR metadata handling or 4K60 throughput
acceptance. See `analysis/hevc-main10-20261116.md` for the evidence and test scope.

The new EFI kext still requires a reboot and the public-API test afterward.
Process-local tests do not prove that a newly built kext has loaded, that
personality injection ran, or that every new page was patched at boot.
