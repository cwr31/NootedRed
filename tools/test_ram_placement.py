#!/usr/bin/env python3
"""Exercise the Renoir RAM placement selectors and runtime switch on CPU stubs.

The selectors, counters and debug.nred.ram.enabled handler are compiled from
NootedRed/X5000.cpp against fake resource objects. Kernel objects, IOKit and the
GPU are not involved.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    while text[end] in " \t":
        end += 1
    if text[end] == ";":
        end += 1
    return text[start:end]


PREAMBLE = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
using UInt8 = uint8_t;
using UInt32 = uint32_t;
using UInt64 = unsigned long long;
using SInt32 = int32_t;
constexpr UInt64 PAGE_MASK = 4095;
constexpr int EINVAL = 22;
struct OSObject {};
template<typename T> T& getMember(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}
struct sysctl_oid {};
struct sysctl_req {
    int oldValue;
    bool hasNew;
    int newValue;
    unsigned long long newptr;
};
int sysctl_handle_int(sysctl_oid*, void* value, int, sysctl_req* request) {
    request->oldValue = *static_cast<int*>(value);
    if (request->hasNew) { *static_cast<int*>(value) = request->newValue; }
    return 0;
}
OSObject ramMemory, videoMemory, deviceCache, otherCache;
bool renoir = true;
bool isRenoirResource(void*) { return renoir; }
bool isRAMMemory(const OSObject* memory) { return memory == &ramMemory; }
bool isDeviceCache(const OSObject* cache) { return cache == &deviceCache; }
"""

TESTS = r"""
unsigned checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", label); }
}

struct Resource {
    alignas(8) UInt8 bytes[0x200];
    Resource(UInt8 type, UInt32 textureFlags) {
        memset(bytes, 0, sizeof(bytes));
        getMember<UInt8>(bytes, kResourceTypeOffset) = type;
        getMember<UInt32>(bytes, kTextureFlagsOffset) = textureFlags;
        getMember<UInt64>(bytes, kResourceSizeOffset) = 0x10000;
    }
    template<typename T> T& at(size_t offset) { return getMember<T>(bytes, offset); }
};

UInt32 textureFlags(ATIFormat format, UInt32 copyKind, UInt32 extra = 0) {
    return static_cast<UInt32>(format) | (copyKind << 26) | extra;
}

int shared = 0;

RAMClass select(Resource& resource, bool withShared = true) {
    return nredRAMSelectAllocation(resource.bytes, withShared ? &shared : nullptr);
}

void enable(UInt32 policies) { ramEnabled = policies; }

Resource privateBuffer() {
    Resource resource(kStandardResourceType, textureFlags(ATIFormat::Null, kPrivateCopyKind));
    resource.at<OSObject*>(kBackingMemoryOffset) = &ramMemory;
    return resource;
}

Resource compositionCache() {
    Resource resource(kVidMemSharedType, textureFlags(ATIFormat::BGRA8, kStagingCopyKind));
    resource.at<UInt32>(kResourceFlagsOffset) = kDeviceCacheFlag;
    resource.at<UInt8>(kSurfaceFlagsOffset) = kSurfaceInitialized;
    resource.at<void*>(kSurfaceParamsOffset) = &shared;
    resource.at<OSObject*>(kSharedSurfaceOffset) = &deviceCache;
    resource.at<OSObject*>(kBackingMemoryOffset) = &ramMemory;
    return resource;
}

Resource compositionPlane() {
    Resource resource(kVidMemSharedType, textureFlags(ATIFormat::Rg8, kLinearCopyKind));
    resource.at<UInt8>(kSurfaceFlagsOffset) = kSurfaceInitialized;
    resource.at<void*>(kSurfaceParamsOffset) = &shared;
    resource.at<OSObject*>(kBackingMemoryOffset) = &ramMemory;
    return resource;
}

void testStandardResources() {
    constexpr UInt32 all = kRAMBuffers | kRAMResourcePolicies;
    enable(all);
    auto buffer = privateBuffer();
    check(select(buffer) == RAMClass::Private, "private buffer with RAM backing");
    check(select(buffer, false) == RAMClass::None, "standard resource needs a shared context");
    buffer.at<OSObject*>(kBackingMemoryOffset) = &videoMemory;
    check(select(buffer) == RAMClass::None, "private buffer needs RAM backing");
    buffer = privateBuffer();
    buffer.at<void*>(kSurfaceParamsOffset) = &shared;
    check(select(buffer) == RAMClass::None, "private resource with surface parameters stays native");
    buffer = privateBuffer();
    buffer.at<void*>(kMainMemoryOffset) = &shared;
    check(select(buffer) == RAMClass::None, "existing primary memory stays native");
    buffer = privateBuffer();
    buffer.at<UInt32>(kResourceFlagsOffset) = 1U << 26;
    check(select(buffer) == RAMClass::None, "display buffer flag stays native");
    buffer = privateBuffer();
    renoir = false;
    check(select(buffer) == RAMClass::None, "other accelerators stay native");
    renoir = true;
    enable(all & ~kRAMTextures);
    check(select(buffer) == RAMClass::None, "private buffers follow the texture switch");

    struct Case {
        ATIFormat format;
        UInt32 copyKind;
        UInt32 policy;
        RAMClass expected;
        const char* label;
    } cases[] = {
        {ATIFormat::BGRA8, kLinearCopyKind, kRAMTextures, RAMClass::Color, "BGRA8 texture"},
        {ATIFormat::RGBA8, kStagingCopyKind, kRAMTextures, RAMClass::Color, "RGBA8 texture"},
        {ATIFormat::RGBA16F, kLinearCopyKind, kRAMHighColor, RAMClass::HighColor, "RGBA16F texture"},
        {ATIFormat::BGRA1010102, kLinearCopyKind, kRAMHighColor, RAMClass::HighColor, "BGR10A2 texture"},
        {ATIFormat::R8, kLinearCopyKind, kRAMVideoPlanes, RAMClass::VideoPlane, "R8 plane"},
        {ATIFormat::Rg16, kLinearCopyKind, kRAMVideoPlanes, RAMClass::VideoPlane, "RG16 plane"},
        {ATIFormat::Depth32F, kDepthStencilCopyKind, kRAMDepthStencil, RAMClass::DepthStencil, "Depth32F"},
        {ATIFormat::Stencil8, kDepthStencilCopyKind, kRAMDepthStencil, RAMClass::DepthStencil, "Stencil8"},
    };
    for (const auto& c : cases) {
        Resource resource(kStandardResourceType, textureFlags(c.format, c.copyKind));
        enable(all);
        check(select(resource) == c.expected, c.label);
        enable(all & ~c.policy);
        check(select(resource) == RAMClass::None, c.label);
        enable(c.policy);
        check(select(resource) == c.expected, c.label);
        enable(all);
        resource.at<OSObject*>(kBackingMemoryOffset) = &ramMemory;
        check(select(resource) == RAMClass::None, "formatted texture with a backing stays native");
    }

    Resource depthColor(kStandardResourceType, textureFlags(ATIFormat::BGRA8, kDepthStencilCopyKind));
    enable(all);
    check(select(depthColor) == RAMClass::None, "color format on the depth path stays native");
    Resource highColor(kStandardResourceType, textureFlags(ATIFormat::RGBA16F, kLinearCopyKind));
    highColor.at<void*>(kSurfaceAuxOffsets[0]) = &shared;
    check(select(highColor) == RAMClass::None, "high color with auxiliary storage stays native");
    Resource color(kStandardResourceType, textureFlags(ATIFormat::BGRA8, kLinearCopyKind));
    color.at<void*>(kSurfaceAuxOffsets[0]) = &shared;
    check(select(color) == RAMClass::Color, "8-bit color ignores auxiliary storage as before");
    Resource other(0x82, textureFlags(ATIFormat::BGRA8, kLinearCopyKind));
    check(select(other) == RAMClass::None, "IOSurface resource type stays native");
}

void testComposition() {
    constexpr UInt32 all = kRAMBuffers | kRAMResourcePolicies;
    enable(all);
    auto cache = compositionCache();
    check(select(cache, false) == RAMClass::Composition, "composition cache");
    check(select(cache) == RAMClass::None, "composition cache with a shared context stays native");
    enable(all & ~kRAMComposition);
    check(select(cache, false) == RAMClass::None, "composition follows its switch");
    enable(all);
    cache.at<UInt8>(kSurfaceFlagsOffset) = kSurfaceInitialized | 0x20;
    check(select(cache, false) == RAMClass::None, "display eligible cache stays native");
    cache = compositionCache();
    cache.at<OSObject*>(kSharedSurfaceOffset) = &otherCache;
    check(select(cache, false) == RAMClass::None, "cache object must be an IOSurfaceDeviceCache");
    cache = compositionCache();
    cache.at<OSObject*>(kBackingMemoryOffset) = &videoMemory;
    check(select(cache, false) == RAMClass::None, "cache backing must be RAM");
    cache = compositionCache();
    cache.at<UInt32>(kTextureFlagsOffset) = textureFlags(ATIFormat::RGBA16F, kStagingCopyKind);
    check(select(cache, false) == RAMClass::None, "only 8-bit color caches");

    auto plane = compositionPlane();
    check(select(plane, false) == RAMClass::CompositionPlane, "composition plane");
    check(select(plane) == RAMClass::CompositionPlane, "composition plane accepts a shared context");
    enable(all & ~kRAMVideoPlanes);
    check(select(plane, false) == RAMClass::None, "composition plane needs the video plane switch");
    enable(all & ~kRAMComposition);
    check(select(plane, false) == RAMClass::None, "composition plane needs the composition switch");
    enable(all);
    plane.at<UInt32>(kTextureFlagsOffset) |= kStagedPageInFlag;
    check(select(plane, false) == RAMClass::None, "plane with flag bit 8 stays native");
    plane = compositionPlane();
    plane.at<void*>(kSurfaceDCCOffset) = &shared;
    check(select(plane, false) == RAMClass::None, "plane with DCC metadata stays native");
    plane = compositionPlane();
    plane.at<OSObject*>(kSharedSurfaceOffset) = &deviceCache;
    check(select(plane, false) == RAMClass::None, "plane with a cache object stays native");
}

int request(int value, bool write, int* readBack) {
    sysctl_req request{0, write, value, write ? 1ULL : 0ULL};
    const int error = handleRAMEnabled(nullptr, nullptr, 0, &request);
    if (readBack != nullptr) { *readBack = request.oldValue; }
    return error;
}

void testRuntimeSwitch() {
    ramRequested = kRAMTextures | kRAMComposition;
    ramAvailable = ramEnabled = 0;
    makeRAMPolicyAvailable(kRAMResourcePolicies & ~kRAMComposition);
    check(ramAvailable == (kRAMTextures | kRAMHighColor | kRAMVideoPlanes | kRAMDepthStencil),
          "installed allocation hook makes texture classes available");
    check(ramEnabled == kRAMTextures, "only requested classes start enabled");
    int current = -1;
    check(request(0, false, &current) == 0 && current == static_cast<int>(kRAMTextures), "read enabled mask");
    check(request(kRAMTextures | kRAMHighColor, true, nullptr) == 0
              && ramEnabled == (kRAMTextures | kRAMHighColor),
          "enable an available class at runtime");
    check(request(kRAMComposition, true, nullptr) == EINVAL && ramEnabled == (kRAMTextures | kRAMHighColor),
          "reject a class without its hook");
    check(request(1 << 12, true, nullptr) == EINVAL, "reject unknown bits");
    makeRAMPolicyAvailable(kRAMComposition);
    check(ramEnabled == (kRAMTextures | kRAMHighColor | kRAMComposition), "requested class enabled when installed");
    check(request(0, true, nullptr) == 0 && ramEnabled == 0, "disable all classes");
    auto buffer = privateBuffer();
    check(select(buffer) == RAMClass::None, "disabled switch keeps new resources native");
}

void testCounters() {
    countRAMAllocation(RAMClass::Private, true, 0x3000);
    countRAMAllocation(RAMClass::Private, true, 0x1000);
    countRAMAllocation(RAMClass::Buffer, false, 0x1000);
    const auto& counter = ramCounters[static_cast<size_t>(RAMClass::Private)];
    check(counter.count == 2 && counter.bytes == 0x4000, "placed allocations are counted per class");
    check(ramFailures == 1 && ramCounters[static_cast<size_t>(RAMClass::Buffer)].count == 0,
          "failed attempts are counted separately");
}

int main() {
    testStandardResources();
    testComposition();
    testRuntimeSwitch();
    testCounters();
    printf("%u checks, %u failures; CPU stubs only, no GPU/kernel allocation\n", checks, failures);
    return failures ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    source = (REPO / "NootedRed" / "X5000.cpp").read_text()
    formats = (REPO / "NootedRed" / "GPUDriversAMD" / "Accel" / "HWAlignManager.hpp").read_text()

    constants = "\n".join(re.findall(r"^\s*constexpr\s+(?:size_t|UInt8|UInt32)\s+k\w+(?:\[\])?\s*=[^;]*;",
                                     source, re.M))
    state = "\n".join(re.findall(r"^\s*(?:UInt32 ram(?:Requested|Available|Enabled)|UInt64 ramFailures)\{0\};",
                                 source, re.M))
    pieces = [
        block(formats, "enum struct ATIFormat"),
        constants,
        state,
        block(source, "bool ramPolicyEnabled("),
        block(source, "void makeRAMPolicyAvailable("),
        block(source, "enum class RAMClass"),
        block(source, "struct RAMCounter"),
        re.search(r"^\s*RAMCounter ramCounters\[[^;]*;", source, re.M).group(0),
        block(source, "void countRAMAllocation("),
        block(source, "int handleRAMEnabled("),
        re.search(r"^\s*ATIFormat textureFormat\([^\n]*", source, re.M).group(0),
        re.search(r"^\s*UInt32 textureCopyKind\([^\n]*", source, re.M).group(0),
        block(source, "enum class RAMTextureKind"),
        block(source, "RAMTextureKind ramTextureKind("),
        block(source, "bool hasSurfaceAuxStorage("),
        block(source, "RAMClass useRAMTexture("),
        block(source, "static RAMClass nredRAMSelectComposition("),
        block(source, "static RAMClass nredRAMSelectAllocation("),
    ]
    unit = PREAMBLE + "\n".join(pieces) + TESTS
    with tempfile.TemporaryDirectory(prefix="nred-ram-placement-test-") as temp:
        path, binary = Path(temp) / "test.cpp", Path(temp) / "test"
        path.write_text(unit)
        subprocess.run([
            "xcrun", "clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-Wno-unused-const-variable", "-Wno-unused-function", "-fsanitize=address,undefined",
            str(path), "-o", str(binary),
        ], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
