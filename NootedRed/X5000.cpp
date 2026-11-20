// AMDRadeonX5000 Patches
//
// Copyright (C) 2022-2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#include <AMDGFX9DCN1Display.hpp>
#include <AMDGFX9DCN2Display.hpp>
#include <AMDGFX9DCNDisplay.hpp>
#include <Backlight.hpp>
#include <CompatTarget.hpp>
#include <GPUDriversAMD/Accel/HWAlignManager.hpp>
#include <GPUDriversAMD/Accel/HWDisplay.hpp>
#include <GPUDriversAMD/Accel/HWEngine.hpp>
#include <GPUDriversAMD/AddrLib.hpp>
#include <GPUDriversAMD/FB/Attributes.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/pci/IOPCIDevice.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <X5000.hpp>
#include <DeviceProfile.hpp>
#include <VCNBridge.hpp>
#include <VDA.hpp>
#include <VideoCommandPatch.hpp>
#include <libkern/OSTypes.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/i386/vm_param.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const UInt8 kChannelTypesPattern[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                             0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};

static const UInt8 kHwlConvertChipFamilyPattern[] = {0x81, 0xFE, 0x8D, 0x00, 0x00, 0x00, 0x0F};

// Make for loop stop after one SDMA engine.
static const UInt8 kStartHWEnginesOriginal[] = {0x40, 0x83, 0xF0, 0x02};
static const UInt8 kStartHWEnginesMask[]     = {0xF0, 0xFF, 0xF0, 0xFF};
static const UInt8 kStartHWEnginesPatched[]  = {0x40, 0x83, 0xF0, 0x01};

// The check in `Addr::Lib::Create` on <=10.15 and 13.4+ is `familyId == 0x8D` instead of `familyId - 0x8D < 2`.
// Change the 0x8D (AI) to 0x8E (RV).
static const UInt8 kAddrLibCreateOriginal[] = {0x41, 0x81, 0x7D, 0x08, 0x8D, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched[]  = {0x41, 0x81, 0x7D, 0x08, 0x8E, 0x00, 0x00, 0x00};

// For some reason Navi (`0x8F`) was added in 14.4 here. Lazy copy pasting?
static const UInt8 kAddrLibCreateOriginal1404[] = {0x41, 0x8B, 0x46, 0x08, 0x3D, 0x8F, 0x00, 0x00, 0x00, 0x74, 0x00,
                                                   0x3D, 0x8D, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreateOriginalMask1404[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched1404[]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatchedMask1404[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Catalina only. Change loop condition to skip SDMA1_HP.
static const UInt8 kCreateAccelChannelsOriginal[] = {0x8D, 0x44, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched[]  = {0x8D, 0x44, 0x09, 0x01};

// Ditto, Mojave.
static const UInt8 kCreateAccelChannelsOriginal10_14[] = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched10_14[]  = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x01};

namespace
{
    constexpr char kAllocDataBufferMemory[] =
        "__ZN16IOAccelResource221allocDataBufferMemoryEP14IOAccelShared2bj";
    constexpr char kCreateSysMemory[] =
        "__ZN16IOAccelSysMemory11withOptionsEP22IOGraphicsAccelerator2P4taskP14IOAccelShared2P16IOAccelResource2jyb";
    constexpr char kGetDisplayFlip[] =
        "__ZN27AMDRadeonX5000_AMDHWDisplay29getDisplayPipeTransactionFlipEjP16IOAccelResource2S1_17_eAMDSwapInterval"
        "P14_AMD_PIPE_FLIPb42AMDAccelDisplayPipeTransactionPipeRotationS4_";
    constexpr char kInitScanoutPlane[] =
        "__ZN36AMDRadeonX5000_AMDAccelResourceAddr218initIOSurfacePlaneEjjjjb";
    constexpr size_t kScanoutLayoutCallOffset = 0x1E1;
    constexpr UInt8 kScanoutLayoutCall[] = {
        0x49, 0x8B, 0xBD, 0x50, 0x02, 0, 0, 0x48, 0x8B, 0x07, 0x4C, 0x89, 0xF6, 0x48, 0x89, 0xDA,
        0xFF, 0x90, 0x30, 0x01, 0, 0,
    };
    constexpr UInt8 kScanoutLayoutInput[] = {0x48, 0xB8, 0x0A, 0, 0, 0, 0x01, 0, 0, 0};
    constexpr size_t kResourceTypeOffset = 0x14;
    constexpr size_t kAcceleratorOffset  = 0x18;
    constexpr size_t kMainMemoryOffset   = 0x88;
    constexpr UInt8  kDataBufferType     = 0xA;

    constexpr size_t kExistingMemoryCheckOffset = 0x89;
    constexpr size_t kSystemAllocationOffset    = 0xA8;
    constexpr UInt8 kBufferAllocationEntry[] = {
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50,
        0x41, 0x89, 0xCE, 0x49, 0x89, 0xF4, 0x48, 0x89, 0xFB, 0x85, 0xD2, 0x74, 0x6E, 0x48,
        0x8B, 0x43, kAcceleratorOffset, 0xF6, 0x80, 0x78, 0x0C, 0x00, 0x00, 0x02, 0x74, 0x61,
    };
    constexpr UInt8 kExistingMemoryCheck[] = {
        0x41, 0xB7, 0x01, 0x48, 0x83, 0xBB, kMainMemoryOffset, 0x00, 0x00, 0x00, 0x00, 0x74, 0x12,
    };
    constexpr UInt8 kSystemAllocation[] = {
        0x48, 0x8B, 0x7B, kAcceleratorOffset, 0x4C, 0x8B, 0x8B, 0xC8, 0x00, 0x00, 0x00, 0xC7, 0x04, 0x24,
        0x00, 0x00, 0x00, 0x00, 0x31, 0xF6, 0x4C, 0x89, 0xE2, 0x31, 0xC9, 0x45, 0x89, 0xF0, 0xE8,
    };
    constexpr size_t kSystemCallEnd = kSystemAllocationOffset + sizeof(kSystemAllocation) + sizeof(SInt32);

    constexpr UInt8 kAcceleratorUUID[] = {
        0x6C, 0x4A, 0x16, 0xB1, 0x66, 0xDC, 0x3A, 0x8B, 0x9F, 0x0B, 0xD3, 0xE0, 0x27, 0xB1, 0x6E, 0xB1,
    };
    constexpr UInt8 kX5000UUID[] = {
        0xE2, 0x7E, 0xFB, 0x64, 0xDE, 0x35, 0x3C, 0xE5, 0xB3, 0xAB, 0xA3, 0x22, 0x18, 0xB4, 0x95, 0xD3,
    };
    constexpr size_t kAllocBranchOffset  = 0x27;
    constexpr size_t kRAMBranchOffset    = 0xDA;
    constexpr size_t kRAMCallOffset      = 0x174;
    constexpr size_t kRAMResultOffset    = 0x18B;
    constexpr size_t kAllocStoreOffset   = 0x1F2;
    constexpr UInt8 kAllocBranch[] = {
        0x8B, 0x87, 0x90, 0x0C, 0x00, 0x00, 0x0F, 0xBA, 0xE0, 0x15, 0x0F, 0x82, 0xA3, 0x00, 0x00, 0x00,
    };
    constexpr UInt8 kRAMBranch[] = {
        0x49, 0x8B, 0x8E, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x15,
    };
    constexpr UInt8 kRAMCall[] = {
        0xC7, 0x04, 0x24, 0x01, 0x00, 0x00, 0x00, 0x31, 0xF6, 0x4C, 0x89, 0xFA, 0x4C, 0x89, 0xF1, 0x4D,
        0x89, 0xE9, 0xE8,
    };
    static_assert(kRAMCallOffset + sizeof(kRAMCall) + sizeof(SInt32) == kRAMResultOffset);
    constexpr UInt8 kRAMResult[] = {
        0x49, 0x89, 0xC4, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0xCC, 0x00, 0x00, 0x00, 0x41, 0xF6, 0x44, 0x24,
        0x0D, 0x80, 0x74, 0x05, 0x41, 0x80, 0x4E, 0x0E, 0x04,
    };
    constexpr UInt8 kAllocStore[] = {0x4D, 0x89, 0xA6, 0x88, 0x00, 0x00, 0x00};
    constexpr UInt8 kDisplayFlipEntry[] = {
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
        0x48, 0x81, 0xEC, 0xB8, 0x01, 0x00, 0x00,
    };
    constexpr size_t kDisplayFlipCheckOffset = 0x78;
    constexpr UInt8 kDisplayFlipCheck[] = {
        0x41, 0xF6, 0x87, 0xB8, 0x01, 0x00, 0x00, 0x20, 0x0F, 0x84, 0x04, 0x04, 0x00, 0x00,
    };
    constexpr size_t kBackingMemoryOffset  = 0x80;
    constexpr size_t kResourceFlagsOffset  = 0x0C;
    constexpr size_t kResourceSizeOffset   = 0xC8;
    constexpr size_t kSharedSurfaceOffset  = 0xE0;
    constexpr size_t kSurfaceParamsOffset  = 0x180;
    constexpr size_t kSurfaceAuxOffsets[]  = {0x188, 0x190, 0x198, 0x1A8};
    constexpr size_t kSurfaceDCCOffset     = 0x1A0;
    constexpr size_t kTextureFlagsOffset   = 0x1B4;
    constexpr size_t kSurfaceFlagsOffset   = 0x1B8;
    constexpr UInt8  kStandardResourceType = 0x40;
    constexpr UInt8  kVidMemSharedType     = 0xC0;
    constexpr UInt32 kLinearCopyKind       = 1;
    constexpr UInt32 kDepthStencilCopyKind = 2;
    constexpr UInt32 kPrivateCopyKind      = 4;
    constexpr UInt32 kStagingCopyKind      = 8;
    // pageon() copies a copy kind 8 texture with this flag through a temporary VRAM map.
    constexpr UInt32 kStagedPageInFlag     = 1U << 8U;
    constexpr UInt32 kDeviceCacheFlag      = 1U << 2U;
    constexpr UInt8  kSurfaceInitialized   = 1U << 3U;
    // Display buffer, alternate system allocator options, external range.
    constexpr UInt32 kSpecialMemoryFlags  = (1U << 26U) | (1U << 30U) | (1U << 31U);

    using CreateSysMemory = void* (*)(void*, void*, void*, void*, UInt32, UInt64, bool);
    CreateSysMemory createRAMPrimary{nullptr};

    bool ramColorRequested{false};
    bool ramDepthStencilRequested{false};
    bool ramHighColorRequested{false};
    bool ramVideoPlanesRequested{false};
    bool ramCompositionRequested{false};
    bool ramCompositionReady{false};
    bool ramX5000Verified{false};
    bool scanoutLayoutRequested{false};
    mach_vm_address_t orgGetDisplayFlip{0};
    const OSMetaClass* acceleratorClass{nullptr};
    const OSMetaClass* sysMemoryClass{nullptr};
    const OSMetaClass* deviceCacheClass{nullptr};

    bool isRenoirResource(void* const resource)
    {
        const auto& nred = NRed::singleton();
        if (resource == nullptr || nred.getIGPU() == nullptr || !nred.getAttributes().isRenoir()) { return false; }
        const auto* const accelerator = getMember<IOService*>(resource, kAcceleratorOffset);
        const auto* const expected = __atomic_load_n(&acceleratorClass, __ATOMIC_ACQUIRE);
        return accelerator != nullptr && expected != nullptr && accelerator->getProvider() == nred.getIGPU()
               && accelerator->metaCast(expected) != nullptr;
    }

    bool isRAMMemory(const OSObject* const memory)
    {
        const auto* const expected = __atomic_load_n(&sysMemoryClass, __ATOMIC_ACQUIRE);
        return memory != nullptr && expected != nullptr && memory->metaCast(expected) != nullptr;
    }

    bool isDeviceCache(const OSObject* const cache)
    {
        // IOSurface is not one of the patched kexts, so look its metaclass up by name once.
        auto* expected = __atomic_load_n(&deviceCacheClass, __ATOMIC_ACQUIRE);
        if (expected == nullptr) {
            const auto* const name = OSSymbol::withCStringNoCopy("IOSurfaceDeviceCache");
            if (name == nullptr) { return false; }
            expected = OSMetaClass::getMetaClassWithName(name);
            name->release();
            if (expected == nullptr) { return false; }
            __atomic_store_n(&deviceCacheClass, expected, __ATOMIC_RELEASE);
        }
        return cache != nullptr && cache->metaCast(expected) != nullptr;
    }

    ATIFormat textureFormat(const UInt32 flags) { return static_cast<ATIFormat>(flags & 0xFF); }

    UInt32 textureCopyKind(const UInt32 flags) { return (flags >> 26) & 0xF; }

    enum class RAMTextureKind : UInt8 { Unsupported, Color8, HighColor, VideoPlane };

    RAMTextureKind ramTextureKind(const ATIFormat format)
    {
        switch (format) {
            case ATIFormat::BGRA8:
            case ATIFormat::RGBA8: return RAMTextureKind::Color8;
            case ATIFormat::BGRA16F:
            case ATIFormat::RGBA16F:
            case ATIFormat::BGRA1010102:
            case ATIFormat::RGBA1010102: return RAMTextureKind::HighColor;
            case ATIFormat::R8:
            case ATIFormat::Rg8:
            case ATIFormat::R16:
            case ATIFormat::Rg16: return RAMTextureKind::VideoPlane;
            default: return RAMTextureKind::Unsupported;
        }
    }

    bool hasSurfaceAuxStorage(void* const resource)
    {
        for (const auto offset : kSurfaceAuxOffsets) {
            if (getMember<void*>(resource, offset) != nullptr) { return true; }
        }
        return false;
    }

    bool useRAMTexture(void* const resource, const UInt32 flags, const RAMTextureKind kind)
    {
        if (textureCopyKind(flags) == kDepthStencilCopyKind
            || getMember<void*>(resource, kSurfaceParamsOffset) != nullptr)
        {
            return false;
        }
        if (kind == RAMTextureKind::Color8) { return ramColorRequested; }
        if ((kind != RAMTextureKind::HighColor || !ramHighColorRequested)
            && (kind != RAMTextureKind::VideoPlane || !ramVideoPlanesRequested))
        {
            return false;
        }
        return !hasSurfaceAuxStorage(resource);
    }

    bool wantsRAMTextureAllocation()
    {
        return ramColorRequested || ramDepthStencilRequested || ramHighColorRequested || ramVideoPlanesRequested
               || ramCompositionRequested;
    }

    void logFirstRAMUse(volatile UInt32& logged, const char* const resource)
    {
        if (OSCompareAndSwap(0, 1, &logged)) { SYSLOG("RAMResources", "Selected system RAM path for %s", resource); }
    }

    template<size_t length>
    bool matchesCode(const mach_vm_address_t function, const mach_vm_address_t slide, const size_t size,
                     const size_t offset, const UInt8 (&expected)[length])
    {
        return function != 0 && function >= slide && offset <= size && length <= size - offset
               && function - slide <= size - offset - length
               && memcmp(reinterpret_cast<const void*>(function + offset), expected, length) == 0;
    }

    bool matchesImage(const mach_vm_address_t slide, const size_t size, const UInt8 (&uuid)[16])
    {
        CompatTarget::Identity identity;
        return CompatTarget::parseMachOIdentity(identity, reinterpret_cast<const UInt8*>(slide), size)
               && memcmp(identity.uuid, uuid, sizeof(uuid)) == 0;
    }

    // Fixes an unsigned 32-bit underflow in IOAccelMemoryAllocator2::allocPages
    // when non-contiguous allocation (`isContiguousOnly == false`) encounters a
    // small free hole whose aligned start `(holeStart + align - 1) & ~(align - 1)`
    // exceeds `holeEnd`.
    void patchIOAccelAllocPages(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                                const size_t size)
    {
        const auto allocNonContig =
            patcher.solveSymbol(id,
                                "__ZN26AMDRadeonX5000_AMDHWMemory21allocateNonContiguousEP16AMDMemoryElementyyyybby15_"
                                "eOP_ORIGINATOR9_eOP_TYPE",
                                slide, size);
        if (allocNonContig == 0) {
            patcher.clearError();
            return;
        }

        const auto* const fnBytes    = reinterpret_cast<const UInt8*>(allocNonContig);
        mach_vm_address_t allocPages = 0;
        for (size_t i = 0; i < 0x110; ++i) {
            // Match `callq allocPages (e8 rel32); addq $0x10, %rsp (48 83 c4 10); testb %al, %al (84 c0)`.
            // Across macOS 13-26, allocPages is the only 8-argument call in allocateNonContiguous(yyyy)
            // that pops 16 bytes of stack arguments and tests %al.
            if (fnBytes[i] == 0xE8 && fnBytes[i + 5] == 0x48 && fnBytes[i + 6] == 0x83 && fnBytes[i + 7] == 0xC4 &&
                fnBytes[i + 8] == 0x10 && fnBytes[i + 9] == 0x84 && fnBytes[i + 10] == 0xC0) {
                SInt32 rel = 0;
                memcpy(&rel, fnBytes + i + 1, sizeof(rel));
                allocPages = allocNonContig + i + 5 + static_cast<mach_vm_address_t>(static_cast<SInt64>(rel));
                break;
            }
        }
        if (allocPages == 0) { return; }

        auto* const targetBytes = reinterpret_cast<UInt8*>(allocPages);
        if (targetBytes[0] != 0x55 || targetBytes[1] != 0x48 || targetBytes[2] != 0x89 || targetBytes[3] != 0xE5) {
            return;
        }

        for (size_t off = 0x40; off < 0x160; ++off) {
            // Match across macOS 13-26:
            //   +0: 41 29 C9              subl %ecx, %r9d
            //   +3: 44 23 4D <disp8>      andl <disp8>(%rbp), %r9d
            //   +7: 0F 84/8E <rel32>      je / jle <skip_hole>
            //  +13: 4D 8D 34 09           leaq (%r9,%rcx), %r14
            if (targetBytes[off] == 0x41 && targetBytes[off + 1] == 0x29 && targetBytes[off + 2] == 0xC9 &&
                targetBytes[off + 3] == 0x44 && targetBytes[off + 4] == 0x23 && targetBytes[off + 5] == 0x4D &&
                targetBytes[off + 7] == 0x0F && (targetBytes[off + 8] == 0x84 || targetBytes[off + 8] == 0x8E) &&
                targetBytes[off + 13] == 0x4D && targetBytes[off + 14] == 0x8D && targetBytes[off + 15] == 0x34 &&
                targetBytes[off + 16] == 0x09) {
                if (targetBytes[off + 8] == 0x84 &&
                    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                    targetBytes[off + 8] = 0x8E;
                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    SYSLOG("X5000", "Patched IOAccelMemoryAllocator2::allocPages non-contiguous hole underflow");
                }
                return;
            }
        }
    }

    // Fixes AMDSWScheduler::advance dereferencing a null peer channel pointer
    // on single-GPU APUs (which have no crossfire peer GPU: 0x68(%r12) == NULL),
    // causing a kernel page fault at CR2: 0x48.
    void patchSWSchedulerAdvance(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                                 const size_t size)
    {
        const auto advanceSym =
            patcher.solveSymbol(id,
                                "__ZN29AMDRadeonX5000_AMDSWScheduler7advanceERNS_12HwChannelCtxE",
                                slide, size);
        if (advanceSym == 0) {
            patcher.clearError();
            return;
        }

        auto* const fnBytes = reinterpret_cast<UInt8*>(advanceSym);
        // Across macOS 15 - 26:
        // Match:
        //   49 8b 7c 24 68       movq 0x68(%r12), %rdi
        //   48 83 7f 48 00       cmpq $0x0, 0x48(%rdi)   <-- crashes when %rdi (peer channel) is null on APU
        // We replace "cmpq $0x0, 0x48(%rdi)" (48 83 7f 48 00) with
        // "testq %rdi, %rdi; nop; nop"       (48 85 ff 90 90) so null %rdi safely takes the jump.
        for (size_t i = 0x200; i < 0x380; ++i) {
            if (fnBytes[i] == 0x49 && fnBytes[i + 1] == 0x8B && fnBytes[i + 2] == 0x7C &&
                fnBytes[i + 3] == 0x24 && fnBytes[i + 4] == 0x68 &&
                fnBytes[i + 5] == 0x48 && fnBytes[i + 6] == 0x83 && fnBytes[i + 7] == 0x7F &&
                fnBytes[i + 8] == 0x48 && fnBytes[i + 9] == 0x00) {
                if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                    fnBytes[i + 5] = 0x48;
                    fnBytes[i + 6] = 0x85;
                    fnBytes[i + 7] = 0xFF;
                    fnBytes[i + 8] = 0x90;
                    fnBytes[i + 9] = 0x90;
                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    SYSLOG("X5000", "Patched AMDSWScheduler::advance null peer channel dereference");
                }
                return;
            }
        }
    }
}    // namespace

static X5000 moduleInstance;

X5000& X5000::singleton() { return moduleInstance; }

X5000::X5000()
{
    if (currentKernelVersion() <= MACOS_10_14_X) {
        this->pm4EngineField               = 0x330;
        this->sdma0EngineField             = 0x338;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->familyTypeField              = 0x29C;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x180;
    }
    else if (currentKernelVersion().majorMatches(MACOS_10_15)) {
        this->pm4EngineField               = 0x348;
        this->sdma0EngineField             = 0x350;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->dccDisplayableSupportField   = 0xA5;
        this->familyTypeField              = 0x2B4;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x188;
    }
    else {
        this->pm4EngineField    = 0x3B8;
        this->sdma0EngineField  = 0x3C0;
        this->familyTypeField   = 0x308;
        this->chipSettingsField = 0x5B10;

        if (currentKernelVersion() <= MACOS_12_X) {
            this->supportedDisplayCountField   = 0x2C;
            this->seCountField                 = 0x5C;
            this->shCountField                 = 0x64;
            this->hwMaxCUsField                = 0x98;
            this->hasUVD0Field                 = 0xAC;
            this->hasVCEField                  = 0xAE;
            this->hasVCN0Field                 = 0xAF;
            this->hasSDMAPagingQueueField      = 0xC0;
            this->hasGetAllClockLimitsField    = 0xBF;
            this->dccDisplayableSupportField   = 0xC1;
            this->hwChannelHWInterfaceField    = 0x18;
            this->hwChannelSubmitCommandBuffer = 0x190;
        }
        else {
            this->supportedDisplayCountField = 0x34;
            this->seCountField               = 0x64;
            this->shCountField               = 0x6C;
            this->hwMaxCUsField              = 0xA0;
            this->hasUVD0Field               = 0xB4;
            this->hasVCEField                = 0xB6;
            this->hasVCN0Field               = 0xB7;
            this->hasSDMAPagingQueueField    = 0xBF;
            this->hasGetAllClockLimitsField  = 0xBE;
            this->dccDisplayableSupportField = 0xC0;
            this->hwChannelHWInterfaceField  = 0x20;
            if (currentKernelVersion() >= MACOS_14_4) { this->hwChannelSubmitCommandBuffer = 0x188; }
            else {
                this->hwChannelSubmitCommandBuffer = 0x190;
            }
        }
    }
}

extern "C" {
[[gnu::used]] static mach_vm_address_t nredRAMAllocContinue{0};
[[gnu::used]] static mach_vm_address_t nredRAMAllocSystem{0};
[[gnu::used]] static mach_vm_address_t nredRAMAllocResult{0};
[[gnu::used]] static mach_vm_address_t nredScanoutLayoutReturn{0};

[[gnu::used, gnu::noinline]] static IOReturn nredSelectScanoutLayout(
    AMDRadeonX5000_AMDHWAlignManager* const align, ADDR2_COMPUTE_SURFACE_INFO_INPUT* const input,
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT* const output, void* const resource)
{
    const auto surfaceFlags = getMember<UInt8>(resource, kSurfaceFlagsOffset);
    if (input->size == sizeof(*input) && output->size == sizeof(*output)
        && input->swizzleMode == 0xA && input->flags.value == 0x220 && input->bpp == 32
        && input->resourceType == ADDR_RSRC_TEX_2D && input->numSlices == 1 && input->numSamples == 1
        && input->width >= 3840 && input->height >= 2160
        && getMember<UInt8>(resource, kResourceTypeOffset) == kVidMemSharedType
        && getMember<UInt8>(resource, 0xAD) == 1 && (surfaceFlags & 0x20) != 0
        && getMember<void*>(resource, kMainMemoryOffset) == nullptr)
    {
        auto candidate = *input;
        const auto swizzle = align->getPreferredSwizzleMode2(&candidate, true);
        if (swizzle != 0 && swizzle < 32 && (Dcn2NonBpp64SwModeMask & (1U << swizzle)) != 0) {
            candidate.swizzleMode = swizzle;
            auto result = *output;
            if (align->getSurfaceInfo2(&candidate, &result) == kIOReturnSuccess) {
                *input = candidate;
                *output = result;
                static volatile UInt32 logged{0};
                if (OSCompareAndSwap(0, 1, &logged)) {
                    SYSLOG("Scanout", "Selected Renoir display layout 0x%x", swizzle);
                }
                return kIOReturnSuccess;
            }
        }
    }
    return align->getSurfaceInfo2(input, output);
}

[[gnu::naked]] static void nredScanoutLayoutBranch()
{
    asm volatile(
        "movq 0x250(%r13), %rdi\n"
        "movq %r14, %rsi\n"
        "movq %rbx, %rdx\n"
        "movq %r13, %rcx\n"
        "callq _nredSelectScanoutLayout\n"
        "jmpq *_nredScanoutLayoutReturn(%rip)\n");
}

static bool nredRAMSelectComposition(void* const resource, void* const shared)
{
    // initIOSurface records the display eligibility before the first allocation.
    if (!__atomic_load_n(&ramCompositionReady, __ATOMIC_ACQUIRE)
        || getMember<UInt8>(resource, kSurfaceFlagsOffset) != kSurfaceInitialized
        || getMember<void*>(resource, kSurfaceParamsOffset) == nullptr)
    {
        return false;
    }
    const auto flags = getMember<UInt32>(resource, kTextureFlagsOffset);
    const auto format = textureFormat(flags);
    const auto copyKind = textureCopyKind(flags);
    const auto* const cache = getMember<OSObject*>(resource, kSharedSurfaceOffset);
    const auto hasDeviceCache = (getMember<UInt32>(resource, kResourceFlagsOffset) & kDeviceCacheFlag) != 0;
    if (copyKind == kLinearCopyKind && format == ATIFormat::Rg8) {
        // The native page-in copies between the two prepared maps for this plane.
        if (!ramVideoPlanesRequested || hasDeviceCache || cache != nullptr || (flags & kStagedPageInFlag) != 0
            || hasSurfaceAuxStorage(resource) || getMember<void*>(resource, kSurfaceDCCOffset) != nullptr)
        {
            return false;
        }
    }
    else if (shared != nullptr || !hasDeviceCache || copyKind != kStagingCopyKind
             || ramTextureKind(format) != RAMTextureKind::Color8 || !isDeviceCache(cache))
    {
        return false;
    }

    const auto* const backing = getMember<OSObject*>(resource, kBackingMemoryOffset);
    return isRAMMemory(backing);
}

static bool nredRAMSelectAllocation(void* const resource, void* const shared)
{
    if (!__atomic_load_n(&ramX5000Verified, __ATOMIC_ACQUIRE) || !isRenoirResource(resource)
        || getMember<void*>(resource, kMainMemoryOffset) != nullptr
        || (getMember<UInt32>(resource, kResourceFlagsOffset) & kSpecialMemoryFlags) != 0)
    {
        return false;
    }

    const auto type = getMember<UInt8>(resource, kResourceTypeOffset);
    if (type == kVidMemSharedType) { return nredRAMSelectComposition(resource, shared); }
    if (type != kStandardResourceType || shared == nullptr
        || getMember<void*>(resource, kSharedSurfaceOffset) != nullptr)
    {
        return false;
    }

    const auto flags = getMember<UInt32>(resource, kTextureFlagsOffset);
    const auto* const backing = getMember<OSObject*>(resource, kBackingMemoryOffset);

    // Private textures and Metal buffers use this copy path before their format is set.
    // Preserve the backing object and let the driver keep ownership of both allocations.
    if ((flags & 0xFF) == 0 && textureCopyKind(flags) == kPrivateCopyKind) {
        return ramColorRequested && getMember<void*>(resource, kSurfaceParamsOffset) == nullptr
               && isRAMMemory(backing);
    }
    if (backing != nullptr) { return false; }

    if (ramDepthStencilRequested && textureCopyKind(flags) == kDepthStencilCopyKind) {
        switch (textureFormat(flags)) {
            case ATIFormat::Depth16:
            case ATIFormat::Depth24Stencil8:
            case ATIFormat::Depth32:
            case ATIFormat::Depth32F:
            case ATIFormat::Depth32fStencil8:
            case ATIFormat::Stencil8: return true;
            default: return false;
        }
    }

    return useRAMTexture(resource, flags, ramTextureKind(textureFormat(flags)));
}

[[gnu::used, gnu::noinline]] static void* nredRAMAllocatePrimary(void* const accelerator, void* const resource,
                                                               void* const shared)
{
    if (!nredRAMSelectAllocation(resource, shared)) { return nullptr; }

    const auto bytes = getMember<UInt64>(resource, kResourceSizeOffset);
    if (bytes == 0 || bytes > UINT64_MAX - PAGE_MASK) { return nullptr; }

    // Match allocMemory's direct system allocation for ordinary resources. The native
    // continuation records the successful result and a failed attempt leaves the slots intact.
    const auto aligned = (bytes + PAGE_MASK) & ~static_cast<UInt64>(PAGE_MASK);
    auto* const memory = createRAMPrimary(accelerator, nullptr, shared, resource, 0x363, aligned, true);
    if (memory != nullptr) {
        static volatile UInt32 loggedComposition{0};
        static volatile UInt32 logged{0};
        if (getMember<UInt8>(resource, kResourceTypeOffset) == kVidMemSharedType) {
            logFirstRAMUse(loggedComposition, "composition surface primary allocation");
        }
        else {
            logFirstRAMUse(logged, "standard resource primary allocation");
        }
    }
    return memory;
}

[[gnu::naked]] static void nredRAMAllocBranch()
{
    asm volatile(
        "movl 0xc90(%rdi), %eax\n"
        "btl $21, %eax\n"
        "jc 1f\n"
        "pushq %rdi\n"
        "subq $8, %rsp\n"
        "movq %r14, %rsi\n"
        "movq %r15, %rdx\n"
        "callq _nredRAMAllocatePrimary\n"
        "addq $8, %rsp\n"
        "popq %rdi\n"
        "testq %rax, %rax\n"
        "jnz 2f\n"
        "jmpq *_nredRAMAllocContinue(%rip)\n"
        "1:\n"
        "jmpq *_nredRAMAllocSystem(%rip)\n"
        "2:\n"
        "jmpq *_nredRAMAllocResult(%rip)\n");
}
}

void X5000::init()
{
    this->ramBuffersRequested = checkKernelArgument("-NRedRAMBuffers");
    ramColorRequested         = checkKernelArgument("-NRedRAMTextures");
    ramDepthStencilRequested  = checkKernelArgument("-NRedRAMDepthStencil");
    ramHighColorRequested     = checkKernelArgument("-NRedRAMHighColor");
    ramVideoPlanesRequested   = checkKernelArgument("-NRedRAMVideoPlanes");
    ramCompositionRequested   = checkKernelArgument("-NRedRAMComposition");
    scanoutLayoutRequested    = checkKernelArgument("-NRedScanoutLayout");
    if (this->ramBuffersRequested || wantsRAMTextureAllocation()) {
        lilu.onKextLoadForce(&kextIOAcceleratorFamily2);
    }
}

namespace
{
    bool ramBackedDisplayCache(void* const resource)
    {
        return resource != nullptr && getMember<UInt8>(resource, kResourceTypeOffset) == kVidMemSharedType
               && isRenoirResource(resource) && isRAMMemory(getMember<OSObject*>(resource, kMainMemoryOffset));
    }

    bool wrapGetDisplayFlip(void* const display, const UInt32 index, void* const resource, void* const second,
                            const UInt32 interval, void* const flip, const bool fill,
                            const UInt32 rotation, void* const savedFlip)
    {
        // The native eligibility query declines direct output for a composition surface.
        // Also reject a RAM cache here if it reaches either slot of a later display request.
        if (ramBackedDisplayCache(resource) || ramBackedDisplayCache(second)) { return false; }
        return FunctionCast(wrapGetDisplayFlip, orgGetDisplayFlip)(
            display, index, resource, second, interval, flip, fill, rotation, savedFlip);
    }

    [[gnu::noinline]] bool installRAMCompositionGuard(KernelPatcher& patcher, const size_t id,
                                                     const mach_vm_address_t slide, const size_t size)
    {
        const auto entry = patcher.solveSymbol(id, kGetDisplayFlip, slide, size);
        patcher.clearError();
        if (!matchesCode(entry, slide, size, 0, kDisplayFlipEntry)
            || !matchesCode(entry, slide, size, kDisplayFlipCheckOffset, kDisplayFlipCheck))
        {
            SYSLOG("RAMResources", "Unsupported X5000 display flip path");
            return false;
        }
        KernelPatcher::RouteRequest route{kGetDisplayFlip, wrapGetDisplayFlip, orgGetDisplayFlip};
        const auto installed = patcher.routeMultipleLong(id, &route, 1, slide, size);
        patcher.clearError();
        return installed;
    }

    bool routeRAMBranch(KernelPatcher& patcher, const size_t id, const mach_vm_address_t address, void (*hook)())
    {
        KernelPatcher::RouteRequest request{nullptr, hook};
        request.from = address;
        const auto installed = patcher.routeMultipleLong(id, &request, 1);
        patcher.clearError();
        return installed;
    }

    [[gnu::noinline]] void installScanoutLayout(KernelPatcher& patcher, const size_t id,
                                               const mach_vm_address_t slide, const size_t size)
    {
        const auto function = patcher.solveSymbol(id, kInitScanoutPlane, slide, size);
        patcher.clearError();
        if (!matchesImage(slide, size, kX5000UUID)
            || !matchesCode(function, slide, size, 0x1D3, kScanoutLayoutInput)
            || !matchesCode(function, slide, size, kScanoutLayoutCallOffset, kScanoutLayoutCall))
        {
            SYSLOG("Scanout", "Unsupported X5000 scanout layout call");
            return;
        }
        const auto call = function + kScanoutLayoutCallOffset;
        nredScanoutLayoutReturn = call + sizeof(kScanoutLayoutCall);
        const auto installed = routeRAMBranch(patcher, id, call, nredScanoutLayoutBranch);
        SYSLOG("Scanout", "Renoir display layout route %s", installed ? "installed" : "failed");
    }

    void installRAMTextureRoute(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                               const size_t size)
    {
        const auto function = patcher.solveSymbol(id, "__ZN16IOAccelResource211allocMemoryEP14IOAccelShared2", slide, size);
        patcher.clearError();
        const auto allocator = patcher.solveSymbol(id, kCreateSysMemory, slide, size);
        patcher.clearError();
        if (!matchesImage(slide, size, kAcceleratorUUID)
            || allocator == 0
            || !matchesCode(function, slide, size, kAllocBranchOffset, kAllocBranch)
            || !matchesCode(function, slide, size, kRAMBranchOffset, kRAMBranch)
            || !matchesCode(function, slide, size, kRAMCallOffset, kRAMCall)
            || !matchesCode(function, slide, size, kRAMResultOffset, kRAMResult)
            || !matchesCode(function, slide, size, kAllocStoreOffset, kAllocStore))
        {
            SYSLOG("RAMResources", "Unsupported IOAcceleratorFamily2 texture allocation path");
            return;
        }
        SInt32 displacement{0};
        memcpy(&displacement, reinterpret_cast<const void*>(function + kRAMResultOffset - sizeof(displacement)),
               sizeof(displacement));
        if (function + kRAMResultOffset + static_cast<mach_vm_address_t>(displacement) != allocator) {
            SYSLOG("RAMResources", "Unexpected allocator in IOAcceleratorFamily2 allocation path");
            return;
        }

        createRAMPrimary     = reinterpret_cast<CreateSysMemory>(allocator);
        nredRAMAllocContinue = function + kAllocBranchOffset + sizeof(kAllocBranch);
        nredRAMAllocSystem   = function + kRAMBranchOffset;
        nredRAMAllocResult   = function + kRAMResultOffset;
        const auto installed = routeRAMBranch(patcher, id, function + kAllocBranchOffset, nredRAMAllocBranch);
        SYSLOG("RAMResources", "Texture allocation route %s", installed ? "installed" : "failed");
    }
}

void X5000::installRAMBufferRoute(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                                 const size_t size)
{
    const auto entry = patcher.solveSymbol(id, kAllocDataBufferMemory, slide, size);
    patcher.clearError();
    const auto systemAllocator = patcher.solveSymbol(id, kCreateSysMemory, slide, size);
    patcher.clearError();

    if (!matchesImage(slide, size, kAcceleratorUUID)
        || entry == 0 || systemAllocator == 0 || entry < slide || size < kSystemCallEnd
        || entry - slide > size - kSystemCallEnd)
    {
        SYSLOG("RAMBuffers", "System RAM allocation entry points unavailable");
        return;
    }

    const auto* const code = reinterpret_cast<const UInt8*>(entry);
    SInt32 displacement{0};
    memcpy(&displacement, code + kSystemCallEnd - sizeof(displacement), sizeof(displacement));
    const auto callTarget = entry + kSystemCallEnd + static_cast<mach_vm_address_t>(displacement);
    if (memcmp(code, kBufferAllocationEntry, sizeof(kBufferAllocationEntry)) != 0
        || memcmp(code + kExistingMemoryCheckOffset, kExistingMemoryCheck, sizeof(kExistingMemoryCheck)) != 0
        || memcmp(code + kSystemAllocationOffset, kSystemAllocation, sizeof(kSystemAllocation)) != 0
        || callTarget != systemAllocator)
    {
        SYSLOG("RAMBuffers", "Unsupported IOAcceleratorFamily2 buffer allocator");
        return;
    }

    KernelPatcher::RouteRequest request{kAllocDataBufferMemory, wrapAllocDataBufferMemory,
                                        this->orgAllocDataBufferMemory};
    if (!patcher.routeMultipleLong(id, &request, 1, slide, size)) {
        patcher.clearError();
        SYSLOG("RAMBuffers", "Could not route data buffer allocation");
        return;
    }

    SYSLOG("RAMBuffers", "Installed native system RAM route for Renoir data buffers");
}

bool X5000::wrapAllocDataBufferMemory(void* const resource, void* const shared, const bool preferVRAM,
                                      const UInt32 options)
{
    const auto original = FunctionCast(wrapAllocDataBufferMemory, singleton().orgAllocDataBufferMemory);
    if (!preferVRAM || shared == nullptr || !isRenoirResource(resource)
        || getMember<UInt8>(resource, kResourceTypeOffset) != kDataBufferType
        || getMember<void*>(resource, kMainMemoryOffset) != nullptr)
    {
        return original(resource, shared, preferVRAM, options);
    }

    const auto allocated = original(resource, shared, false, options);
    static volatile UInt32 logged{0};
    if (allocated && getMember<void*>(resource, kMainMemoryOffset) != nullptr && OSCompareAndSwap(0, 1, &logged)) {
        SYSLOG("RAMBuffers", "Allocated first Renoir data buffer in system RAM");
    }
    return allocated || original(resource, shared, preferVRAM, options);
}

void X5000::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if ((this->ramBuffersRequested || wantsRAMTextureAllocation())
        && kextIOAcceleratorFamily2.loadIndex == id)
    {
        const auto memoryClass = patcher.solveSymbol(id, "__ZN16IOAccelSysMemory10gMetaClassE", slide, size);
        patcher.clearError();
        if (memoryClass == 0) {
            SYSLOG("RAMResources", "IOAccelSysMemory metaclass unavailable");
            return;
        }
        __atomic_store_n(&sysMemoryClass, reinterpret_cast<const OSMetaClass*>(memoryClass), __ATOMIC_RELEASE);
        if (this->ramBuffersRequested) { this->installRAMBufferRoute(patcher, id, slide, size); }
        if (wantsRAMTextureAllocation()) { installRAMTextureRoute(patcher, id, slide, size); }
        return;
    }
    if (kextRadeonX5000.loadIndex != id) { return; }

    NRed::singleton().hwLateInit();

    if (this->ramBuffersRequested || wantsRAMTextureAllocation()) {
        // The selectors only accept resources owned by this accelerator class.
        const auto metaClass = patcher.solveSymbol(
            id, "__ZN43AMDRadeonX5000_AMDVega10GraphicsAccelerator10gMetaClassE", slide, size);
        patcher.clearError();
        if (metaClass == 0) { SYSLOG("RAMResources", "Renoir accelerator metaclass unavailable"); }
        __atomic_store_n(&acceleratorClass, reinterpret_cast<const OSMetaClass*>(metaClass), __ATOMIC_RELEASE);
    }
    if (wantsRAMTextureAllocation()) {
        const auto verified = matchesImage(slide, size, kX5000UUID);
        __atomic_store_n(&ramX5000Verified, verified, __ATOMIC_RELEASE);
        if (!verified) {
            SYSLOG("RAMResources", "Unsupported X5000 image for texture placement");
        }
        else if (ramCompositionRequested) {
            const auto protectedFlip = installRAMCompositionGuard(patcher, id, slide, size);
            __atomic_store_n(&ramCompositionReady, protectedFlip, __ATOMIC_RELEASE);
            SYSLOG("RAMResources", "Composition cache RAM placement %s", protectedFlip ? "enabled" : "disabled");
        }
    }

    UInt32*           orgChannelTypes;
    mach_vm_address_t orgStartHWEngines;
    void*             pm4ComputeChannelVT;

    PenguinWizardry::PatternSolveRequest solveRequests[] = {
        {currentKernelVersion() <= MACOS_10_15_X ?
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator22getAdditionalQueueListEPPK18_"
             "AMDQueueSpecifierE27additionalQueueList_Default" :
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator19createAccelChannelsEbE12channelTypes",
         orgChannelTypes, kChannelTypesPattern},
        {"__ZN31AMDRadeonX5000_AMDGFX9PM4Engine10gMetaClassE", this->pm4EngineMC},
        {"__ZN32AMDRadeonX5000_AMDGFX9SDMAEngine10gMetaClassE", this->sdmaEngineMC},
        {"__ZN26AMDRadeonX5000_AMDHardware14startHWEnginesEv", orgStartHWEngines},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware32setupAndInitializeHWCapabilitiesEv",
         this->orgGFX9SetupAndInitializeHWCapabilities},
        {"__ZTV39AMDRadeonX5000_AMDGFX9PM4ComputeChannel", pm4ComputeChannelVT},
        {"__ZN30AMDRadeonX5000_AMDPM4HWChannel19submitCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
         this->orgPM4SubmitCommandBuffer},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "X5000",
               "Failed to resolve symbols");

    AMDRadeonX5000_AMDHWAlignManager::resolve(patcher, id, slide, size);
    if (scanoutLayoutRequested && NRed::singleton().getAttributes().isRenoir()) {
        installScanoutLayout(patcher, id, slide, size);
    }
    AMDRadeonX5000_AMDHWDisplay::resolve(patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCNDisplay::registerMC(kextRadeonX5000.id, patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCN1Display::resolve(kextRadeonX5000.id);
    AMDRadeonX5000_AMDGFX9DCN2Display::resolve(kextRadeonX5000.id);

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware17allocateHWEnginesEv", allocateHWEngines},
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware32setupAndInitializeHWCapabilitiesEv",
         wrapSetupAndInitializeHWCapabilities},
        {"__ZN26AMDRadeonX5000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE", wrapGetHWChannel,
         this->orgGetHWChannel},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20initializeFamilyTypeEv", initializeFamilyType},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20allocateAMDHWDisplayEv", allocateAMDHWDisplay},
        {"__ZN26AMDRadeonX5000_AMDHWMemory17adjustVRAMAddressEy", wrapAdjustVRAMAddress, this->orgAdjustVRAMAddress},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20writeASICHangLogInfoEPPv", returnZero},
        {"__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj", wrapHwlConvertChipFamily, this->orgHwlConvertChipFamily,
         kHwlConvertChipFamilyPattern},
        {"__ZN27AMDRadeonX5000_AMDHWDisplay14getDisplayInfoEjbbPvP17_FRAMEBUFFER_INFO", fixedGetDisplayInfo},
        {"__ZN33AMDRadeonX5000_AMDHWAlignManager214getSurfaceInfoEP24_AMD_SURFACE_INFO_STRUCT", fixedGetSurfaceInfo},
    };
    PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "X5000",
               "Failed to route symbols");

    this->orgTotalFreeMemory = reinterpret_cast<decltype(this->orgTotalFreeMemory)>(
        patcher.solveSymbol(id, "__ZN26AMDRadeonX5000_AMDHWMemory15totalFreeMemoryEv", slide, size));
    this->orgAllocate = reinterpret_cast<decltype(this->orgAllocate)>(patcher.solveSymbol(
        id,
        "__ZN26AMDRadeonX5000_AMDHWMemory8allocateEP16AMDMemoryElementyy28_eAMD_MEMORY_ALLOCATION_TYPEb15_eOP_ORIGINATOR9_eOP_TYPE",
        slide, size));
    this->orgAllocateNonContiguous = reinterpret_cast<decltype(this->orgAllocateNonContiguous)>(patcher.solveSymbol(
        id,
        // Match the allocation-TYPE overload declared by the function pointer. The yyyy overload
        // takes a range start and length instead, adding an argument and shifting the stack arguments.
        "__ZN26AMDRadeonX5000_AMDHWMemory21allocateNonContiguousEP16AMDMemoryElementyy28_eAMD_MEMORY_ALLOCATION_TYPEbby15_eOP_ORIGINATOR9_eOP_TYPE",
        slide, size));
    this->allocatorTraceEnabled = checkKernelArgument("-NRedAllocTrace");

    if (this->orgTotalFreeMemory != nullptr && this->orgAllocate != nullptr && this->orgAllocateNonContiguous != nullptr) {
        PenguinWizardry::PatternRouteRequest allocLargeReq{
            "__ZN26AMDRadeonX5000_AMDHWMemory19allocateLargeBlocksEP16AMDMemoryElementyyy15_eOP_ORIGINATOR9_eOP_TYPEb",
            wrapAllocateLargeBlocks, this->orgAllocateLargeBlocks};
        if (!allocLargeReq.route(patcher, id, slide, size)) {
            patcher.clearError();
            SYSLOG("X5000", "Failed to route AMDHWMemory::allocateLargeBlocks");
        } else {
            NRed::singleton().setProp32("NRedVRAMAllocatorRevision", 2);
        }
    }
    patchIOAccelAllocPages(patcher, id, slide, size);
    VideoCommandPatch::install(
        patcher, id, slide, size,
        "__ZN35AMDRadeonX5000_AMDAccelVideoContext14processCommandEPj16VideoCommandTypeS0_j",
        "__ZN35AMDRadeonX5000_AMDAccelVideoContext13fetchResourceEP22_AMDVideoResourceParamP21_AMDVideoResourceInfo",
        "NRedVideoCommandGuard5000");
    patchSWSchedulerAdvance(patcher, id, slide, size);

    const auto isGfxOffSupported =
        patcher.solveSymbol(id, "__ZN28AMDRadeonX5000_AMDRTHardware17isGfxOffSupportedEv", slide, size);
    if (isGfxOffSupported != 0) {
        const auto* code = reinterpret_cast<const UInt8*>(isGfxOffSupported);
        if (code[0] == 0x55 && code[1] == 0x48 && code[2] == 0x89 && code[3] == 0xe5 && code[4] == 0x8a &&
            code[5] == 0x87) {
            this->gfxOffSupportedOffset = *reinterpret_cast<const UInt32*>(code + 6);
            DBGLOG("X5000", "Detected gfxOffSupportedOffset = 0x%x", this->gfxOffSupportedOffset);
        } else {
            this->gfxOffSupportedOffset = 0x206b8;
        }
    } else {
        patcher.clearError();
        this->gfxOffSupportedOffset = 0x206b8;
    }

    PenguinWizardry::PatternRouteRequest rtStartReq{
        "__ZN28AMDRadeonX5000_AMDRTHardware16hardwareDidStartEv",
        wrapRTHardwareDidStart, this->orgRTHardwareDidStart};
    if (!rtStartReq.route(patcher, id, slide, size)) {
        patcher.clearError();
        SYSLOG("X5000", "Failed to route AMDRTHardware::hardwareDidStart");
    }

    PenguinWizardry::PatternRouteRequest gfxOffRoutes[] = {
        {"__ZN28AMDRadeonX5000_AMDRTHardware17isGfxOffSupportedEv", returnZero},
        {"__ZN28AMDRadeonX5000_AMDRTHardware12enableGfxOffEv", returnZero},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware19gfxOffTimerCallbackEv", returnZero},
    };
    for (auto& req : gfxOffRoutes) {
        if (!req.route(patcher, id, slide, size)) {
            patcher.clearError();
            DBGLOG("X5000", "Optional GFXOFF route omitted: %s", req.symbol);
        }
    }
    NRed::singleton().setProp32("NRedGFXOFFPatched", 1);

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternSolveRequest solveRequest{"__ZN30AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv",
                                                          this->notifyGfxAccess};
        PANIC_COND(!solveRequest.solve(patcher, id, slide, size), "X5000", "Failed to resolve notifyGfxAccess");

        bool hooked = false;
        if (pm4ComputeChannelVT != nullptr && this->orgPM4SubmitCommandBuffer != 0) {
            auto* vt = reinterpret_cast<mach_vm_address_t*>(pm4ComputeChannelVT);
            for (size_t idx = (0x170 / sizeof(mach_vm_address_t)); idx <= (0x1B0 / sizeof(mach_vm_address_t)); ++idx) {
                if (vt[idx] == this->orgPM4SubmitCommandBuffer) {
                    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                        vt[idx] = reinterpret_cast<mach_vm_address_t>(computeSubmitCommandBuffer);
                        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                        hooked = true;
                        DBGLOG("X5000", "Successfully hooked computeSubmitCommandBuffer at dynamic vtable offset 0x%lx",
                               idx * sizeof(mach_vm_address_t));
                    }
                    break;
                }
            }
        }
        if (!hooked) {
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                this->hwChannelSubmitCommandBuffer(pm4ComputeChannelVT) =
                    reinterpret_cast<mach_vm_address_t>(computeSubmitCommandBuffer);
                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                hooked = true;
                SYSLOG("X5000", "Fallback hooked computeSubmitCommandBuffer via field offset");
            }
        }
    }

    if (currentKernelVersion() >= MACOS_13_4) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_"
            "PRIORITYP27AMDRadeonX5000_AMDAccelTask",
            wrapObtainAccelChannelGroup1304, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }
    else if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_PRIORITY",
            wrapObtainAccelChannelGroup, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }

    if (currentKernelVersion() >= MACOS_14_4) {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,          kAddrLibCreateOriginal1404,    kAddrLibCreateOriginalMask1404,
            kAddrLibCreatePatched1404, kAddrLibCreatePatchedMask1404, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply 14.4+ Addr::Lib::Create patch");
    }
    else if (currentKernelVersion() <= MACOS_10_15_X || currentKernelVersion() >= MACOS_13_4) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kAddrLibCreateOriginal, kAddrLibCreatePatched,
                                                       1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply Addr::Lib::Create patch");
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        if (currentKernelVersion().majorMatches(MACOS_10_15)) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal,
                                                           kCreateAccelChannelsPatched, 2};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal10_14,
                                                           kCreateAccelChannelsPatched10_14, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }

        // TODO: wait, what is this doing again?
        if (NRed::singleton().getAttributes().isRenoir()) {
            UInt32                                   findNonBpp64 = Dcn1NonBpp64SwModeMask1015;
            UInt32                                   replNonBpp64 = Dcn2NonBpp64SwModeMask1015;
            UInt32                                   findBpp64 = Dcn1NonBpp64SwModeMask1015 ^ Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64 = Dcn2NonBpp64SwModeMask1015 ^ Dcn2Bpp64SwModeMask1015;
            UInt32                                   findBpp64Pt2 = Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64Pt2 = Dcn2Bpp64SwModeMask1015;
            const PenguinWizardry::MaskedLookupPatch patches[]    = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32), 2},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32), 1},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64Pt2),
                 reinterpret_cast<const UInt8*>(&replBpp64Pt2), sizeof(UInt32), 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        *orgChannelTypes = 1;    // Make VMPT use SDMA0 instead of SDMA1
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
    else {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,       kStartHWEnginesOriginal, kStartHWEnginesMask,
            kStartHWEnginesPatched, kStartHWEnginesMask,     currentKernelVersion() >= MACOS_13 ? 2U : 1};
        PANIC_COND(!patch.apply(patcher, orgStartHWEngines, PAGE_SIZE), "X5000", "Failed to patch startHWEngines");

        if (NRed::singleton().getAttributes().isRenoir()) {
            UInt32 findBpp64 = Dcn1Bpp64SwModeMask, replBpp64 = Dcn2Bpp64SwModeMask;
            UInt32 findNonBpp64 = Dcn1NonBpp64SwModeMask, replNonBpp64 = Dcn2NonBpp64SwModeMask;
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        // createAccelChannels: stop at SDMA0
        orgChannelTypes[5] = 1;
        // getPagingChannel: get only SDMA0
        orgChannelTypes[currentKernelVersion() >= MACOS_12 ? 12 : 11] = 0;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
}

bool X5000::allocateHWEngines(void* const self)
{
    [[clang::suppress]] singleton().pm4EngineField(self)   = singleton().pm4EngineMC->alloc();
    [[clang::suppress]] singleton().sdma0EngineField(self) = singleton().sdmaEngineMC->alloc();

    // VCN0 is only populated when the bridge validated both drivers and allocated the native X6000
    // VCN2 engine.  If allocation fails, make sure the capability does not stay advertised.
    const auto* const profile = DeviceProfile::current();
    if (profile != nullptr && profile->media.hasVCN0 && VCNBridge::singleton().isRequested()) {
        if (auto* const vcn0 = VCNBridge::singleton().allocateEngine(self); vcn0 != nullptr) {
            (singleton().pm4EngineField + kAMDHWEngineTypeVCN0 * sizeof(void*))(self) = vcn0;
            // Keep the capability in sync regardless of whether setupAndInitializeHWCapabilities runs before
            // or after allocateHWEngines in this macOS build.
            singleton().hasVCN0Field(self) = true;
        }
        else {
            singleton().hasVCN0Field(self) = false;
        }
    }

    return true;
}

void X5000::setVCN0Capability(void* const self, const bool enabled)
{
    if (self != nullptr) { singleton().hasVCN0Field(self) = enabled; }
}

// TODO: Replace with IP Discovery?
void X5000::wrapSetupAndInitializeHWCapabilities(void* const self)
{
    auto& seCount  = singleton().seCountField(self);
    auto& shCount  = singleton().shCountField(self);
    auto& hwMaxCUs = singleton().hwMaxCUsField(self);

    seCount = 1;
    shCount = 1;
    if (NRed::singleton().getAttributes().isRenoir()) { hwMaxCUs = 8; }
    else if (NRed::singleton().getAttributes().isRaven2()) {
        hwMaxCUs = 3;
    }
    else {
        hwMaxCUs = 11;
    }

    FunctionCast(wrapSetupAndInitializeHWCapabilities, singleton().orgGFX9SetupAndInitializeHWCapabilities)(self);

    const auto* const profile = DeviceProfile::current();

    // Media engines stay off unless the device/build profile allows VCN0 and the bridge
    // enabled/allocated the native engine. allocateHWEngines() sets the same
    // capability after a successful allocation, so the result does not depend on the hook ordering.
    const auto mediaEnabled = VDA::singleton().canAdvertise() && VCNBridge::singleton().userPatchesAllowed();

    singleton().supportedDisplayCountField(self) = 4;
    singleton().hasUVD0Field(self)               = mediaEnabled && profile != nullptr && profile->media.hasUVD0;
    singleton().hasVCEField(self)                = mediaEnabled && profile != nullptr && profile->media.hasVCE;
    singleton().hasVCN0Field(self)               = mediaEnabled && profile != nullptr && profile->media.hasVCN0;
    singleton().hasSDMAPagingQueueField(self)    = false;
    singleton().hasGetAllClockLimitsField(self)  = false;
    if (currentKernelVersion() >= MACOS_10_15) { singleton().dccDisplayableSupportField(self) = true; }

    if (mediaEnabled) { DBGLOG("X5000", "VCN0 media engine enabled from the device profile"); }
}

// TODO: Investigate why this is needed.
void* X5000::wrapGetHWChannel(void* const self, AMDHWEngineType engineType, const UInt32 ringId)
{
    if (engineType == kAMDHWEngineTypeSDMA1) { engineType = kAMDHWEngineTypeSDMA0; }
    return FunctionCast(wrapGetHWChannel, singleton().orgGetHWChannel)(self, engineType, ringId);
}

void X5000::initializeFamilyType(void* const self) { singleton().familyTypeField(self) = AMD_FAMILY_RAVEN; }

void* X5000::allocateAMDHWDisplay(void* const)
{
    if (NRed::singleton().getAttributes().isRenoir()) {
        return AMDRadeonX5000_AMDGFX9DCN2Display::gRTMetaClass.alloc();
    }
    return AMDRadeonX5000_AMDGFX9DCN1Display::gRTMetaClass.alloc();
}

UInt64 X5000::wrapAdjustVRAMAddress(void* const self, const UInt64 addr)
{
    auto ret = FunctionCast(wrapAdjustVRAMAddress, singleton().orgAdjustVRAMAddress)(self, addr);
    if (ret != addr) { return ret + NRed::singleton().getFbOffset(); }
    return ret;
}

bool X5000::wrapAllocateLargeBlocks(void* const self, void* const elem, const UInt64 size, const UInt64 align,
                                    const UInt64 minBlockSize, const UInt32 orig, const UInt32 opType,
                                    const bool onlyNonVisible)
{
    auto& inst = singleton();
    if (self == nullptr || elem == nullptr || size == 0) { return false; }

    const UInt64 pageAlignedSize = (size + PAGE_MASK) & ~static_cast<UInt64>(PAGE_MASK);
    if (inst.orgTotalFreeMemory != nullptr && inst.orgTotalFreeMemory(self) < pageAlignedSize) {
        // Fast-fail without walking the allocator free list or spamming IOLog while
        // IOAcceleratorFamily2 is evicting small LRU buffers one by one to make room.
        return false;
    }

    const UInt64 blockGranularity = minBlockSize >= PAGE_SIZE ? minBlockSize : 0x10000ULL;
    const UInt64 roundedSize =
        size > blockGranularity ? ((size + blockGranularity - 1) & ~(blockGranularity - 1)) : pageAlignedSize;
    const UInt64 safeAlign = align >= blockGranularity ? align : blockGranularity;

    // 1. Contiguous attempt in Non-Visible range [256MB, totalVRAMSize] (allocType = 1)
    if (inst.orgAllocate(self, elem, size, align, 1, false, orig, opType)) { return true; }

    // 2. Contiguous attempt in Visible range [reservedNdrvEnd, 256MB] (allocType = 0) if allowed
    if (!onlyNonVisible && inst.orgAllocate(self, elem, size, align, 0, false, orig, opType)) { return true; }

    auto tryNonContiguous = [&](const UInt64 requestSize, const UInt64 requestAlign,
                                const UInt32 allocType, const UInt64 chunkSize) -> bool {
        if (onlyNonVisible && allocType != 1) { return false; }
        const auto result = inst.orgAllocateNonContiguous(self, elem, requestSize, requestAlign, allocType,
                                                          false, false, chunkSize, orig, opType);
        if (inst.allocatorTraceEnabled) {
            const auto event = OSIncrementAtomic(&inst.allocatorTraceCount);
            if (event < 64) {
                SYSLOG("X5000", "Non-contiguous allocation %s: size=0x%llX align=0x%llX type=%u chunk=0x%llX min=0x%llX",
                       result ? "succeeded" : "failed", requestSize, requestAlign, allocType, chunkSize, minBlockSize);
            } else if (event == 64) {
                SYSLOG("X5000", "Allocator trace limit reached; allocation continues");
            }
        }
        return result;
    };

    auto tryNonContiguousRange = [&](const UInt32 allocType) -> bool {
        if (roundedSize > blockGranularity &&
            (inst.orgTotalFreeMemory == nullptr || inst.orgTotalFreeMemory(self) >= roundedSize)) {
            UInt64 chunkSize = 1;
            UInt64 tmp       = roundedSize;
            while ((tmp & 1) == 0) {
                tmp >>= 1;
                chunkSize <<= 1;
            }
            if (tmp == 1) { chunkSize >>= 1; }

            if (chunkSize >= blockGranularity &&
                tryNonContiguous(roundedSize, align, allocType, chunkSize)) {
                return true;
            }
            if (chunkSize > blockGranularity) {
                const UInt64 halfChunk = chunkSize >> 1;
                if (halfChunk >= blockGranularity &&
                    tryNonContiguous(roundedSize, align, allocType, halfChunk)) {
                    return true;
                }
                if (halfChunk > blockGranularity &&
                    tryNonContiguous(roundedSize, align, allocType, blockGranularity)) {
                    return true;
                }
            }
        }
        return false;
    };

    // 3. Non-contiguous attempts in Non-Visible range [256MB, totalVRAMSize] (allocType = 1)
    if (tryNonContiguousRange(1)) { return true; }

    // 4. Fallback to Full UMA VRAM range [reservedNdrvEnd, totalVRAMSize] (allocType = 2):
    //    On AMD APUs, BAR0-visible and non-visible UMA are backed by identical system RAM,
    //    and totalFreeMemory() includes free space in [reservedNdrvEnd, 256MB].
    if (!onlyNonVisible && inst.orgAllocate(self, elem, size, align, 2, false, orig, opType)) { return true; }
    if (tryNonContiguousRange(2)) { return true; }

    // 5. Fine-grained non-contiguous fallback (16KB -> 4KB segment lengths with 64KB+ start alignment)
    //    when VRAM is heavily fragmented:
    if (pageAlignedSize > 0x4000ULL && minBlockSize <= 0x4000ULL) {
        const UInt64 align16K = (pageAlignedSize + 0x3FFFULL) & ~0x3FFFULL;
        if (inst.orgTotalFreeMemory == nullptr || inst.orgTotalFreeMemory(self) >= align16K) {
            if (tryNonContiguous(align16K, safeAlign, 1, 0x4000ULL) ||
                tryNonContiguous(align16K, safeAlign, 2, 0x4000ULL)) {
                return true;
            }
        }
    }
    if (pageAlignedSize > PAGE_SIZE && minBlockSize <= PAGE_SIZE) {
        if (tryNonContiguous(pageAlignedSize, safeAlign, 1, PAGE_SIZE) ||
            tryNonContiguous(pageAlignedSize, safeAlign, 2, PAGE_SIZE)) {
            return true;
        }
    }

    return false;
}

void X5000::wrapRTHardwareDidStart(void* const self)
{
    FunctionCast(wrapRTHardwareDidStart, singleton().orgRTHardwareDidStart)(self);
    if (self != nullptr && singleton().gfxOffSupportedOffset != 0) {
        *reinterpret_cast<UInt8*>(reinterpret_cast<uintptr_t>(self) + singleton().gfxOffSupportedOffset) = 0;
        NRed::singleton().setProp32("NRedGFXOFFDisabled", 1);
        DBGLOG("X5000", "Disabled GFXOFF in RTHardware (offset 0x%x)", singleton().gfxOffSupportedOffset);
    }
}

UInt32 X5000::returnZero() { return 0; }

// Replaces SDMA1 field with SDMA0 because we don't have SDMA1
// TODO: Investigate why this is needed.
static void* fixAccelGroup(void* const group)
{
    if (group != nullptr) { getMember<void*>(group, 0x18) = getMember<void*>(group, 0x10); }
    return group;
}

void* X5000::wrapObtainAccelChannelGroup(void* const self, const UInt32 priority)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup, singleton().orgObtainAccelChannelGroup)(self, priority));
}

void* X5000::wrapObtainAccelChannelGroup1304(void* const self, const UInt32 priority, void* const task)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup1304, singleton().orgObtainAccelChannelGroup)(self, priority, task));
}

UInt32 X5000::wrapHwlConvertChipFamily(void* const self, const UInt32 family, const UInt32 revision)
{
    DBGLOG("X5000", "HwlConvertChipFamily >> (self: %p family: 0x%X revision: 0x%X)", self, family, revision);
    if (family == AMD_FAMILY_RAVEN) {
        auto& settings          = singleton().chipSettingsField(self);
        settings.isArcticIsland = 1;
        settings.isRaven        = 1;
        if (NRed::singleton().getAttributes().isRenoir()) {
            settings.htileAlignFix = 1;
            settings.applyAliasFix = 1;
        }
        else if (!NRed::singleton().getAttributes().isRaven2()) {
            settings.depthPipeXorDisable = 1;
        }
        settings.isDcn1           = 1;    // what to do about this?
        settings.metaBaseAlignFix = 1;
        return ADDR_CHIP_FAMILY_AI;
    }
    return FunctionCast(wrapHwlConvertChipFamily, singleton().orgHwlConvertChipFamily)(self, family, revision);
}

UInt32 X5000::computeSubmitCommandBuffer(void* const self, void* const info)
{
    singleton().notifyGfxAccess(singleton().hwChannelHWInterfaceField(self));
    return FunctionCast(computeSubmitCommandBuffer, singleton().orgPM4SubmitCommandBuffer)(self, info);
}

//  -- VRR was introduced on macOS Big Sur. --

namespace
{

    class Constants
    {
        static void _setVrrTimestampInfoVentura(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                                const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfoVentura();
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = vTotalMin;
            vrrTimestampInfo.horizontalLineTime         = horizontalLineTime;
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = vTotalMin;
            vrrTimestampInfo.vTotalMax                  = vTotalMax;
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _setVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                         const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfo();
            vrrTimestampInfo.field0                     = 0;
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.horizontalLineTime         = static_cast<UInt32>(horizontalLineTime);
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.vTotalMax                  = static_cast<UInt32>(vTotalMax);
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _calcAndSetVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self,
                                                const FramebufferInfo* const       fbInfo,
                                                const IOTimingInformation&         timingInfo)
        {
            assert(currentKernelVersion() >= MACOS_11);

            if (!fbInfo->isOnline) { return; }

            const auto vTotalMin =
                timingInfo.detailedInfo.v2.verticalBlanking + timingInfo.detailedInfo.v2.verticalActive;
            const auto vTotalMax  = vTotalMin + timingInfo.detailedInfo.v2.verticalBlankingExtension;
            const auto pixelClock = timingInfo.detailedInfo.v2.pixelClock;
            if (pixelClock == 0) { singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, 0); }
            else {
                const auto hTotal =
                    timingInfo.detailedInfo.v2.horizontalBlanking + timingInfo.detailedInfo.v2.horizontalActive;
                const auto hLineTimeNs = static_cast<UInt64>(hTotal) * 1000000000ULL / pixelClock;
                UInt64     horizontalLineTime;
                nanoseconds_to_absolutetime(hLineTimeNs, &horizontalLineTime);
                singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, horizontalLineTime);
            }
        }

        static void _calcAndSetVrrTimestampInfoDummy(AMDRadeonX5000_AMDHWDisplay*, const FramebufferInfo*,
                                                     const IOTimingInformation&)
        { assert(currentKernelVersion() <= MACOS_10_15_X); }

    public:
        static const Constants singleton;

        void (*setVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, UInt64 vTotalMin, UInt64 vTotalMax,
                                    const UInt64 horizontalLineTime){nullptr};
        void (*calcAndSetVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, const FramebufferInfo* fbInfo,
                                           const IOTimingInformation& timingInfo){_calcAndSetVrrTimestampInfoDummy};

        explicit Constants()
        {
            if (currentKernelVersion() < MACOS_11) { return; }

            this->calcAndSetVrrTimestampInfo = _calcAndSetVrrTimestampInfo;
            this->setVrrTimestampInfo =
                currentKernelVersion() >= MACOS_13 ? _setVrrTimestampInfoVentura : _setVrrTimestampInfo;
        }
    };

    const Constants Constants::singleton;

    constexpr inline auto& vrrConstants = Constants::singleton;

}    // namespace

bool X5000::fixedGetDisplayInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt32 fbIndex, const bool isCRTEnabled,
                                const bool ignoreCRTOffsetCheck, IOFramebuffer* const fb, FramebufferInfo* const fbInfo)
{
    if (fb == nullptr || fbIndex >= self->supportedDisplayCount()) { return false; }

    fbInfo->crtOffset   = 0;
    fbInfo->size        = 0;
    fbInfo->crtSize     = 0;
    fbInfo->pitch       = 0;
    fbInfo->rect.width  = 0;
    fbInfo->rect.height = 0;

    auto& displayState = self->displayStates()[fbIndex];

    displayState.framebuffer = fb;
    displayState.status.setIsEnabled(isCRTEnabled);
    displayState.status.setIsIOFBFlipEnabled(true);
    displayState.status.setIsAccelBacked(false);

    uintptr_t wsaa = -1ULL;
    if (fb->getAttribute(ATTRIBUTE_WSAA, &wsaa) == kIOReturnSuccess) {
        self->wsaaAttributes()[fbIndex] = static_cast<UInt32>(wsaa);
        displayState.status.setIsWSAASupported(true);
    }
    else {
        displayState.status.setIsWSAASupported(false);
    }

    uintptr_t  dpt    = 0;
    const auto dptRet = fb->getAttribute(ATTRIBUTE_DISPLAY_PIPE_TRANSACTION, &dpt);
    displayState.status.setIsDPTSupported(dptRet == kIOReturnSuccess || dptRet == kIOReturnNotReady);

    const auto crtOffset = OSDynamicCast(OSData, fb->getProperty("ATY,fb_offset"));
    if (crtOffset != nullptr) {
        const auto data = crtOffset->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtOffset = *static_cast<const UInt64*>(data); }
    }

    const auto crtSize = OSDynamicCast(OSData, fb->getProperty("ATY,fb_size"));
    if (crtSize != nullptr) {
        const auto data = crtSize->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtSize = *static_cast<const UInt32*>(data); }
    }

    auto aperture = fb->getApertureRange(kIOFBSystemAperture);
    auto vram     = fb->getVRAMRange();

    fbInfo->isMapped = aperture != nullptr && vram != nullptr;

    [[clang::suppress]] OSSafeReleaseNULL(aperture);
    [[clang::suppress]] OSSafeReleaseNULL(vram);

    self->getDisplayModeViewportSpecificInfo(fbIndex, &self->viewportStartYs()[fbIndex],
                                             &self->viewportHeights()[fbIndex]);

    uintptr_t isOnline = 0;
    // In IOKit, an IOFramebuffer instance represents a single display pipe/connector,
    // so its internal connection index is typically 0. Passing the card-level fbIndex
    // (1, 2, 3...) causes getAttributeForConnection to return kIOReturnBadArgument.
    // Query connection index 0 first, and fallback to fbIndex if connection 0 fails.
    IOReturn onlineRet = fb->getAttributeForConnection(0, kConnectionEnable, &isOnline);
    if (onlineRet != kIOReturnSuccess && fbIndex != 0) {
        onlineRet = fb->getAttributeForConnection(static_cast<IOIndex>(fbIndex), kConnectionEnable, &isOnline);
    }

    bool online = (onlineRet == kIOReturnSuccess && isOnline != 0);

    // Fallback guard for early-boot race conditions:
    // If the kernel and WindowServer start quickly (e.g. SetApfsTrimTimeout=0, fast NVMe, non-verbose boot),
    // WindowServer queries display status while asynchronous DCN/eDP link training is still settling.
    // Intentionally cover both LVDS and eDP embedded panels, but not external-only configurations.
    // When CRT is enabled for the primary built-in display (fbIndex == 0 && hasEmbeddedPanel()), guarantee online status
    // to prevent CoreDisplay from logging "Setting offline display 0x00000000 main" and aborting WindowServer.
    if (!online && isCRTEnabled && fbIndex == 0 && Backlight::singleton().hasEmbeddedPanel()) {
        online = true;
    }

    fbInfo->isOnline = online;

    self->fedsParamInfo()[fbIndex].crtIndex = 0;
    self->fedsParamInfo()[fbIndex].scaledW  = 0;
    self->fedsParamInfo()[fbIndex].scaledH  = 0;
    self->fedsParamInfo()[fbIndex].srcW     = 0;
    self->fedsParamInfo()[fbIndex].srcH     = 0;

    self->scalerFlags()[fbIndex] = 0;

    IODisplayModeID displayMode = 0;
    IOIndex         depth       = 0;
    if (fb->getCurrentDisplayMode(&displayMode, &depth) == kIOReturnSuccess) {
        if (fb->getPixelInformation(displayMode, depth, kIOFBSystemAperture, &displayState.pixelInfo)
            == kIOReturnSuccess)
        {
            const auto bytesPerPixel = displayState.pixelInfo.bitsPerPixel / 8;
            if (bytesPerPixel != 0) { fbInfo->pitch = displayState.pixelInfo.bytesPerRow / bytesPerPixel; }
            fbInfo->rect.width  = displayState.pixelInfo.activeWidth;
            fbInfo->rect.height = displayState.pixelInfo.activeHeight;
        }

        IODisplayModeInformation modeInfo;
        memset(&modeInfo, 0, sizeof(modeInfo));
        if (fb->getInformationForDisplayMode(displayMode, &modeInfo) == kIOReturnSuccess
            && (modeInfo.flags & kDisplayModeAcceleratorBackedFlag) != 0)
        {
            displayState.status.setIsAccelBacked(true);
            self->fedsParamInfo()[fbIndex].crtIndex = 1;
        }

        IOTimingInformation timingInfo;
        memset(&timingInfo, 0, sizeof(timingInfo));
        timingInfo.flags = kIODetailedTimingValid;
        if (fb->getTimingInfoForDisplayMode(displayMode, &timingInfo) == kIOReturnSuccess
            && (timingInfo.flags & kIODetailedTimingValid) != 0)
        {
            self->scalerFlags()[fbIndex] = timingInfo.detailedInfo.v2.scalerFlags;

            if (self->fedsParamInfo()[fbIndex].crtIndex == 1) {
                self->fedsParamInfo()[fbIndex].scaledW = timingInfo.detailedInfo.v2.horizontalActive;
                self->fedsParamInfo()[fbIndex].scaledH = timingInfo.detailedInfo.v2.verticalActive;
                self->fedsParamInfo()[fbIndex].srcW    = timingInfo.detailedInfo.v2.horizontalScaled;
                self->fedsParamInfo()[fbIndex].srcH    = timingInfo.detailedInfo.v2.verticalScaled;
            }

            vrrConstants.calcAndSetVrrTimestampInfo(self, fbInfo, timingInfo);
        }
    }

    auto& hwSpecificInfo = self->crtHWSpecificInfos()[fbIndex];

    auto ret = true;

    if (!isCRTEnabled
        || (!ignoreCRTOffsetCheck && fbInfo->crtOffset >= self->getHWInterface()->getHWMemory()->getVisibleSize()))
    {
        fbInfo->crtOffset = 0;
        fbInfo->size      = 0;
    }
    else {
        CRTHWDepth hwDepth;
        switch (displayState.pixelInfo.bitsPerPixel) {
            case 8: {
                hwDepth = CRTHWDepth::DEPTH_8;
            } break;
            case 16: {
                hwDepth = CRTHWDepth::DEPTH_16;
            } break;
            case 32: {
                hwDepth = CRTHWDepth::DEPTH_32;
            } break;
            case 64: {
                hwDepth = CRTHWDepth::DEPTH_64;
            } break;
            default: {
                hwDepth = CRTHWDepth::DEPTH_32;
                ret     = false;
            } break;
        }
        DBGLOG("X5000", "%s hwDepth=%s for bpp %d", __func__, stringifyCRTHWDepth(hwDepth),
               displayState.pixelInfo.bitsPerPixel);
        hwSpecificInfo.graphDepth = hwDepth;

        CRTHWFormat hwFormat;    // bug fix - original code only handled depth 64 and format 10
        switch (displayState.pixelInfo.bitsPerComponent) {
            case 8: {
                hwFormat = CRTHWFormat::FORMAT_8;
            } break;
            case 10: {
                hwFormat = CRTHWFormat::FORMAT_10;
            } break;
            case 12: {
                hwFormat = CRTHWFormat::FORMAT_12;
            } break;
            default: {
                hwFormat = CRTHWFormat::FORMAT_8;
                ret      = false;
            } break;
        }
        DBGLOG("X5000", "%s hwFormat=%s for bpc %d", __func__, stringifyCRTHWFormat(hwFormat),
               displayState.pixelInfo.bitsPerComponent);
        hwSpecificInfo.graphFormat = hwFormat;

        switch (hwDepth) {
            case CRTHWDepth::DEPTH_8: {
                hwSpecificInfo.bytesPerPixel = 1;
            } break;
            case CRTHWDepth::DEPTH_16: {
                hwSpecificInfo.bytesPerPixel = 2;
            } break;
            case CRTHWDepth::DEPTH_32: {
                hwSpecificInfo.bytesPerPixel = 4;
            } break;
            case CRTHWDepth::DEPTH_64: {
                hwSpecificInfo.bytesPerPixel = 8;
            } break;
        }
        hwSpecificInfo.pixelMode = self->getPixelMode(hwDepth, hwFormat);
        DBGLOG("X5000", "%s hwSpecificInfo.pixelMode=%s", __func__, stringifyATIPixelMode(hwSpecificInfo.pixelMode));
        hwSpecificInfo.format = self->getPixelFormat(hwSpecificInfo.pixelMode);
        DBGLOG("X5000", "%s hwSpecificInfo.format=%s", __func__, stringifyATIFormat(hwSpecificInfo.format));
        hwSpecificInfo.isInterlaced = self->isDisplayInterlaceEnabled(fbIndex);
        displayState.status.setIsInterlaced(hwSpecificInfo.isInterlaced);

        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = fbInfo->rect.width;
        surfInfoInput.height       = fbInfo->rect.height;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.resourceType = ADDR_RSRC_TEX_2D;
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        surfInfoInput.swizzleMode =
            self->getHWInterface()->getHWAlignManager()->getPreferredSwizzleMode2(&surfInfoInput);
        self->savedSwizzleModes()[fbIndex]  = surfInfoInput.swizzleMode;
        self->swizzleModes()[fbIndex]       = surfInfoInput.swizzleMode;
        self->savedResourceTypes()[fbIndex] = surfInfoInput.resourceType;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput,
                                                                         &self->surfInfoOutputs()[fbIndex])
            == kIOReturnSuccess)
        {
            fbInfo->size = fbInfo->pitch * self->surfInfoOutputs()[fbIndex].height * hwSpecificInfo.bytesPerPixel;
        }
        else {
            fbInfo->size = 0;
        }
        displayState.status.setIsEnabled(true);
    }

    AMDHWDisplayState::Status combinedStatus;
    for (UInt32 i = 0; i < self->supportedDisplayCount(); i += 1) { combinedStatus |= self->displayStates()[i].status; }
    self->combinedStatus() = combinedStatus;

    fbInfo->savedSize = fbInfo->size;

    if (self->fedsParamInfo()[fbIndex].crtIndex != 0) {
        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = self->fedsParamInfo()[fbIndex].scaledW;
        surfInfoInput.height       = self->fedsParamInfo()[fbIndex].scaledH;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.swizzleMode  = self->savedSwizzleModes()[fbIndex];
        surfInfoInput.resourceType = self->savedResourceTypes()[fbIndex];
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT surfInfoOutput;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput, &surfInfoOutput)
            == kIOReturnSuccess)
        {
            fbInfo->savedSize =
                static_cast<UInt64>(surfInfoOutput.height) * surfInfoOutput.pitch * hwSpecificInfo.bytesPerPixel;
        }
    }

    UInt64 baseAlign = self->surfInfoOutputs()[fbIndex].baseAlign;
    UInt8  shift     = 0;
    while (page_size < baseAlign) {
        baseAlign >>= 1;
        shift      += 1;
    }
    fbInfo->pageCount = alignValue(page_size << shift);

    return ret;
}

void X5000::fixedGetSurfaceInfo(AMDRadeonX5000_AMDHWAlignManager* const self, AMD_SURFACE_INFO_STRUCT* const pStruct)
{
    if (pStruct == nullptr) { return; }

    pStruct->outWidth  = 0;
    pStruct->outHeight = 0;

    if (pStruct->version != 1 || pStruct->revision != 0 || pStruct->sizeOf != sizeof(*pStruct)) { return; }

    ADDR2_COMPUTE_SURFACE_INFO_INPUT input;
    input.width         = pStruct->inWidth;
    input.height        = pStruct->inHeight;
    input.bpp           = pStruct->bytesPerPixel * 8;
    input.resourceType  = ADDR_RSRC_TEX_2D;
    input.numSamples    = 1;
    input.numSlices     = 1;
    input.flags.display = 1;
    input.swizzleMode   = self->getPreferredSwizzleMode2(&input);    // this was hardcoded to 0xa

    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT output;
    if (self->getSurfaceInfo2(&input, &output) == kIOReturnSuccess) {
        pStruct->outWidth      = static_cast<UInt16>(output.pitch);
        pStruct->outHeight     = static_cast<UInt16>(output.height);
        pStruct->outTilingMode = input.swizzleMode;    // I think AMD forgot this, albeit seemingly unused
    }
}
