# System RAM placement for Renoir

On the test Renoir machine, firmware reserves 512 MiB of system memory for the integrated GPU.
The Apple X5000 driver treats that fixed region as VRAM and also has separate allocators for
system RAM. Its existing graphics paths can choose VRAM for new resources or allocate temporary
VRAM when bringing a texture back into use. Under pressure, the driver can try reclaiming memory
before its native fallback redirects a resource to system RAM. In the reference binary,
`IOAccelResource2::fallback()` waits for GPU events and redirects active mappings.

The experimental policy moves eligible new resources into system RAM early and skips selected
temporary VRAM allocations during page-in. The goal is to reduce competition for the fixed pool
and reduce avoidable copies while keeping the native allocation, mapping, and paging logic.
The measurable targets are a correctly rendered working set above the firmware reservation and
fewer bytes copied between the two pools in a comparable workload.

## Boot options

Each option is off by default. See the [test and rollback guide](RAM-Validation.md) for OpenCore
setup and a test matrix. Color and plane format names below refer to the X5000
[`ATIFormat`](../NootedRed/GPUDriversAMD/Accel/HWAlignManager.hpp) enum.

| Option | New allocation | Page-in |
| --- | --- | --- |
| `-NRedRAMBuffers` | Routes new `Data` buffers that prefer VRAM to the native system RAM allocator. | Original driver path. |
| `-NRedRAMTextures` | Routes eligible standard `BGRA8` and `RGBA8` textures, including eligible color attachments, to system RAM. | Skips temporary VRAM for qualifying standard textures when both prepared maps use system RAM. |
| `-NRedRAMHighColor` | Extends that allocation policy to `BGRA16F`, `RGBA16F`, `BGRA1010102`, and `RGBA1010102`. | Covers qualifying standard and IOSurface resources in those formats. |
| `-NRedRAMVideoPlanes` | Extends that allocation policy to `R8`, `Rg8`, `R16`, and `Rg16` textures. | Covers qualifying standard and IOSurface resources in those formats. |
| `-NRedRAMDepthStencil` | Routes qualifying standard `Depth16`, `Depth24Stencil8`, `Depth32`, `Depth32F`, `Depth32fStencil8`, and `Stencil8` resources on the depth/stencil path to system RAM. | Original driver path. |
| `-NRedRAMIOSurfaces` | Original driver path for IOSurface creation. | Skips temporary VRAM for qualifying `BGRA8` and `RGBA8` IOSurfaces when both prepared maps use system RAM. |

The video-plane option operates on texture formats rather than a Core Video pixel-buffer
identifier or codec. NV12 and P010 plane views are relevant workloads. Direct allocation of
hardware decoder and encoder surfaces is handled separately by the VCN driver.

## How the hooks work

`NRed::init()` calls `X5000::init()` to read the options. Options affecting new allocations
register a Lilu load handler for `IOAcceleratorFamily2`. The X5000 load handler verifies
its Mach-O identity and installs the page-in hook when requested. The per-object selector
checks that the accelerator is attached to the integrated PCI device detected by NootedRed
and belongs to `AMDRadeonX5000_AMDVega10GraphicsAccelerator` on a device marked as Renoir.
See [NRed.cpp](../NootedRed/NRed.cpp), [Kexts.cpp](../NootedRed/Kexts.cpp), and
[X5000.cpp](../NootedRed/X5000.cpp).

| Hook | Policy and continuation |
| --- | --- |
| `IOAccelResource2::allocDataBufferMemory()` | Wraps the original method and passes `preferVRAM=false` for an eligible resource. The selector requires type `0x0A`, a shared context, an original VRAM preference, and an empty primary memory slot. The driver's original method performs allocation and returns its result. |
| `IOAccelResource2::allocMemory()` | At the allocation branch, it selects the driver's native system-memory path for eligible type `0x40` resources with empty primary and backing memory slots. For color and plane formats, the surface parameter pointer must be empty. High-precision and plane formats also require empty auxiliary slots. Depth and stencil formats instead require the matching depth/stencil copy path. Other requests follow the original allocation branch. |
| `AMDRadeonX5000_AMDAccelResource::pageon()` | Evaluates the texture after both maps have been prepared. For copy kind `8`, with texture flag bit `8` clear, it can skip the temporary VRAM map when both maps reference `IOAccelSysMemory`. It rejoins the driver's normal `pageTexture()` logic, which can still perform layout or format copies. Each map may refer to a distinct system memory object. |

The IOSurface selector uses the `0x82` resource type or a shared resource with a surface
reference. The 8-bit IOSurface option permits shared type `0xC0`. The high-color and video-plane
options also permit shared type `0x80`. All page-in paths require valid maps and their respective
format and option checks.

## Reference images and ABI

The offsets below are relative to the named functions or objects in the x86_64 macOS Tahoe
26.7.2 kernel collection, build `25G332`. Kext UUIDs identify the driver images, independently
of the macOS version string.

| Driver | Reference Mach-O UUID |
| --- | --- |
| `com.apple.iokit.IOAcceleratorFamily2` | `6C4A16B1-66DC-3A8B-9F0B-D3E027B16EB1` |
| `com.apple.kext.AMDRadeonX5000` | `E27EFB64-DE35-3CE5-B3AB-A32218B495D3` |

| Path | Reference sites and checks |
| --- | --- |
| Data buffer wrapper | Checks the function entry, existing-memory branch at `+0x89`, and native allocation sequence at `+0xA8`. Resolves the relative call and verifies that it targets `IOAccelSysMemory::withOptions()`. |
| Texture branch | Requires both reference UUIDs for selection. Checks the instructions at `allocMemory()+0x27` and the system RAM continuation at `+0xDA`. The trampoline preserves the original bit-21 decision for other requests and resumes their VRAM path at `+0x37`. For eligible requests it clears bit 26 in the local register to select the direct system-memory branch. The shared accelerator configuration field is read from object offset `0xC90`. |
| Surface branch | Requires the reference X5000 UUID. Checks `pageon()+0x143` and the reuse target at `+0x360`. The trampoline reconstructs the texture flag calculation and resumes the original decision at `+0x152` when the selector declines. |

The resource type, owning accelerator, backing memory, and primary memory fields are at `0x14`,
`0x18`, `0x80`, and `0x88`. The texture flags at `0x1B4` carry the format in their low byte and
the copy kind in bits `26–29`. Surface parameters are at `0x180` and the auxiliary fields
checked for the high-color and plane policy are at `0x188`, `0x190`, `0x198`, and `0x1A8`.
A shared resource uses `0xE0` for its surface reference. A prepared map uses `0x18` for its
memory object.

The two hooks inside driver functions rely on the live registers and stack layout of these images.
At the allocation site, `r14` carries the resource and `rdi` carries the accelerator. At
the page-in site, `r14` carries the resource while `r13` and `r12` carry its prepared maps.
When porting, recheck the register and stack contract, branch destinations, virtual dispatch,
object fields, and native allocation flags from the target x86_64 slice. Compare the loaded
images and instruction signatures before updating the guards. An identity or signature check
failure leaves the corresponding original driver path in use.

## Remaining VRAM use and limits

Display scanout, cursor and display firmware reservations, some GPU virtual-memory allocations,
and internal blit buffers have separate allocation paths. Unsupported formats, independent
compression metadata, and other texture routes continue through the native driver. The
system-memory wire limit and GPU address-space availability also constrain the workloads
that can stay in RAM. The firmware reservation and overall UMA budget remain under the
original driver policy.

Future work should measure placement per resource and record page-in and copy bytes before
widening the format or hardware coverage. The functional smoke results and the separate
placement and capacity acceptance criteria are in the [test guide](RAM-Validation.md).
