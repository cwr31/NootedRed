# Renoir AVC / HEVC / VP9 capability expansion

Target: Ryzen 7 PRO 4750U, PCI `1002:1636`, VCN 2.2, macOS 26.7.2
(`25G332`). These results describe tested driver behavior, not a guarantee
of every feature or the peak frame rate of every video.

## Code changes

Revision 5 adds four reviewed resolution-check instruction windows to the
existing media manifest: three for HEVC and one for AVC. The resulting manifest
has **12 pages and 22 sites**. Every original page outside the new capability
page is unchanged. Kernel/cache/image identities and complete masked page
fingerprints remain required.

| Path | Previous restriction | New bounded behavior |
| --- | --- | --- |
| AVC decode | Width 4096, height 2304, 9,437,184 pixels | Height up to 4096; existing width, minimum, profile and pixel checks retained |
| HEVC Main/Main10 decode | Ordinary-client width check reads the 4096-wide field | Reads the existing 8192-wide field; height 4352 and 35,651,584-pixel maximum |
| HEVC decode advertisement | Level 5.1 (`153`) | Level 6.2 (`186`), only on the reviewed Renoir build/backend |
| VP9 profile 0/2 decode | 4096x2304 | Width 8192, height 4352, at most 35,389,440 pixels |

`-NRedNoVDA`, `-NRedNoHEVC` and `-NRedNoHEVC10` retain their policies.
Main-only decode can use the larger HEVC size; removed ten-bit profiles are
not reintroduced. `NRedHEVC8KDecodeAdvertised` is cleared after a bridge failure,
and effective capabilities are withdrawn as before.

The VP9 session maximum rectangle and coded-frame validation have separate
purposes: a session can contain either wide or tall valid streams, while every
coded frame must satisfy the pixel budget. Invalid format descriptions are
rejected before VCN creation, and frames are checked again before submission.
Its large tier-0 reference allocation keeps the existing 8K formula and ten-bit
multiplier. No GL budget, source-pitch divisor, GPU clock or power policy is
changed by this revision.

The VP9 parser additionally records the previous frame type. A keyframe has no
motion vectors to reuse, even though its `intra_only` syntax field is zero.
The new condition matches libvpx 1.15.2's `use_prev_frame_mvs` rule.

## Hardware correctness checks before installation

The currently booted kext was revision 4. New AVC/HEVC instructions were tested
only in isolated processes using the original Apple decoder factories, with
explicit GPU selection and hardware-required VideoToolbox sessions. The HEVC
test process also raised its capability level. The final VP9 bundle was loaded
in-process with its exact dynamic decoder ID. These tests do not replace
post-reboot testing of the system decoder service.

| Test | Frames | Result |
| --- | ---: | --- |
| HEVC Main, 7680x4320 | 3 | Hardware; every effective-pixel hash matches CPU |
| HEVC Main10, 7680x4320 | 3 | Hardware; every effective-pixel hash matches CPU |
| HEVC Main, 8192x4352 | 30 | Hardware; every effective-pixel hash matches CPU |
| HEVC Main10, 8192x4352 | 3, then 30 | Hardware; every effective-pixel hash matches CPU |
| AVC, 2304x4096, including B frames | 3, then 30 | Hardware; every effective-pixel hash matches CPU |
| VP9 profile 0, 1920x1080 / 3840x2160 | 30 each | Hardware; every frame matches CPU |
| VP9 profile 2, real ten-bit 3840x2160 | 3 | Hardware; every frame matches CPU |
| VP9 profiles 0 and 2, 7680x4320 | 30 each | Hardware; every frame matches CPU |
| VP9 profile 0, 8192x4320 / 7680x4352 | 3 each | Hardware; every frame matches CPU |
| VP9 profile 2, FATE ten-bit sample | 10 | Hardware; every frame matches reference |

The final VP9 binary reruns the supported cases and verifies that the rejected
8192x4352 cases fail at session creation. New streams, logs, binaries and all
experiments live outside the repository under:

```text
installer-staging/verification/gpu-status-2026-11-16/media-max-capability/
```

## Encoding and throughput

Public VideoToolbox sessions under the running revision 4 passed 30-frame
encode/decode tests at **4096x2304** for:

* H.264 Baseline with CAVLC, Main with CABAC, and High with CABAC.
* HEVC Main and Main10, the latter using source values that vary all ten bits.

All 150 hardware-decoded output frames match independent CPU decoding.
Independent reconstruction against the source patterns also passes; minimum
sampled per-plane PSNR is 38.694 dB for AVC, 48.985 dB for HEVC Main, and
47.761 dB for HEVC Main10. PPS inspection confirms the requested AVC entropy
modes. These encoder paths need no new format patch in this revision.

A separate 300-frame test reuses eight synthetic source buffers, keeps the
hardware requirement, and skips decoder pixel readback:

| 4096x2304 path | Encode fps, including first eight source fills | Decode fps without pixel readback |
| --- | ---: | ---: |
| AVC High | 49.98 | 122.50 |
| HEVC Main | 38.18 | 96.28 |
| HEVC Main10 | 22.61 | 65.46 |

VP9's short repeated ten-bit samples measured 77.6 fps at 3840x2160 and
12.7 fps at 7680x4320 without pixel hashing. Content, keyframe frequency,
shared power and memory bandwidth, concurrent GPU work and presentation
overhead all matter. These measurements are not sustained peak guarantees,
nor proof that a browser presents every frame smoothly.

## Remaining gaps and rejected experiments

This revision **does not claim complete hardware feature parity**.

1. **VP9 8192x4352:** the first frame is correct, but 29/30 frames in the
   eight-bit test have inter-frame chroma errors. A two-frame pixel comparison
   finds correct luma and errors near the bottom of the chroma planes.
   Increasing allocation height, increasing capacity to twelve pictures,
   padding reference height and changing reference chroma pitch do not fix it.
   Those experimental changes are absent from the shipped code. The validated
   pixel guard rejects this case; standard UHD 8K works at both bit depths.
2. **AVC 4096x4096 / Level 6:** increasing the pixel budget makes the API
   report hardware success, but all three output frames are identical blank
   images and fail CPU comparison. That pixel-budget patch is rejected.
   Portrait Level-5.2 streams work with only the height fix and the existing
   9,437,184-pixel budget.
3. **AVC multi-slice:** `NumberOfSlices=4` returns success, but PPS/NAL
   inspection finds one slice per frame. Setter success is not feature
   support. This is still a driver-integration gap; no speculative slice
   command or header patch is included.
4. Encoder tests cover the 4096x2304 boundary and the listed profiles. Taller
   encode dimensions, additional rate-control modes, low-latency behavior,
   HDR metadata, sleep/wake, multi-session contention and sustained thermal
   throughput are not established by these tests.

## Host regression checks

```sh
python3 tools/media_patch_manifest.py
python3 tools/test_codec_decode_limits.py
python3 tools/test_hevc_encode_params.py
python3 tools/test_hevc_advertisement.py
python3 tools/test_media_policy.py
python3 tools/test_userpatch_lifecycle.py
python3 Media/VP9/test_message.py
python3 Media/VP9/test_policy.py
python3 Media/VP9/test_library_validation.py
```

The dimension test executes the actual original/patched capability function
with mock objects and a stubbed lock-status helper. The VP9 test executes its
actual message builder, checks pitches, buffer bounds and ten-bit fields, tests
keyframe/inter-frame motion-vector state, and verifies pre-submission rejection
of the unsupported corner. Neither host test submits GPU work.

## Installation and post-reboot acceptance

The kext and VP9 bundle must be rebuilt and installed together because the
allowlist contains the exact signed bundle's cdhash. Full EFI and old-bundle
backups, installed-file hashes and a rollback record are kept outside Git.

The final pair was installed at **2026-11-16 19:52:35 +08:00**:

* Kext UUID: `1101EE82-B3CB-39AA-B789-B23C2DD03CF1`.
* Kext binary SHA-256:
  `26e2891b7e9320c32d81356c5a85ca8949d0f1ea93bbac36eeec3d8d0a085bf5`.
* VP9 binary SHA-256:
  `92654562f2000ea95fd7ff8ade20fc10209a3b3f573e4a036b6c9e505cd96af9`.
* Paired plugin cdhash: `8e96e451ec50cf755058d1941c43742f1974962b`.

Both installed trees match the tested build byte for byte. The complete EFI
and previous plugin backup is
`installer-staging/backups/20261116-195233-before-media-revision5/`.
Hashes of every other EFI file and system video plugin are unchanged.
The final plugin passed its ten-case GPU matrix and both signature-policy
regressions after removing experimental tracing code. The current boot still
uses revision 4; system-wide acceptance of revision 5 is pending a reboot.

After reboot, require revision 5, the new kext UUID, 12 observed manifest pages
(`4095`), 22 expected sites, `NRedHEVC8KDecodeAdvertised=1`, and HEVC levels
`186`. Verify the installed plugin hash against its paired allowlist, then run
fresh public-API sessions **without** a local factory, injected capability
dictionary, debugger code patch or `-load` bundle override. Check GPU recovery,
VCN resource failures and new crash reports as well as frame correctness.
