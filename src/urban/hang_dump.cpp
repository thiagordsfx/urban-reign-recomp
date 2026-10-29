#include "urban/hang_dump.h"
#include "urban/call_profile.h"
#include "urban/hle_bindings.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <algorithm>
#include <map>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

namespace urban::hangdump
{
    namespace
    {
        std::atomic<PS2Runtime *> g_runtime{nullptr};

        const char *statusName(EeThreadStatus status)
        {
            switch (status)
            {
            case EeThreadStatus::Running: return "RUN";
            case EeThreadStatus::Ready: return "READY";
            case EeThreadStatus::Waiting: return "WAIT";
            case EeThreadStatus::WaitingSuspended: return "WAIT+SUSP";
            case EeThreadStatus::Suspended: return "SUSP";
            case EeThreadStatus::Dormant: return "DORMANT";
            }
            return "?";
        }

        const char *waitName(EeWaitReason reason)
        {
            switch (reason)
            {
            case EeWaitReason::None: return "";
            case EeWaitReason::Sleep: return "sleep";
            case EeWaitReason::Semaphore: return "sema";
            case EeWaitReason::EventFlag: return "evflag";
            case EeWaitReason::VSync: return "vsync";
            case EeWaitReason::External: return "external";
            case EeWaitReason::Mpeg: return "mpeg";
            }
            return "?";
        }

        std::string guestLabel(uint32_t pc)
        {
            char text[96];
            if (const SdkFunction *function = sdkFunctionAt(pc))
                std::snprintf(text, sizeof(text), "0x%08x (%.*s)", pc, static_cast<int>(function->name.size()), function->name.data());
            else
                std::snprintf(text, sizeof(text), "0x%08x", pc);
            return text;
        }

        void dumpGuestKernel()
        {
            PS2Runtime *runtime = g_runtime.load();
            if (!runtime)
                return;
            const EeKernelSnapshot snapshot = runtime->eeScheduler().snapshot();
            std::fprintf(stderr, "[hang-dump] ===== guest EE kernel (seq %llu, cycle %llu, slice end %llu, next event %llu, running thread %d) =====\n",
                         static_cast<unsigned long long>(snapshot.sequence), static_cast<unsigned long long>(snapshot.eeCycle),
                         static_cast<unsigned long long>(snapshot.sliceEndCycle), static_cast<unsigned long long>(snapshot.nextEventCycle),
                         snapshot.runningThreadId);
            for (const EeThreadSnapshot &t : snapshot.threads)
            {
                std::fprintf(stderr, "[hang-dump]   thread %2d %-9s %-8s id=%-3d prio=%-3d pc=%s ra=%s entry=0x%08x sp=0x%08x\n", t.id,
                             statusName(t.status), waitName(t.waitReason), t.waitId, t.currentPriority, guestLabel(t.pc).c_str(),
                             guestLabel(t.ra).c_str(), t.entry, t.sp);
            }
            // DMA channel CHCR/MADR/QWC: a channel stuck with STR (0x100) set stalls code that waits for DMA idle.
            static constexpr struct { const char *name; uint32_t base; } kDmaChannels[] = {
                {"VIF0", 0x10008000u}, {"VIF1", 0x10009000u}, {"GIF", 0x1000A000u}, {"IPUfrom", 0x1000B000u},
                {"IPUto", 0x1000B400u}, {"SIF0", 0x1000C000u}, {"SIF1", 0x1000C400u}, {"SIF2", 0x1000C800u},
                {"SPRfrom", 0x1000D000u}, {"SPRto", 0x1000D400u},
            };
            for (const auto &channel : kDmaChannels)
            {
                const uint32_t chcr = runtime->memory().readIORegister(channel.base);
                const uint32_t madr = runtime->memory().readIORegister(channel.base + 0x10u);
                const uint32_t qwc = runtime->memory().readIORegister(channel.base + 0x20u);
                std::fprintf(stderr, "[hang-dump]   dma %-7s @0x%08x chcr=0x%08x%s madr=0x%08x qwc=0x%x\n", channel.name, channel.base, chcr,
                             (chcr & 0x100u) ? " STR" : "    ", madr, qwc);
            }
            for (uint32_t timer = 0; timer < 4; ++timer)
            {
                const uint32_t base = 0x10000000u + timer * 0x800u;
                std::fprintf(stderr, "[hang-dump]   timer T%u count=0x%04x mode=0x%04x comp=0x%04x hold=0x%04x\n", timer,
                             runtime->memory().readIORegister(base), runtime->memory().readIORegister(base + 0x10u),
                             runtime->memory().readIORegister(base + 0x20u), runtime->memory().readIORegister(base + 0x30u));
            }
            std::fprintf(stderr, "[hang-dump]   INTC stat=0x%08x mask=0x%08x\n", runtime->memory().readIORegister(0x1000F000u),
                         runtime->memory().readIORegister(0x1000F010u));
            std::fprintf(stderr, "[hang-dump]   GS csr=0x%llx imr=0x%llx\n",
                         static_cast<unsigned long long>(runtime->memory().gs().csr.load()),
                         static_cast<unsigned long long>(runtime->memory().gs().imr));
            {
                const GSRegisters &gs = runtime->memory().gs();
                std::fprintf(stderr, "[hang-dump]   GS pmode=0x%llx smode2=0x%llx dispfb1=0x%llx display1=0x%llx dispfb2=0x%llx display2=0x%llx bgcolor=0x%llx\n",
                             (unsigned long long)gs.pmode, (unsigned long long)gs.smode2, (unsigned long long)gs.dispfb1,
                             (unsigned long long)gs.display1, (unsigned long long)gs.dispfb2, (unsigned long long)gs.display2,
                             (unsigned long long)gs.bgcolor);
            }
            std::fprintf(stderr, "[hang-dump]   counters dmaStarts=%llu gifCopies=%llu gsWrites=%llu vifWrites=%llu\n",
                         static_cast<unsigned long long>(runtime->memory().dmaStartCount()),
                         static_cast<unsigned long long>(runtime->memory().gifCopyCount()),
                         static_cast<unsigned long long>(runtime->memory().gsWriteCount()),
                         static_cast<unsigned long long>(runtime->memory().vifWriteCount()));
            // URBAN_DUMP_RAM=<file>: write all 32 MB of guest RAM (offline analysis of DMA chains, structures...).
            if (const char *path = std::getenv("URBAN_DUMP_RAM"); path && *path)
            {
                if (std::FILE *file = std::fopen(path, "wb"))
                {
                    std::fwrite(runtime->memory().getRDRAM(), 1, 32u * 1024u * 1024u, file);
                    std::fclose(file);
                    std::fprintf(stderr, "[hang-dump]   RAM written to %s\n", path);
                }
            }
            // URBAN_DUMP_VRAM=<file>: the host copy of the 4 MB GS VRAM (authoritative with PS2X_GS_GPU=0).
            if (const char *path = std::getenv("URBAN_DUMP_VRAM"); path && *path)
            {
                if (std::FILE *file = std::fopen(path, "wb"))
                {
                    std::fwrite(runtime->memory().getGSVRAM(), 1, 4u * 1024u * 1024u, file);
                    std::fclose(file);
                    std::fprintf(stderr, "[hang-dump]   VRAM written to %s\n", path);
                }
            }
            // URBAN_DUMP_MEM=addr:len[,addr:len...] (hex addr, decimal or hex len): hex dump of guest memory.
            if (const char *spec = std::getenv("URBAN_DUMP_MEM"); spec && *spec)
            {
                std::string_view list(spec);
                const uint8_t *rdram = runtime->memory().getRDRAM();
                while (!list.empty())
                {
                    const size_t comma = list.find(',');
                    const std::string item(list.substr(0, comma));
                    list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
                    const size_t colon = item.find(':');
                    const uint32_t address = static_cast<uint32_t>(std::stoul(item.substr(0, colon), nullptr, 16));
                    const uint32_t length = colon == std::string::npos ? 16u : static_cast<uint32_t>(std::stoul(item.substr(colon + 1), nullptr, 0));
                    for (uint32_t offset = 0; offset < length; offset += 16)
                    {
                        std::fprintf(stderr, "[hang-dump]   mem %08x:", address + offset);
                        for (uint32_t i = 0; i < 16 && offset + i < length; ++i)
                            std::fprintf(stderr, " %02x", rdram[(address + offset + i) & 0x01FFFFFFu]);
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            // Live PC sampling: generated code stores ctx->pc before every instruction. Racy by design (diagnostic only).
            std::map<uint32_t, int> samples;
            for (int i = 0; i < 2000; ++i)
            {
                if (const R5900Context *ctx = runtime->eeScheduler().currentContext())
                    ++samples[ctx->pc];
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            std::vector<std::pair<int, uint32_t>> hot;
            for (const auto &[pc, count] : samples)
                hot.emplace_back(count, pc);
            std::sort(hot.begin(), hot.end(), std::greater<>());
            for (size_t i = 0; i < hot.size() && i < 12; ++i)
                std::fprintf(stderr, "[hang-dump]   pc-sample %4d x %s\n", hot[i].first, guestLabel(hot[i].second).c_str());
            for (const EeSemaphoreSnapshot &s : snapshot.semaphores)
                std::fprintf(stderr, "[hang-dump]   sema %2d count=%d/%d waiters=%u\n", s.id, s.count, s.maxCount, s.waiters);
            for (const EeEventFlagSnapshot &e : snapshot.eventFlags)
                std::fprintf(stderr, "[hang-dump]   evflag %2d bits=0x%08x waiters=%u\n", e.id, e.bits, e.waiters);
            std::fflush(stderr);
        }
    }

    void setRuntime(PS2Runtime *runtime)
    {
        g_runtime.store(runtime);
    }

#if defined(_WIN32)
    namespace
    {
        std::string symbolize(HANDLE process, DWORD64 pc)
        {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            char text[512];
            if (SymFromAddr(process, pc, &displacement, symbol))
            {
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(process, pc, &lineDisplacement, &line))
                    std::snprintf(text, sizeof(text), "%s+0x%llx (%s:%lu)", symbol->Name, static_cast<unsigned long long>(displacement),
                                  line.FileName, line.LineNumber);
                else
                    std::snprintf(text, sizeof(text), "%s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
            }
            else
            {
                std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(pc));
            }
            return text;
        }

        std::string threadName(HANDLE thread)
        {
            PWSTR description = nullptr;
            std::string name;
            if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
            {
                for (const wchar_t *c = description; *c; ++c)
                    name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
                LocalFree(description);
            }
            return name;
        }

        void dumpAllThreads()
        {
            HANDLE process = GetCurrentProcess();
            static bool symbolsReady = false;
            if (!symbolsReady)
            {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
                symbolsReady = SymInitialize(process, nullptr, TRUE) == TRUE;
            }

            const DWORD self = GetCurrentThreadId();
            const DWORD pid = GetCurrentProcessId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            std::fprintf(stderr, "[hang-dump] ===== native thread stacks =====\n");
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
                    continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                           entry.th32ThreadID);
                if (!thread)
                    continue;
                std::vector<DWORD64> pcs;
                if (SuspendThread(thread) != static_cast<DWORD>(-1))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread, &context))
                    {
                        STACKFRAME64 frame{};
                        frame.AddrPC.Offset = context.Rip;
                        frame.AddrPC.Mode = AddrModeFlat;
                        frame.AddrFrame.Offset = context.Rbp;
                        frame.AddrFrame.Mode = AddrModeFlat;
                        frame.AddrStack.Offset = context.Rsp;
                        frame.AddrStack.Mode = AddrModeFlat;
                        for (int i = 0; i < 40; ++i)
                        {
                            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                                frame.AddrPC.Offset == 0)
                                break;
                            pcs.push_back(frame.AddrPC.Offset);
                        }
                    }
                    ResumeThread(thread);
                }
                std::fprintf(stderr, "[hang-dump] thread %lu %s\n", entry.th32ThreadID, threadName(thread).c_str());
                for (DWORD64 pc : pcs)
                    std::fprintf(stderr, "[hang-dump]     %s\n", symbolize(process, pc).c_str());
                CloseHandle(thread);
            }
            CloseHandle(snapshot);
            std::fflush(stderr);
        }
    }

#endif

    void startFromEnvironment()
    {
        const char *delay = std::getenv("URBAN_HANG_DUMP");
        if (!delay || !*delay)
            return;
        const int delaySeconds = std::atoi(delay);
        const char *repeat = std::getenv("URBAN_HANG_DUMP_REPEAT");
        const int repeatSeconds = repeat ? std::atoi(repeat) : 0;
        const char *native = std::getenv("URBAN_HANG_DUMP_NATIVE");
        const bool nativeStacks = !native || *native != '0';
        std::thread([delaySeconds, repeatSeconds, nativeStacks]() {
            std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
            do
            {
#if defined(_WIN32)
                if (nativeStacks)
                    dumpAllThreads();
#else
                (void)nativeStacks; // native stacks are Windows only
#endif
                dumpGuestKernel();
                static const int top = [] { const char *v = std::getenv("URBAN_CALL_PROFILE_TOP"); return v ? std::atoi(v) : 30; }();
                callprofile::report(top);
                if (repeatSeconds > 0)
                    std::this_thread::sleep_for(std::chrono::seconds(repeatSeconds));
            } while (repeatSeconds > 0);
        }).detach();
    }
}
