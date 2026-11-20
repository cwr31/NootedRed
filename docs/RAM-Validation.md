# Renoir RAM testing and rollback

The boot options in [RAM-Placement.md](RAM-Placement.md) are experimental. The reference
configuration is macOS Tahoe 26.7.2 build `25G332` on x86_64 Renoir with a 512 MiB UMA
reservation. The [placement design](RAM-Placement.md#reference-images-and-abi) lists the
expected Apple driver UUIDs.

## Build and enable

Build the branch using the normal xmake workflow and its AppleForge dependencies.
See [Build-Artifacts.md](Build-Artifacts.md) for the output location and
[the workflow](../.github/workflows/main.yml) for dependency setup.

```sh
tools/build-branch.sh -y
```

Keep a copy of your current `NootedRed.kext` and OpenCore `config.plist`, and keep a known
working boot entry available. Replace `NootedRed.kext` on the EFI used for boot and retain its
`Kernel → Add` entry after Lilu. Append the desired flags to
`NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`, preserving the existing
arguments. Reboot from that EFI. The full test set uses these flags.

```text
-NRedRAMBuffers -NRedRAMTextures -NRedRAMHighColor -NRedRAMVideoPlanes -NRedRAMDepthStencil -NRedRAMComposition
```

Start with a boot with the switches off for a baseline. Compare each option by itself or use
the full set for an integration check. Confirm the effective args and loaded driver identities.

```sh
sysctl -n kern.bootargs
kmutil showloaded --list-only | awk '/NootedRed|IOAcceleratorFamily2|AMDRadeonX5000 /'
```

## Check hook selection and output

The `RAMBuffers` and `RAMResources` tags report installation results and the first matching
resource. For example, `Texture allocation route installed` confirms the detour installed.
`Selected system RAM path` for a texture reports a selector decision just before native
allocation. `Allocated first Renoir data buffer in system RAM` reports the first
successful redirected buffer allocation. Boot arguments alone establish configuration.
On the reference boot, the available kernel logs did not show route-installation or selection
messages, so placement remains unconfirmed.

```sh
log show --last boot --info --style compact \
  --predicate 'process == "kernel" AND (eventMessage CONTAINS "RAMBuffers" OR eventMessage CONTAINS "RAMResources")'
```

| Path | GPU workload and expected result |
| --- | --- |
| Buffers | Create a workload that reaches `allocDataBufferMemory()`. Use known GPU input data and compare the GPU-written values read back by the CPU. Correlate with the buffer hook because individual Metal buffers can take different allocation paths. |
| Ordinary and high-color textures | Render a known color into a private texture, sample it in a separate GPU operation, and compare the readback. Repeat with different colors for `BGRA8`, `RGBA8`, `RGBA16Float`, `RGB10A2Unorm`, and `BGR10A2Unorm`. |
| IOSurface and video planes | Create an IOSurface-backed `CVPixelBuffer`, fill distinct known channel values, and create Metal views with `CVMetalTextureCacheCreateTextureFromImage`. Check BGRA8 and both luma/chroma views for NV12 (`R8`/`RG8`) and P010 (`R16`/`RG16`). Compare GPU readback against the expected normalized values. |
| Depth and stencil | Clear depth to `0.5` and stencil to `7`. Draw a red triangle at depth `0.25` with stencil reference `7`, then a green triangle at depth `0.375` and a blue one at depth `0.125` with reference `6`. The final color should be red. Check both packed formats on devices that support them. |

On November 19, 2026, all six flags were present after reboot on the reference machine.
The x86_64 NootedRed 1.2.8 test kext (`56811552-E94C-38C9-B68E-2FE8987EF309`) loaded
with Lilu 1.7.3. The following Metal checks passed in that boot.

- Four render, sample, and readback iterations per format on 1024×1024 textures for all five color formats in the table.
- BGRA8 IOSurface and both planes of NV12 and P010.
- `Depth32Float_Stencil8` and `Depth24Unorm_Stencil8` with both comparisons exercised.

The build passed strict code signature verification. Local checks compared the guarded
instructions and UUIDs against the reference kernel collection and exercised the selection
and branch continuations in x86_64 harnesses.

## Measure placement and capacity

1. Compare the same workload with the options off and on. Instrument successful allocation
   and map preparation with a resource identifier, requested bytes, backing class, and format.
   Count temporary VRAM allocations and copies during repeated use.
2. Keep a set of textures alive at the same time while clearing and sampling each one.
   Re-read the earliest textures after filling the rest. For example, 20 BGRA8 textures at
   4096×2048 account for 640 MiB of texel data. Allocation, correct readback, and actual
   residency are separate measurements. Successful readback can also result from native paging.
3. Confirm that the same resources use system RAM and that fixed-pool residency and copy bytes
   decrease relative to the baseline. The two native counters in `IOGraphicsAccelerator2`
   distinguish locked system and video memory. Keep per-resource evidence when the workload
   and desktop are sharing those counters.

The November 19, 2026 follow-up boot loaded build `1815B079-4FFF-38EB-863E-B093CF63F464`.
Allocation traces showed 39 private textures across 13 formats, 20 large private textures,
and 20 Metal private buffers taking the new RAM path. A separate trace followed two RAM
allocations through wiring and into the GPU page table. The 633 MiB texture workload passed
eight timed full-image verification rounds. The 640 MiB buffer workload passed 100 rounds.
The shared and managed IOSurface readback tests also passed with the native cache allocator.

The physical 3840×2160 display stayed online. Three animated 2112×1188 windows presented
at about 42–43 FPS each. With three 3456×1944 windows they presented at about 17 FPS each.
The larger run still showed IOSurface paging and VRAM eviction waits. The tests added no
panic or GPU restart report. The window results describe this boot and workload. They do
not establish a controlled comparison against the previous kext.

On November 20, 2026, build `DA95A878-3A09-3F6C-A653-A2DD6F906CF3` booted with
`-NRedRAMComposition` on the same 4K 60 Hz display. Both IOSurface readback tests and
the 633 MiB private texture test passed eight timed rounds each. No GPU recovery or
new panic report appeared during the runs.

With three animated 3456×1944 windows, eight composition caches of about 27 MiB each
were allocated in system RAM. The trace followed the new allocations into the GPU
page tables. The native flip path still requested physical addresses from VRAM.
The test did not exercise a RAM cache later becoming eligible for direct output.

Window presentation with two drawables per window was about 16–18 FPS per window
across the new runs, versus about 18 FPS in the prior run with the same parameters.
The old trace counted 313 calls to reclaim VRAM during the test. The new trace counted
zero, and roughly 286–298 MiB of the reserved VRAM stayed free. In an untraced repeat,
the application command buffers took 1.3–1.5 ms to execute at the 95th percentile,
after about 90–101 ms of waiting between scheduling and GPU execution. These are
application measurements. They do not isolate where the compositor or GPU spends
that interval. Raising the test app's drawable count to three produced 18.5–19.5
FPS per window in two runs, with a longer delay before presentation.

## 4K scanout layout

On November 20, 2026, build `96A5B931-165B-34C0-9323-1CD501783425` loaded on
the Renoir 4750U with macOS 26.7.2 (`25G332`) and a 3840×2160 60 Hz display.
With `-NRedScanoutLayout`, the native display flip path resolved layout `0x19` buffers of
33,816,576 bytes with DCC metadata present. The layout has not been measured on its own.

The same build carried two other experimental switches that were later dropped because
neither changed the result. A shared linear `Rg8` plane path never triggered: the observed
plane requested 4,587,520 bytes for primary storage, while its RAM backing was 3,735,552
bytes with a 3,136 byte adjustment. A GFX clock floor of 550 MHz raised the lowest sampled
clock from 400 to 550 MHz, and the median stayed at the 1,600 MHz maximum.

The 61 second combined test played 4K video while exercising Mission Control,
show desktop, minimize, restore, and scrolling. All 56 actions completed.
The player reported 763 dropped frames out of 3,594 (21.23%), versus
757/3,592 (21.07%) on the earlier build with the ineffective clock boost and
700/3,604 (19.42%) on the earlier linear RAM build. The revised run used up
to 1,191.7 MiB of system RAM for GPU resources, and the reserved VRAM pool
stayed at least 248.7 MiB free. It recorded zero VRAM reclaim calls, eviction
waits, surface or texture page outs, and GPU recoveries. System swap usage
stayed at zero. Neither run on the revised build produced a new panic report.

In a separate repeat of the 35 action display capture test, the 95th
percentile gap between display transaction completions during the actions
was 50.01 ms, versus 66.66 ms on the earlier composition build
(`DA95A878-3A09-3F6C-A653-A2DD6F906CF3`). The worst gap increased from
100.03 to 116.70 ms. In the revised run, 54 of the 62 gaps of at least
49 ms had display submission spacing longer than half the completion gap.
This comparison spans different boots and several driver changes, so it
does not establish the effect of an individual switch.

The revised run reports are in `build/desktop-tuning/postboot/revised`
and `build/desktop-tuning/postboot/revised-capture`.

## Roll back

Remove the `-NRedRAM*` arguments added for this experiment and reboot to use the stock
allocation paths in this kext. The scanout switch can be removed independently for a
comparison after reboot. If you need to restore the prior driver, use the known working
boot entry to access the affected EFI, restore the saved `config.plist` and `NootedRed.kext`,
and reboot from that EFI. Verify the effective boot arguments and the loaded kext with the
commands above.
