# System RAM placement for Renoir

On the test Renoir machine, firmware reserves 512 MiB of system memory for the integrated GPU.
The Apple X5000 driver treats that fixed region as VRAM and also has separate allocators for
system RAM. Its existing graphics paths can choose VRAM for new resources or allocate temporary
VRAM when bringing a texture back into use. Under pressure, the driver can try reclaiming memory
before its native fallback redirects a resource to system RAM. In the reference binary,
`IOAccelResource2::fallback()` waits for GPU events and redirects active mappings.

The experimental policy moves eligible new resources into system RAM when they are first
allocated. The goal is to reduce competition for the fixed pool while keeping the native
allocation, mapping, and paging logic. The measurable target is a correctly rendered working
set above the firmware reservation.

## Boot options

Each option is off by default. See the [test and rollback guide](RAM-Validation.md) for OpenCore
setup and a test matrix. Color and plane format names below refer to the X5000
[`ATIFormat`](../NootedRed/GPUDriversAMD/Accel/HWAlignManager.hpp) enum.

| Option | New allocation |
| --- | --- |
| `-NRedRAMBuffers` | Routes new `Data` buffers that prefer VRAM to the native system RAM allocator. |
| `-NRedRAMTextures` | Prefers RAM for eligible standard private textures and Metal buffers during their first primary allocation. Also covers eligible standard `BGRA8` and `RGBA8` textures after format initialization. |
| `-NRedRAMHighColor` | Extends that allocation policy to `BGRA16F`, `RGBA16F`, `BGRA1010102`, and `RGBA1010102`. |
| `-NRedRAMVideoPlanes` | Extends that allocation policy to `R8`, `Rg8`, `R16`, and `Rg16` textures. |
| `-NRedRAMDepthStencil` | Routes qualifying standard `Depth16`, `Depth24Stencil8`, `Depth32`, `Depth32F`, `Depth32fStencil8`, and `Stencil8` resources on the depth/stencil path to system RAM. |
| `-NRedRAMComposition` | Prefers RAM for new `BGRA8` and `RGBA8` device caches without direct display eligibility. With `-NRedRAMVideoPlanes`, it also covers non-display `Rg8` IOSurface planes on the linear copy path with no device cache, auxiliary storage, or DCC metadata. |

Page-in always follows the original driver path. Earlier builds also had a page-in route for
the color, high-color, and plane options, and a `-NRedRAMIOSurfaces` option that only fed that
route. The route never changed the copy path, as [described below](#page-in), so both were
removed. The `-NRedRAMIOSurfaces` argument is now ignored.

The following Renoir experiments have independent switches.

| Boot option | Behavior | Check on hardware |
| --- | --- | --- |
| `-NRedScanoutLayout` | Requests a display compatible layout for new 4K or larger 32-bit single plane scanout surfaces. AddrLib must accept the layout and the display driver still decides whether to use DCC. | Recreate display buffers and verify the layout, metadata and visible output. |
| `-NRedRAMBudget` | Reports the fixed pool plus half of the wired system memory limit as the video memory of Metal devices, while `-NRedRAMTextures` is enabled. See [Metal working set](#metal-working-set). | Start a new Metal process and compare `recommendedMaxWorkingSetSize` before and after. |

The video-plane option operates on texture formats rather than a Core Video pixel-buffer
identifier or codec. NV12 and P010 plane views are relevant workloads. Direct allocation of
hardware decoder and encoder surfaces is handled separately by the VCN driver.

## Runtime switch and counters

Once a placement hook is installed, `sysctl debug.nred.ram` shows the runtime state. Writing
`debug.nred.ram.enabled` as root changes the classes used for new allocations without a reboot.

| Name | Meaning |
| --- | --- |
| `enabled` | Bit mask of the classes used for new allocations. It starts with the classes requested by boot arguments. |
| `available` | Classes whose hooks are installed. `enabled` rejects other bits with `EINVAL`. |
| `<class>_count`, `<class>_bytes` | Allocations placed in system RAM since boot, and their size rounded up to whole pages. |
| `declined_<reason>_count`, `declined_<reason>_bytes` | Textures that an enabled class would have taken, except for one property that keeps them native. See [Textures that stay native](#textures-that-stay-native). |
| `failures` | RAM attempts that fell back to the native path. |

| Bit | Counter classes | Boot argument |
| --- | --- | --- |
| `0x01` | `buffer` | `-NRedRAMBuffers` |
| `0x02` | `private`, `color` | `-NRedRAMTextures` |
| `0x04` | `highcolor` | `-NRedRAMHighColor` |
| `0x08` | `videoplane` | `-NRedRAMVideoPlanes` |
| `0x10` | `depthstencil` | `-NRedRAMDepthStencil` |
| `0x20` | `composition`, and `compositionplane` together with `0x08` | `-NRedRAMComposition` |
| `0x40` | none | `-NRedRAMBudget` |

Any of the texture options installs the shared allocation hook, which makes bits `0x02` to
`0x10` available. Bit `0x20` also needs the display flip guard, bit `0x01` the data buffer
hook and bit `0x40` the configuration hook, so those three are available only when their boot
arguments are present.

A change applies to later allocations. Existing resources keep their placement, and the flip
guard keeps checking RAM-backed caches after bit `0x20` is cleared. Bit `0x40` applies to
Metal devices created afterwards. To compare placements, recreate the measured resources
after each change, for example by relaunching the workload.

## How the hooks work

`NRed::init()` calls `X5000::init()` to read the options. Options affecting new allocations
register a Lilu load handler for `IOAcceleratorFamily2`. The X5000 load handler verifies
its Mach-O identity before the selector reads X5000 resource fields, and installs the display
flip guard for composition caches. The per-object selector
checks that the accelerator is attached to the integrated PCI device detected by NootedRed
and belongs to `AMDRadeonX5000_AMDVega10GraphicsAccelerator` on a device marked as Renoir.
See [NRed.cpp](../NootedRed/NRed.cpp), [Kexts.cpp](../NootedRed/Kexts.cpp), and
[X5000.cpp](../NootedRed/X5000.cpp).

| Hook | Policy and continuation |
| --- | --- |
| `IOAccelResource2::allocDataBufferMemory()` | Calls the driver with `preferVRAM=false` for a new type `0x0A` data buffer that prefers VRAM. If the call fails, retries with the original preference. |
| `IOAccelResource2::allocMemory()` | Prefers system RAM for eligible type `0x40` resources with an empty primary slot. Private textures and Metal buffers arrive with copy kind `4`, an unset format and a system RAM backing. The driver keeps that backing and records the new primary allocation itself. Resources whose formats are already available use the format checks. A failed RAM attempt resumes the native VRAM path. |
| `IOAccelDevice2::get_config()` | Runs the driver, then raises the video memory size in the returned configuration for devices that Metal created on a Renoir accelerator. See [Metal working set](#metal-working-set). |

The composition route requires the reference driver images and a verified display flip guard.
It preserves the IOSurface backing and lets the driver allocate and map a separate RAM cache.
The display guard declines direct output if either input is a `0xC0` cache with RAM as its
primary memory. X5000 handles normal output through its existing display buffers.

`AMDHWDisplay` and `AMDGFX9Display` share one `getDisplayPipeTransactionFlip()` implementation
at vtable slot `0x198`, and the display classes that NootedRed registers inherit that slot.
`AMDAccelDisplayPipe::executeTransaction()` and
`AMDAccelDisplayMachine::set_display_mode_and_vram()` call it. The guard patches the function
entry, so every caller goes through it. No hardware test has yet made a RAM cache eligible for
direct output after its allocation.

### Page-in

Page-in is unchanged. `AMDRadeonX5000_AMDAccelResource::pageon()` copies a texture through a
temporary VRAM map only for copy kind `8` with texture flag bit `8` set. Every other texture,
including copy kind `8` with bit `8` clear, goes directly to `pageTexture()`. The removed route
accepted only copy kind `8` with bit `8` clear, so it never changed the copy path. Skipping the
temporary map when bit `8` is set would call `pageTexture()` with a combination the native code
never produces at that site, so it needs its own analysis and measurement first.

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
| Primary allocation | Requires both reference UUIDs for selection. Verifies `allocMemory()+0x27`, the native RAM branch and allocator call, and the result and storage instructions. When accelerator bit 21 already selects RAM, resumes the original RAM branch at `+0xDA`. A successful new RAM allocation resumes at `+0x18B` with the memory object in `rax`. Declined or failed attempts resume at `+0x37`. |
| Display flip guard | Requires the reference X5000 UUID. Checks the entry of `AMDHWDisplay::getDisplayPipeTransactionFlip()` and its native eligibility test at `+0x78` before wrapping the function. |
| Scanout layout | Requires the reference X5000 UUID. Checks the AddrLib input at `initIOSurfacePlane()+0x1D3` and the surface information call at `+0x1E1`, then resumes after that call. |
| Metal working set | Requires the reference IOAcceleratorFamily2 UUID. Checks that `IOAccelDevice2::get_config()` stores the VRAM size at `+0x83` and the wired system memory limit at `+0xAB` before wrapping the function. |

The resource type, owning accelerator, backing memory, and primary memory fields are at `0x14`,
`0x18`, `0x80`, and `0x88`. The texture flags at `0x1B4` carry the format in their low byte and
the copy kind in bits `26–29`. Surface parameters are at `0x180` and the auxiliary fields
checked for the high-color and plane policy are at `0x188`, `0x190`, `0x198`, and `0x1A8`.
A shared resource uses `0xE0` for its surface reference.

The allocation and scanout layout hooks run inside driver functions and rely on the live
registers and stack layout of these images. At the allocation site, `rdi` carries the
accelerator, `r14` the resource, and `r15` the shared context. At the scanout layout call,
`r13` carries the resource while `r14` and `rbx` carry the AddrLib input and output.
When porting, recheck the register and stack contract, branch destinations, virtual dispatch,
object fields, and native allocation flags from the target x86_64 slice. Compare the loaded
images and instruction signatures before updating the guards. An identity or signature check
failure leaves the corresponding original driver path in use.

## Remaining VRAM use and limits

Display scanout, cursor and display firmware reservations, some GPU virtual-memory allocations,
and internal blit buffers have separate allocation paths. IOSurface device caches that are
eligible for display scanout use the original allocator. The
system-memory wire limit and GPU address-space availability also constrain the workloads
that can stay in RAM. The firmware reservation and overall UMA budget remain under the
original driver policy.

### Metal working set

Placement does not change the memory size reported to applications. Metal takes
`recommendedMaxWorkingSetSize` and `dedicatedMemorySize` from the VRAM size at accelerator
offset `0xCA8`, which `IOAccelDevice2::get_config()` returns at offset `0x10` of its output.
Both report 512 MiB on the reference machine, so applications that size their working set
from Metal still plan for 512 MiB. The AMD Metal driver also copies this size into
`_segmentByteThreshold`, the byte limit for the resources of one command buffer segment.
A command buffer that references more is split into further segments.

Reporting unified memory in the first `get_config()` word would not raise either value. Metal
then limits the shared size to the VRAM size and clears `_videoRam`, which would leave the
segment limit at zero. `-NRedRAMBudget` therefore changes only the VRAM size in the returned
configuration. It adds half of the wired system memory limit at offset `0x20`, which is a third
of physical memory rounded up to 1 GiB, and stays within that limit. The reference machine
reports 11.5 GiB instead of 512 MiB.

The hook applies only to devices whose `API` property is `Metal`, on a Renoir accelerator, and
only while the texture class is enabled. Metal sets that property when it creates the device.
Clients that do not set it keep the original values, and so does kernel code that reads the
accelerator's own field. Metal reads the configuration when it creates a device, so processes
started earlier keep the old value.

The limit rests on the placement policy. Metal resources reach system RAM, where the fixed pool
does not bound them, but a resource that stays in VRAM can still be referenced together with
more than the pool holds in one segment. Textures in the next section stay native, so keep the
option off for workloads that depend on them until the counters show how much they take.

### Textures that stay native

The `declined_*` counters list textures that an enabled class would have taken, except for one
property. They are reported in this order, and only the first matching reason is counted.

| Reason | Property |
| --- | --- |
| `declined_backing` | The resource already has a system memory backing. Private resources are exempt when that backing is RAM. |
| `declined_params` | The client passed surface parameters when it created the resource. |
| `declined_aux` | A high-color or plane texture has auxiliary storage. |
| `declined_format` | The format belongs to no enabled class, for example compressed or 32-bit float formats. |

On the build before the counters existed, OpenGL textures stayed in VRAM while the same
formats went to RAM through Metal. Uploaded textures carry a RAM backing of the same size, and
render targets carry none. `tools/gl_texture_probe.py` creates both kinds, and the counters
show which property keeps each one native. Surface parameters and compression metadata may
constrain where the GPU can address the data, so the policy does not take such textures
before the counters and a hardware test show that it is safe.

The counters report placement per class. Page-in and copy bytes are not counted yet. Measure
them before widening the format or hardware coverage. The functional smoke results and the separate
placement and capacity acceptance criteria are in the [test guide](RAM-Validation.md).
