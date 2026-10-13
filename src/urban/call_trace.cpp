#include "urban/call_trace.h"
#include "urban/function_hooks.h"
#include "urban/log.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace urban::calltrace
{
    namespace
    {
        struct Watch
        {
            uint32_t address = 0;
            uint32_t offset = 0;
            bool deref = false;
            std::string label;
        };

        std::vector<Watch> g_watches;

        std::vector<std::string> splitList(const char *value)
        {
            std::vector<std::string> items;
            std::string_view list = value ? value : "";
            while (!list.empty())
            {
                const size_t comma = list.find(',');
                if (const std::string_view token = list.substr(0, comma); !token.empty())
                    items.emplace_back(token);
                list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
            }
            return items;
        }

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            return rdram[address & 0x01FFFFFFu];
        }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
                value |= static_cast<uint32_t>(readByte(rdram, address + i)) << (8 * i);
            return value;
        }
    }

#if defined(_WIN32)
    namespace
    {
        PS2Runtime *g_hwRuntime = nullptr;
        uint32_t g_hwGuestAddress = 0;
        uintptr_t g_hwHostAddress = 0;
        uint32_t g_hwLength = 1;
        std::atomic<int> g_hwHits{0};

        LONG CALLBACK hwWatchHandler(EXCEPTION_POINTERS *info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (info->ContextRecord->Dr6 & 1u) == 0u)
                return EXCEPTION_CONTINUE_SEARCH;
            info->ContextRecord->Dr6 = 0;
            if (g_hwHits.fetch_add(1) < 20000)
            {
                const R5900Context *ctx = g_hwRuntime->eeScheduler().currentContext();
                // With reads watched too, reading the address here would fire the watchpoint again inside the handler.
                uint64_t value = 0;
                if (!std::getenv("URBAN_HW_WATCH_RW"))
                    std::memcpy(&value, reinterpret_cast<const void *>(g_hwHostAddress), g_hwLength);
                char line[256];
                std::snprintf(line, sizeof(line), "hw-watch %s 0x%08x = 0x%llx guest pc=0x%08x ra=0x%08x host rip=0x%llx",
                              std::getenv("URBAN_HW_WATCH_RW") ? "access" : "write", g_hwGuestAddress,
                              static_cast<unsigned long long>(value), ctx ? ctx->pc : 0u, ctx ? getRegU32(ctx, 31) : 0u,
                              static_cast<unsigned long long>(info->ContextRecord->Rip));
                URBAN_INFO(Game, line);
                if (std::getenv("URBAN_HW_WATCH_STACK"))
                {
                    // Native stack of the writer: tells guest code apart from runtime-side writes (DMA, IOP, HLE).
                    static bool symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) == TRUE;
                    (void)symbols;
                    CONTEXT context = *info->ContextRecord;
                    STACKFRAME64 frame{};
                    frame.AddrPC.Offset = context.Rip;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = context.Rbp;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = context.Rsp;
                    frame.AddrStack.Mode = AddrModeFlat;
                    for (int i = 0; i < 14 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &context,
                                                          nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
                         ++i)
                    {
                        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
                        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
                        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                        symbol->MaxNameLen = 255;
                        DWORD64 displacement = 0;
                        const bool known = SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &displacement, symbol) != FALSE;
                        URBAN_INFO(Game, "    #" << i << ' ' << (known ? symbol->Name : "?") << "+0x" << std::hex << displacement);
                    }
                }
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        bool armOnThread(DWORD threadId)
        {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, threadId);
            if (!thread)
                return false;
            SuspendThread(thread);
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            GetThreadContext(thread, &context);
            const uint64_t lenBits = g_hwLength == 8 ? 2u : g_hwLength == 4 ? 3u : g_hwLength == 2 ? 1u : 0u;
            // URBAN_HW_WATCH_RW=1: reads too (R/W0 = 3); the default is writes only (R/W0 = 1).
            const uint64_t rwBits = std::getenv("URBAN_HW_WATCH_RW") ? 3u : 1u;
            context.Dr0 = g_hwHostAddress;
            context.Dr7 = (context.Dr7 & ~0xF0003ull) | 1ull /* L0 */ | (rwBits << 16) | (lenBits << 18);
            const bool ok = SetThreadContext(thread, &context) != FALSE;
            ResumeThread(thread);
            CloseHandle(thread);
            return ok;
        }

        void armWhenGameThreadExists()
        {
            for (int attempt = 0; attempt < 2000; ++attempt)
            {
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId())
                        continue;
                    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                    PWSTR description = nullptr;
                    const bool isGame = thread && SUCCEEDED(GetThreadDescription(thread, &description)) && description &&
                                        std::wstring_view(description) == L"GameThread";
                    if (description)
                        LocalFree(description);
                    if (thread)
                        CloseHandle(thread);
                    if (isGame)
                    {
                        CloseHandle(snapshot);
                        URBAN_INFO(Game, "hw-watch armed on GameThread " << entry.th32ThreadID << ": " << armOnThread(entry.th32ThreadID));
                        return;
                    }
                }
                CloseHandle(snapshot);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            URBAN_ERROR(Game, "hw-watch: GameThread not found");
        }
    }
#endif

    void installFromEnvironment(PS2Runtime &runtime)
    {
#if defined(_WIN32)
        if (const char *hw = std::getenv("URBAN_HW_WATCH"); hw && *hw)
        {
            const std::string item(hw);
            const size_t comma = item.find(',');
            g_hwRuntime = &runtime;
            g_hwGuestAddress = static_cast<uint32_t>(std::stoul(item.substr(0, comma), nullptr, 16));
            g_hwLength = comma == std::string::npos ? 1u : static_cast<uint32_t>(std::stoul(item.substr(comma + 1)));
            g_hwHostAddress = reinterpret_cast<uintptr_t>(runtime.memory().getRDRAM() + (g_hwGuestAddress & 0x01FFFFFFu));
            AddVectoredExceptionHandler(1, &hwWatchHandler);
            std::thread(&armWhenGameThreadExists).detach();
        }
#endif
        // Each item: "ptr:offset" (byte at *(u32*)ptr + offset) or "=addr" (the u32 at addr).
        std::vector<std::string> polls = splitList(std::getenv("URBAN_WATCH_POLL"));
        if (!polls.empty())
        {
            PS2Runtime *rt = &runtime;
            std::thread([rt, polls]() {
                const uint8_t *rdram = rt->memory().getRDRAM();
                std::vector<int64_t> last(polls.size(), -1);
                for (;;)
                {
                    for (size_t i = 0; i < polls.size(); ++i)
                    {
                        const std::string &item = polls[i];
                        int64_t value;
                        uint32_t target;
                        if (item[0] == '=')
                        {
                            target = static_cast<uint32_t>(std::stoul(item.substr(1), nullptr, 16));
                            value = readWord(rdram, target);
                        }
                        else
                        {
                            const size_t colon = item.find(':');
                            const uint32_t pointer = static_cast<uint32_t>(std::stoul(item.substr(0, colon), nullptr, 16));
                            const uint32_t offset =
                                colon == std::string::npos ? 0u : static_cast<uint32_t>(std::stoul(item.substr(colon + 1), nullptr, 0));
                            const uint32_t base = readWord(rdram, pointer);
                            target = base ? base + offset : 0u;
                            value = target ? readByte(rdram, target) : -1;
                        }
                        if (value != last[i])
                        {
                            const R5900Context *ctx = rt->eeScheduler().currentContext();
                            std::ostringstream line;
                            line << std::hex << "watch [" << item << "] @0x" << target << " 0x" << last[i] << " -> 0x" << value
                                 << " guest pc=0x" << (ctx ? ctx->pc : 0u) << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u);
                            URBAN_INFO(Game, line.str());
                            last[i] = value;
                        }
                    }
                }
            }).detach();
        }
        for (const std::string &item : splitList(std::getenv("URBAN_WATCH")))
        {
            Watch watch;
            watch.label = item;
            if (item[0] == '=')
            {
                watch.address = static_cast<uint32_t>(std::stoul(item.substr(1), nullptr, 16));
            }
            else
            {
                const size_t colon = item.find(':');
                watch.address = static_cast<uint32_t>(std::stoul(item.substr(0, colon), nullptr, 16));
                watch.offset = colon == std::string::npos ? 0u : static_cast<uint32_t>(std::stoul(item.substr(colon + 1), nullptr, 0));
                watch.deref = true;
            }
            g_watches.push_back(watch);
        }

        // URBAN_PSM_AUDIT=1: CRI ADX PS2 sound-manager chunk accounting (story "Now Loading" hang, PROGRESS §34).
        // On every IOP response (callback 0x2fa200, a1 = response list) it logs entries the IOP flagged (not returned to
        // the free lane) and, when it changes, the 0x4000-byte total of PSM channel 0/1 (free + data + listed + in DMA).
        if (const char *audit = std::getenv("URBAN_PSM_AUDIT"); audit && *audit == '1')
        {
            FunctionHooks::instance().observeEntry(0x2fa200u, "psm_audit", [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *) {
                static uint32_t lastTotals[2] = {0x4000u, 0x4000u};
                const uint32_t list = getRegU32(ctx, 5);
                const uint32_t count = readWord(rdram, list);
                uint32_t listed[2] = {};
                for (uint32_t e = 0; e < count && e < 0x80u; ++e)
                {
                    const uint32_t entry = list + 0x10u + e * 0x10u;
                    const uint32_t object = readWord(rdram, entry + 4u);
                    const uint32_t size = readWord(rdram, entry + 12u);
                    if (readByte(rdram, entry) != 0u)
                    {
                        std::ostringstream line;
                        line << std::hex << "psm-audit flagged entry " << e << "/" << count << " flag=" << unsigned(readByte(rdram, entry))
                             << " lane=" << unsigned(readByte(rdram, entry + 1u)) << " obj=0x" << object << " chunk=0x"
                             << readWord(rdram, entry + 8u) << "+0x" << size;
                        URBAN_INFO(Game, line.str());
                    }
                    for (uint32_t ch = 0; ch < 2u; ++ch)
                        if (object == 0x69efe8u + ch * 0x14u)
                            listed[ch] += size;
                }
                static const bool logChunks = std::getenv("URBAN_PSM_AUDIT_CHUNKS") != nullptr;
                if (logChunks)
                {
                    std::ostringstream line;
                    line << std::hex << "psm-chunks";
                    for (uint32_t e = 0; e < count && e < 0x80u; ++e)
                    {
                        const uint32_t entry = list + 0x10u + e * 0x10u;
                        const uint32_t object = readWord(rdram, entry + 4u);
                        line << " " << (object == 0x69efe8u ? "c0" : object == 0x69effcu ? "c1" : "x") << ":" << readWord(rdram, entry + 8u)
                             << "+" << readWord(rdram, entry + 12u) << "/f" << unsigned(readByte(rdram, entry)) << "l"
                             << unsigned(readByte(rdram, entry + 1u));
                    }
                    URBAN_INFO(Game, line.str());
                }
                for (uint32_t ch = 0; ch < 2u; ++ch)
                {
                    const uint32_t sj = readWord(rdram, 0x696968u + ch * 0x18u + 8u);
                    uint32_t lanes = 0u;
                    for (uint32_t lane = 0; lane < 2u; ++lane)
                        for (uint32_t node = readWord(rdram, sj + 0x18u + lane * 4u), guard = 0; node && guard < 64u;
                             node = readWord(rdram, node), ++guard)
                            lanes += readWord(rdram, node + 12u);
                    const uint32_t transfer = readWord(rdram, 0x698d68u + 0x10u + ch * 4u);
                    const uint32_t inDma = readByte(rdram, transfer + 2u) == 1u ? readWord(rdram, transfer + 0x18u) : 0u;
                    const uint32_t total = lanes + listed[ch] + inDma;
                    if (total != lastTotals[ch])
                    {
                        std::ostringstream line;
                        line << std::hex << "psm-audit ch" << ch << " total 0x" << lastTotals[ch] << " -> 0x" << total << " (lanes 0x" << lanes
                             << " listed 0x" << listed[ch] << " dma 0x" << inDma << ") entries=" << count;
                        URBAN_INFO(Game, line.str());
                        lastTotals[ch] = total;
                    }
                }
            });
        }

        for (const std::string &item : splitList(std::getenv("URBAN_CALL_TRACE")))
        {
            const uint32_t address = static_cast<uint32_t>(std::stoul(item, nullptr, 16));
            std::ostringstream name;
            name << "trace_0x" << std::hex << address;
            FunctionHooks::instance().observeEntry(address, name.str(), [address](uint8_t *rdram, R5900Context *ctx, PS2Runtime *) {
                std::ostringstream line;
                line << std::hex << "call 0x" << address << " a0=0x" << getRegU32(ctx, 4) << " a1=0x" << getRegU32(ctx, 5)
                     << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7) << " ra=0x" << getRegU32(ctx, 31)
                     << " s0-s3=0x" << getRegU32(ctx, 16) << ",0x" << getRegU32(ctx, 17) << ",0x" << getRegU32(ctx, 18) << ",0x"
                     << getRegU32(ctx, 19) << " sp=0x" << getRegU32(ctx, 29);
                for (const Watch &watch : g_watches)
                {
                    const uint32_t target = watch.deref ? readWord(rdram, watch.address) + watch.offset : watch.address;
                    line << " [" << watch.label << "]=0x" << static_cast<unsigned>(readByte(rdram, target));
                }
                URBAN_INFO(Game, line.str());
            });
        }
    }
}
