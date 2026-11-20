// Metal benchmark for Renoir RAM placement.
//
// Measures where new private Metal buffers are placed, their GPU read, write and copy
// bandwidth, and the cost of creating a buffer and using it for the first time. Run it once
// per placement and change debug.nred.ram.enabled in between to compare placements in one
// boot. With --heap, the bandwidth buffers come from one private MTLHeap instead.
// See docs/RAM-Validation.md.
//
// Build: xcrun clang -fobjc-arc -O2 -framework Foundation -framework Metal -framework IOKit \
//            tools/ram_placement_bench.m -o build/tools/ram_placement_bench
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void read_sum(device const uint4 *src [[buffer(0)]], device uint *out [[buffer(1)]],\n"
     "                     constant uint &count [[buffer(2)]], uint id [[thread_position_in_grid]],\n"
     "                     uint threads [[threads_per_grid]]) {\n"
     "    uint4 sum = 0;\n"
     "    for (uint i = id; i < count; i += threads) { sum += src[i]; }\n"
     "    out[id] = sum.x ^ sum.y ^ sum.z ^ sum.w;\n"
     "}\n"
     "kernel void write_fill(device uint4 *dst [[buffer(0)]], constant uint &count [[buffer(2)]],\n"
     "                       uint id [[thread_position_in_grid]], uint threads [[threads_per_grid]]) {\n"
     "    for (uint i = id; i < count; i += threads) { dst[i] = uint4(i, i ^ 0x9e3779b9u, ~i, id); }\n"
     "}\n";

static const char *const kClasses[] = {"buffer", "private", "color", "highcolor", "videoplane",
                                       "depthstencil", "composition", "compositionplane"};
enum { kClassCount = sizeof(kClasses) / sizeof(kClasses[0]), kLanes = 1 << 18 };

typedef struct {
    uint64_t vid, sys;
    uint64_t coreMHz, memoryMHz;
    uint64_t bytes[kClassCount];
    bool hasCounters;
} Snapshot;

static double nowSeconds(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }

static double median(double *values, int count) {
    for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && values[j - 1] > values[j]; j--) {
            const double swap = values[j];
            values[j] = values[j - 1];
            values[j - 1] = swap;
        }
    }
    return count % 2 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
}

static bool readCounter(const char *name, uint64_t *value) {
    char key[96];
    snprintf(key, sizeof(key), "debug.nred.ram.%s", name);
    uint64_t result = 0;
    size_t length = sizeof(result);
    if (sysctlbyname(key, &result, &length, NULL, 0) != 0) { return false; }
    *value = length == sizeof(uint32_t) ? (uint32_t)result : result;
    return true;
}

static uint64_t statistic(NSDictionary *statistics, NSString *key) {
    id value = statistics[key];
    return [value isKindOfClass:NSNumber.class] ? [value unsignedLongLongValue] : 0;
}

static Snapshot snapshot(id<MTLDevice> device) {
    Snapshot result = {0};
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(device.registryID));
    if (service != IO_OBJECT_NULL) {
        CFTypeRef value = IORegistryEntryCreateCFProperty(service, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
        NSDictionary *statistics = CFBridgingRelease(value);
        if ([statistics isKindOfClass:NSDictionary.class]) {
            result.vid = statistic(statistics, @"inUseVidMemoryBytes");
            result.sys = statistic(statistics, @"inUseSysMemoryBytes");
            result.coreMHz = statistic(statistics, @"Core Clock(MHz)");
            result.memoryMHz = statistic(statistics, @"Memory Clock(MHz)");
        }
        IOObjectRelease(service);
    }
    result.hasCounters = true;
    for (int i = 0; i < kClassCount; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s_bytes", kClasses[i]);
        result.hasCounters = readCounter(name, &result.bytes[i]) && result.hasCounters;
    }
    return result;
}

static double mib(int64_t bytes) { return (double)bytes / (1024.0 * 1024.0); }

static void printDelta(const char *label, Snapshot before, Snapshot after) {
    printf("%s: vid %+.1f MiB, sys %+.1f MiB", label, mib((int64_t)(after.vid - before.vid)),
           mib((int64_t)(after.sys - before.sys)));
    if (before.hasCounters && after.hasCounters) {
        for (int i = 0; i < kClassCount; i++) {
            const uint64_t delta = after.bytes[i] - before.bytes[i];
            if (delta != 0) { printf(", %s %+.1f MiB", kClasses[i], mib((int64_t)delta)); }
        }
    }
    printf("\n");
}

static double run(id<MTLCommandQueue> queue, void (^encode)(id<MTLCommandBuffer>)) {
    id<MTLCommandBuffer> command = [queue commandBuffer];
    encode(command);
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "GPU command failed: %s\n", command.error.description.UTF8String);
        exit(1);
    }
    return command.GPUEndTime - command.GPUStartTime;
}

static void dispatch(id<MTLCommandBuffer> command, id<MTLComputePipelineState> pipeline, id<MTLBuffer> first,
                     id<MTLBuffer> second, uint32_t count) {
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:first offset:0 atIndex:0];
    if (second != nil) { [encoder setBuffer:second offset:0 atIndex:1]; }
    [encoder setBytes:&count length:sizeof(count) atIndex:2];
    [encoder dispatchThreads:MTLSizeMake(kLanes, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [encoder endEncoding];
}

static int option(int argc, char **argv, const char *name, int fallback, int low, int high) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            const int value = atoi(argv[i + 1]);
            if (value < low || value > high) {
                fprintf(stderr, "%s must be in %d...%d\n", name, low, high);
                exit(2);
            }
            return value;
        }
    }
    return fallback;
}

static bool flag(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) { return true; }
    }
    return false;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const int sizeMiB = option(argc, argv, "--mib", 128, 16, 1024);
        const int rounds = option(argc, argv, "--rounds", 10, 3, 1000);
        const int allocMiB = option(argc, argv, "--alloc-mib", 8, 1, 256);
        const int allocCount = option(argc, argv, "--alloc-count", 16, 1, 256);
        const bool useHeap = flag(argc, argv, "--heap");
        if (sizeMiB % 16 != 0) {
            fprintf(stderr, "--mib must be a multiple of 16\n");
            return 2;
        }

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            fprintf(stderr, "No Metal device\n");
            return 1;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:kSource options:nil error:&error];
        id<MTLComputePipelineState> reader =
            library ? [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"read_sum"] error:&error] : nil;
        id<MTLComputePipelineState> writer =
            library ? [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"write_fill"] error:&error] : nil;
        if (reader == nil || writer == nil) {
            fprintf(stderr, "Metal setup failed: %s\n", error.description.UTF8String);
            return 1;
        }

        uint64_t enabled = 0;
        printf("device: %s\n", device.name.UTF8String);
        printf("recommendedMaxWorkingSetSize: %.1f MiB, hasUnifiedMemory: %d\n",
               mib((int64_t)device.recommendedMaxWorkingSetSize), device.hasUnifiedMemory);
        if (readCounter("enabled", &enabled)) {
            printf("debug.nred.ram.enabled: 0x%llx\n", (unsigned long long)enabled);
        }
        else {
            printf("debug.nred.ram.enabled: unavailable\n");
        }

        const NSUInteger bytes = (NSUInteger)sizeMiB << 20;
        const uint32_t count = (uint32_t)(bytes / 16);
        const Snapshot before = snapshot(device);
        id<MTLHeap> heap = nil;
        if (useHeap) {
            const MTLSizeAndAlign layout = [device heapBufferSizeAndAlignWithLength:bytes
                                                                            options:MTLResourceStorageModePrivate];
            MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
            descriptor.storageMode = MTLStorageModePrivate;
            descriptor.size = 2 * ((layout.size + layout.align - 1) & ~(layout.align - 1));
            heap = [device newHeapWithDescriptor:descriptor];
        }
        id<MTLBuffer> source = useHeap ? [heap newBufferWithLength:bytes options:MTLResourceStorageModePrivate] :
                                         [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        id<MTLBuffer> destination = useHeap ? [heap newBufferWithLength:bytes options:MTLResourceStorageModePrivate] :
                                              [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        id<MTLBuffer> sums = [device newBufferWithLength:kLanes * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        if (source == nil || destination == nil || sums == nil) {
            fprintf(stderr, "Buffer allocation failed\n");
            return 1;
        }
        run(queue, ^(id<MTLCommandBuffer> command) {
          dispatch(command, writer, source, nil, count);
          dispatch(command, writer, destination, nil, count);
        });
        // PerformanceStatistics is refreshed periodically.
        [NSThread sleepForTimeInterval:2.0];
        char label[64];
        snprintf(label, sizeof(label), "placement of 2 x %d MiB private %s", sizeMiB, useHeap ? "heap buffers" : "buffers");
        printDelta(label, before, snapshot(device));

        double readTimes[rounds], writeTimes[rounds], copyTimes[rounds];
        for (int i = 0; i < rounds; i++) {
            readTimes[i] = run(queue, ^(id<MTLCommandBuffer> command) { dispatch(command, reader, source, sums, count); });
            writeTimes[i] = run(queue, ^(id<MTLCommandBuffer> command) { dispatch(command, writer, destination, nil, count); });
            copyTimes[i] = run(queue, ^(id<MTLCommandBuffer> command) {
              id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
              [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:bytes];
              [blit endEncoding];
            });
        }
        printf("bandwidth (median of %d): read %.1f GB/s, write %.1f GB/s, copy %.1f GB/s (read + write)\n", rounds,
               bytes / median(readTimes, rounds) / 1e9, bytes / median(writeTimes, rounds) / 1e9,
               2.0 * bytes / median(copyTimes, rounds) / 1e9);
        // Memory clock DPM changes bandwidth between runs, so record the last sampled clocks.
        const Snapshot clocks = snapshot(device);
        printf("sampled clocks: core %llu MHz, memory %llu MHz\n", (unsigned long long)clocks.coreMHz,
               (unsigned long long)clocks.memoryMHz);

        const NSUInteger allocBytes = (NSUInteger)allocMiB << 20;
        double createTimes[rounds], firstUseTimes[rounds], firstUseGPUTimes[rounds], secondUseTimes[rounds];
        const Snapshot allocBefore = snapshot(device);
        for (int i = 0; i < rounds; i++) {
            @autoreleasepool {
                NSMutableArray<id<MTLBuffer>> *buffers = [NSMutableArray arrayWithCapacity:allocCount];
                const double start = nowSeconds();
                for (int j = 0; j < allocCount; j++) {
                    id<MTLBuffer> buffer = [device newBufferWithLength:allocBytes options:MTLResourceStorageModePrivate];
                    if (buffer == nil) {
                        fprintf(stderr, "Buffer allocation failed\n");
                        return 1;
                    }
                    [buffers addObject:buffer];
                }
                const double created = nowSeconds();
                void (^fill)(id<MTLCommandBuffer>) = ^(id<MTLCommandBuffer> command) {
                  id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                  for (id<MTLBuffer> buffer in buffers) {
                      [blit fillBuffer:buffer range:NSMakeRange(0, allocBytes) value:(uint8_t)i];
                  }
                  [blit endEncoding];
                };
                firstUseGPUTimes[i] = run(queue, fill);
                const double firstUse = nowSeconds();
                firstUseTimes[i] = (firstUse - created) / allocCount;
                createTimes[i] = (created - start) / allocCount;
                run(queue, fill);
                secondUseTimes[i] = (nowSeconds() - firstUse) / allocCount;
            }
        }
        printf("allocation (median of %d rounds of %d x %d MiB, per buffer): create %.3f ms, first use %.3f ms wall "
               "(%.3f ms GPU), second use %.3f ms wall\n",
               rounds, allocCount, allocMiB, median(createTimes, rounds) * 1e3, median(firstUseTimes, rounds) * 1e3,
               median(firstUseGPUTimes, rounds) / allocCount * 1e3, median(secondUseTimes, rounds) * 1e3);
        printDelta("allocation rounds placed", allocBefore, snapshot(device));
    }
    return 0;
}
