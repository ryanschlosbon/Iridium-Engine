// M7R R5c.8: see AllocationTrace.h.
#include "qualification/harness/AllocationTrace.h"

#include "profiling/CpuAllocationProfile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#endif

namespace Iridium {

    namespace {

        constexpr size_t MaximumRecords = 16384;
        constexpr uint32_t MaximumDepth = 40;
        constexpr size_t ReportedStacks = 32;
        constexpr size_t ReportedFrames = 24;

        struct Record {
            uint64_t frame = 0;
            uint64_t bytes = 0;
            uint32_t thread = 0;
            uint32_t depth = 0;
            void* frames[MaximumDepth]{};
        };

        struct TraceState {
            std::vector<Record> records;
            std::atomic<uint32_t> count{ 0 };
            std::atomic<uint64_t> overflow{ 0 };
            // Allocation frames with a serial at or above this are traced.
            std::atomic<uint64_t> armSerial{ UINT64_MAX };
        };

        std::atomic<TraceState*> g_trace{ nullptr };

        // Runs inside operator new: no allocation, no locks.
        void observeFrameAllocation(std::size_t bytes) noexcept {
            TraceState* const state = g_trace.load(std::memory_order_acquire);
            if (!state) return;
            const uint64_t serial = cpu_allocation_detail::allocationFrameSerial.load(
                std::memory_order_relaxed);
            if (serial < state->armSerial.load(std::memory_order_relaxed)) return;
            const uint32_t index = state->count.fetch_add(1, std::memory_order_relaxed);
            if (index >= state->records.size()) {
                state->overflow.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            Record& record = state->records[index];
            record.frame = serial;
            record.bytes = bytes;
#if defined(_WIN32)
            record.thread = GetCurrentThreadId();
            record.depth = RtlCaptureStackBackTrace(1, MaximumDepth, record.frames,
                nullptr);
#endif
        }

        std::string jsonEscape(const std::string& text) {
            std::string out;
            out.reserve(text.size());
            for (const char character : text) {
                switch (character) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                default:
                    if (static_cast<unsigned char>(character) < 0x20) out += ' ';
                    else out += character;
                }
            }
            return out;
        }

        // Frames of the allocation path itself (dropped from the front of
        // every stack).
        bool allocatorFrame(const std::string& symbol) {
            for (const char* name : { "observeFrameAllocation", "recordAllocation",
                    "allocateUnaligned", "allocateAligned", "operator new" })
                if (symbol.find(name) != std::string::npos) return true;
            return false;
        }

    } // namespace

    struct AllocationTrace::Impl {
        uint64_t firstSteadyFrame = 1;
        uint64_t steadyFrames = 0;
        TraceState state;
    };

    AllocationTrace::AllocationTrace(uint64_t firstSteadyFrame)
        : impl_(std::make_unique<Impl>()) {
        impl_->firstSteadyFrame = (std::max)(firstSteadyFrame, uint64_t{ 1 });
        impl_->state.records.resize(MaximumRecords);
        g_trace.store(&impl_->state, std::memory_order_release);
        setCpuFrameAllocationObserver(&observeFrameAllocation);
    }

    AllocationTrace::~AllocationTrace() {
        setCpuFrameAllocationObserver(nullptr);
        g_trace.store(nullptr, std::memory_order_release);
    }

    void AllocationTrace::onFrameBegin(bool measured, uint64_t measuredIndex) noexcept {
        if (!measured) return;
        if (measuredIndex >= impl_->firstSteadyFrame) ++impl_->steadyFrames;
        if (impl_->state.armSerial.load(std::memory_order_relaxed) != UINT64_MAX)
            return;
        // This frame's allocation frame has begun: arm it if it is steady,
        // otherwise from the next one when that is.
        const uint64_t serial = cpu_allocation_detail::allocationFrameSerial.load(
            std::memory_order_relaxed);
        if (measuredIndex >= impl_->firstSteadyFrame)
            impl_->state.armSerial.store(serial, std::memory_order_relaxed);
        else if (measuredIndex + 1u >= impl_->firstSteadyFrame)
            impl_->state.armSerial.store(serial + 1u, std::memory_order_relaxed);
    }

    std::string AllocationTrace::finishJson() {
        TraceState& state = impl_->state;
        setCpuFrameAllocationObserver(nullptr);
        g_trace.store(nullptr, std::memory_order_release);
        const uint32_t recorded = (std::min)(
            state.count.load(std::memory_order_relaxed),
            static_cast<uint32_t>(state.records.size()));

        // Symbols for every distinct address.
        std::map<void*, std::string> symbols;
        for (uint32_t index = 0; index < recorded; ++index)
            for (uint32_t frame = 0; frame < state.records[index].depth; ++frame)
                symbols.emplace(state.records[index].frames[frame], std::string{});
        bool symbolsLoaded = false;
#if defined(_WIN32)
        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        symbolsLoaded = SymInitialize(process, nullptr, TRUE) != FALSE;
        for (auto& [address, text] : symbols) {
            const DWORD64 value = reinterpret_cast<DWORD64>(address);
            char moduleName[MAX_PATH] = "?";
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCSTR>(address), &module) && module) {
                char path[MAX_PATH]{};
                GetModuleFileNameA(module, path, MAX_PATH);
                const char* base = std::max(std::strrchr(path, '\\'),
                    std::strrchr(path, '/'));
                std::snprintf(moduleName, sizeof(moduleName), "%s",
                    base ? base + 1 : path);
            }
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "%s+0x%llx", moduleName,
                static_cast<unsigned long long>(module
                    ? value - reinterpret_cast<DWORD64>(module) : value));
            text = buffer;
            if (!symbolsLoaded) continue;
            alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + 512]{};
            SYMBOL_INFO* const symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, value, &displacement, symbol)) {
                text = std::string(moduleName) + "!" + symbol->Name;
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(process, value, &lineDisplacement, &line)) {
                    const char* file = line.FileName;
                    const char* src = std::strstr(file, "\\src\\");
                    text += " (" + std::string(src ? src + 1 : file) + ":" +
                        std::to_string(line.LineNumber) + ")";
                }
            }
        }
        if (symbolsLoaded) SymCleanup(process);
#endif

        // Group by the stack below the allocator frames.
        struct Group {
            uint64_t count = 0;
            uint64_t bytes = 0;
            uint64_t mainThreadCount = 0;
        };
        std::map<std::vector<std::string>, Group> groups;
        std::map<uint64_t, uint64_t> perFrame;
        uint64_t totalBytes = 0;
#if defined(_WIN32)
        const DWORD mainThread = GetCurrentThreadId();
#else
        const uint32_t mainThread = 0;
#endif
        for (uint32_t index = 0; index < recorded; ++index) {
            const Record& record = state.records[index];
            std::vector<std::string> frames;
            uint32_t frame = 0;
            while (frame < record.depth && allocatorFrame(symbols[record.frames[frame]]))
                ++frame;
            for (; frame < record.depth && frames.size() < ReportedFrames; ++frame)
                frames.push_back(symbols[record.frames[frame]]);
            Group& group = groups[frames];
            ++group.count;
            group.bytes += record.bytes;
            if (record.thread == mainThread) ++group.mainThreadCount;
            ++perFrame[record.frame];
            totalBytes += record.bytes;
        }
        std::vector<std::pair<const std::vector<std::string>*, Group>> ordered;
        for (const auto& [frames, group] : groups) ordered.push_back({ &frames, group });
        std::ranges::stable_sort(ordered, [](const auto& lhs, const auto& rhs) {
            return lhs.second.count > rhs.second.count;
        });
        uint64_t maximumPerFrame = 0;
        for (const auto& [frame, count] : perFrame)
            maximumPerFrame = (std::max)(maximumPerFrame, count);

        std::ostringstream json;
        json << "{\"passed\":true,\"first_steady_frame\":" << impl_->firstSteadyFrame
             << ",\"steady_frames\":" << impl_->steadyFrames
             << ",\"allocations\":" << recorded
             << ",\"bytes\":" << totalBytes
             << ",\"frames_with_allocations\":" << perFrame.size()
             << ",\"maximum_per_frame\":" << maximumPerFrame
             << ",\"overflow\":" << state.overflow.load(std::memory_order_relaxed)
             << ",\"symbols\":\"" << (symbolsLoaded ? "dbghelp" : "none") << "\""
             << ",\"stacks\":[";
        for (size_t index = 0; index < ordered.size() && index < ReportedStacks; ++index) {
            const auto& [frames, group] = ordered[index];
            json << (index ? "," : "") << "{\"count\":" << group.count
                 << ",\"bytes\":" << group.bytes
                 << ",\"main_thread\":" << group.mainThreadCount << ",\"frames\":[";
            for (size_t frame = 0; frame < frames->size(); ++frame)
                json << (frame ? "," : "") << '"' << jsonEscape((*frames)[frame]) << '"';
            json << "]}";
        }
        json << "]}";
        return json.str();
    }

} // namespace Iridium
