#include "profiling/CpuAllocationProfile.h"
#include "renderer/graph/RenderGraph.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

    using namespace Iridium;
    using namespace Iridium::RenderGraph;

    constexpr uint32_t PassCount = 1'000;
    constexpr uint64_t LookupCount = 1'000'000;
    constexpr size_t Repetitions = 5;
    constexpr uint64_t LookupGateNanoseconds = 10'000;
    volatile uintptr_t lookupSink = 0;

    CompiledGraph buildGraph(uint64_t& compileNanoseconds,
        CpuAllocationFrameSample& compileAllocations) {
        GraphCapacity capacity{};
        capacity.maxPasses = PassCount;
        capacity.maxLogicalResources = 1;
        capacity.maxResourceVersions = PassCount + 1;
        capacity.maxUsages = PassCount;
        capacity.maxDependencies = 1;
        RenderGraphBuilder builder(capacity);

        ResourceDesc desc{};
        desc.type = ResourceType::Image;
        desc.lifetime = ResourceLifetime::Transient;
        desc.image.format = Format::Rgba16Float;
        desc.image.extent = { 3840, 2160, 1 };
        ResourceHandle resource = builder.createResource("benchmark-image", desc);

        for (uint32_t index = 0; index < PassCount; ++index) {
            const PassHandle pass = builder.addPass("pass-" + std::to_string(index));
            resource = builder.write(pass, resource, Access::ColorAttachment,
                index == 0 ? LoadOp::Clear : LoadOp::Load);
        }

        beginCpuAllocationFrame();
        const auto start = std::chrono::steady_clock::now();
        CompileResult result = builder.compile();
        const auto stop = std::chrono::steady_clock::now();
        compileAllocations = endCpuAllocationFrame();
        compileNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
        if (!result.succeeded()) {
            throw std::runtime_error("Synthetic graph failed to compile");
        }
        return std::move(*result.graph);
    }

} // namespace

int main() {
    uint64_t compileNanoseconds = 0;
    CpuAllocationFrameSample compileAllocations{};
    CompiledGraph graph = buildGraph(compileNanoseconds, compileAllocations);
    const uint64_t topologyHash = graph.topologyHash();
    CompiledGraphCache cache;
    cache.store(std::move(graph));

    std::array<uint64_t, Repetitions> lookupTotals{};
    uint64_t allocationCalls = 0;
    uint64_t allocationBytes = 0;
    for (size_t repetition = 0; repetition < Repetitions; ++repetition) {
        beginCpuAllocationFrame();
        uintptr_t localSink = 0;
        const auto start = std::chrono::steady_clock::now();
        for (uint64_t lookup = 0; lookup < LookupCount; ++lookup) {
            const uint64_t lookupHash = topologyHash + (lookup & 1u);
            const CompiledGraph* found = cache.find(lookupHash);
            localSink += reinterpret_cast<uintptr_t>(found) ^ lookup;
        }
        const auto stop = std::chrono::steady_clock::now();
        const CpuAllocationFrameSample allocations = endCpuAllocationFrame();
        allocationCalls += allocations.allocationCount;
        allocationBytes += allocations.requestedBytes;
        lookupSink = localSink;
        lookupTotals[repetition] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
    }

    std::sort(lookupTotals.begin(), lookupTotals.end());
    const uint64_t medianTotal = lookupTotals[Repetitions / 2];
    const double medianNanoseconds = static_cast<double>(medianTotal) /
        static_cast<double>(LookupCount);
    std::cout << "{\"passes\":" << PassCount
        << ",\"compile_ns\":" << compileNanoseconds
        << ",\"compile_allocation_calls\":"
        << compileAllocations.allocationCount
        << ",\"compile_allocation_bytes\":"
        << compileAllocations.requestedBytes
        << ",\"lookup_count\":" << LookupCount
        << ",\"repetitions\":" << Repetitions
        << ",\"lookup_median_total_ns\":" << medianTotal
        << ",\"lookup_median_ns\":" << medianNanoseconds
        << ",\"lookup_allocation_calls\":" << allocationCalls
        << ",\"lookup_allocation_bytes\":" << allocationBytes
        << "}\n";

    if (allocationCalls != 0 || allocationBytes != 0 ||
        medianNanoseconds >= static_cast<double>(LookupGateNanoseconds)) {
        return 1;
    }
    return 0;
}
