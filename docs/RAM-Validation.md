# Renoir RAM testing and rollback

The six boot options in [RAM-Placement.md](RAM-Placement.md) are experimental. The reference
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
-NRedRAMBuffers -NRedRAMTextures -NRedRAMHighColor -NRedRAMIOSurfaces -NRedRAMVideoPlanes -NRedRAMDepthStencil
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
allocation or page-in. `Allocated first Renoir data buffer in system RAM` reports the first
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

The recorded smoke runs establish successful output. A measured working set above 512 MiB,
actual backing placement, copy reduction, and changes in desktop or editor performance
remain open acceptance checks.

## Roll back

Remove the `-NRedRAM*` arguments added for this experiment and reboot to use the stock
allocation paths in this kext. If you need to restore the prior driver, use the known working
boot entry to access the affected EFI, restore the saved `config.plist` and `NootedRed.kext`,
and reboot from that EFI. Verify the effective boot arguments and the loaded kext with the
commands above.
