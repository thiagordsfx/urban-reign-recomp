#include "runtime/ps2_async_vif.h"
#include "ps2_runtime.h"
#include "runtime/ps2_save_state.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_display_options.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/gs/gs_shared_present.h"
#include "runtime/ee_scheduler.h"
#include "ThreadPriority.h"
#include "ThreadNaming.h"
#include "GuestKernelMemory.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2_host_input.h"
void ps2xSetLocalToHostGs(GS *gs); // ps2_memory.cpp: VIF1 FIFO image downloads
void ps2xRequestVu1Trace(uint32_t fields); // vu/ps2_vu1_core.cpp: F8 debug capture
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <rlgl.h>

namespace ps2_stubs
{
    void resetSifState();
}

namespace ps2_host_display
{
    std::atomic<float> displayAspect{4.0f / 3.0f};
    std::atomic<float> windowAspect{4.0f / 3.0f};
    std::atomic<bool> fitWindow{false};
    bool startMaximized = false;
    bool startFullscreen = false;

    void setAspect(float aspect)
    {
        displayAspect.store(aspect, std::memory_order_relaxed);
    }

    float aspect()
    {
        return fitWindow.load(std::memory_order_relaxed)
                   ? windowAspect.load(std::memory_order_relaxed)
                   : displayAspect.load(std::memory_order_relaxed);
    }

    void setFitWindow(bool enabled)
    {
        fitWindow.store(enabled, std::memory_order_relaxed);
    }

    void updateWindowAspect()
    {
        const int width = GetScreenWidth();
        const int height = GetScreenHeight();
        if (width > 0 && height > 0)
            windowAspect.store(static_cast<float>(width) / static_cast<float>(height), std::memory_order_relaxed);
    }

    void setStartMaximized(bool enabled)
    {
        startMaximized = enabled;
    }

    void setStartFullscreen(bool enabled)
    {
        startFullscreen = enabled;
    }

    bool vsyncEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_VSYNC"); return !v || *v != '0'; }();
        return enabled;
    }

    // PS2X_PRESENT_SYNC (on by default, =0 disables): present once per emulated field. The present loop waits (up to
    // 50 ms, so the window stays responsive while the game loads) for the vsync tick to move before uploading the
    // frame; with real vsync each emulated field then becomes exactly one monitor refresh. Without it the 59.94 Hz
    // emulation and the 60 Hz loop sat near the same phase and ~1 ms of jitter repeated one field and skipped the
    // next, so a steady 30 FPS game looked like ~20-24.
    bool presentSyncEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_PRESENT_SYNC"); return !v || *v != '0'; }();
        return enabled;
    }

    std::mutex overlayMutex;
    std::string overlayNote;
    std::string lastFpsLine;
    unsigned lastFpsSerial = 0u;
    thread_local std::string t_fpsLineCopy;

    void setOverlayNote(const char *text)
    {
        std::lock_guard<std::mutex> lock(overlayMutex);
        overlayNote = text ? text : "";
    }

    const char *fpsLine(unsigned &serial)
    {
        std::lock_guard<std::mutex> lock(overlayMutex);
        serial = lastFpsSerial;
        t_fpsLineCopy = lastFpsLine;
        return t_fpsLineCopy.c_str();
    }

    std::atomic<uint64_t> gameFrames{0u};

    std::mutex presentMutex;
    std::vector<PresentRecord> presentRing(65536u);
    uint64_t presentCount = 0u;

    void notePresent(uint64_t field, uint64_t imageSequence)
    {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(presentMutex);
        presentRing[presentCount % presentRing.size()] = PresentRecord{now, field, imageSequence, gameFrames.load(std::memory_order_relaxed)};
        ++presentCount;
    }

    void copyPresents(double sinceHostSeconds, std::vector<PresentRecord> &out)
    {
        std::lock_guard<std::mutex> lock(presentMutex);
        const uint64_t first = presentCount > presentRing.size() ? presentCount - presentRing.size() : 0u;
        for (uint64_t i = first; i < presentCount; ++i)
        {
            const PresentRecord &r = presentRing[i % presentRing.size()];
            if (r.hostSeconds >= sinceHostSeconds)
                out.push_back(r);
        }
    }
    std::atomic<int64_t> worstGameFrameUs{0}; // longest game-frame interval since the overlay last read it

    void noteGameFrame()
    {
        gameFrames.fetch_add(1u, std::memory_order_relaxed);
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0)
        {
            const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
            int64_t worst = worstGameFrameUs.load(std::memory_order_relaxed);
            while (us > worst && !worstGameFrameUs.compare_exchange_weak(worst, us, std::memory_order_relaxed))
            {
            }
        }
        last = now;
    }

    // FPS overlay, toggled with F3 (PS2X_SHOW_FPS=1 starts with it on). Once per second:
    // - Game: game frames (main-loop iterations reported by the port);
    // - Screen: host presents (PCSX2's GS: its presents on the GS thread);
    // - Speed: emulated vsync fields per second vs 59.94 (NTSC);
    // - worst frame: the longest game-frame interval of the last second (hitches the averages hide).
    struct FpsOverlay
    {
        bool visible = [] { const char *v = std::getenv("PS2X_SHOW_FPS"); return v && *v == '1'; }();
        std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastPresent = windowStart;
        uint64_t startFrames = 0u;
        uint64_t startFields = 0u;
        uint32_t presents = 0u;
        uint64_t startGsPresents = 0u;
        double worstPresentMs = 0.0;
        std::string text = "FPS ...";

        void present(uint64_t field)
        {
            const auto now = std::chrono::steady_clock::now();
            lastPresent = now;
            ++presents;
            const double seconds = std::chrono::duration<double>(now - windowStart).count();
            if (seconds < 1.0)
                return;
            const uint64_t frames = gameFrames.load(std::memory_order_relaxed);
            worstPresentMs = static_cast<double>(worstGameFrameUs.exchange(0, std::memory_order_relaxed)) / 1000.0;
            const double fields = static_cast<double>(field - startFields) / seconds;
            // With PCSX2's GS the screen is its presents on the GS thread, not this loop's.
            if (GSPcsx2Backend::Active())
            {
                const uint64_t gsPresents = GSPcsx2Backend::Presents();
                presents = static_cast<uint32_t>(gsPresents - startGsPresents);
                startGsPresents = gsPresents;
            }
            char line[160];
            std::snprintf(line, sizeof(line), "Game %.0f FPS | Screen %.0f | Speed %.0f%% | worst frame %.0f ms",
                          static_cast<double>(frames - startFrames) / seconds, presents / seconds, 100.0 * fields / 59.94,
                          worstPresentMs);
            text = line;
            {
                std::lock_guard<std::mutex> lock(ps2_host_display::overlayMutex);
                lastFpsLine = line;
                ++lastFpsSerial;
            }
            windowStart = now;
            startFrames = frames;
            startFields = field;
            presents = 0u;
            worstPresentMs = 0.0;
        }

        void draw() const
        {
            const int fontSize = std::max(16, GetScreenHeight() / 36);
            std::string note;
            {
                std::lock_guard<std::mutex> lock(ps2_host_display::overlayMutex);
                note = overlayNote;
            }
            if (!note.empty())
            {
                const int noteWidth = MeasureText(note.c_str(), fontSize);
                DrawRectangle(8, 8, noteWidth + 16, fontSize + 12, Fade(MAROON, 0.8f));
                DrawText(note.c_str(), 16, 14, fontSize, RAYWHITE);
            }
            if (!visible)
                return;
            const int width = MeasureText(text.c_str(), fontSize);
            const int y = GetScreenHeight() - fontSize - 20;
            DrawRectangle(8, y - 6, width + 16, fontSize + 12, Fade(BLACK, 0.6f));
            DrawText(text.c_str(), 16, y, fontSize, RAYWHITE);
        }
    };
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = 640;
static constexpr int HOST_WINDOW_HEIGHT = 480;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;
    thread_local uint64_t g_guestJumpSerial = 0u;

    struct BranchEvent
    {
        uint32_t kind;
        uint32_t source;
        uint32_t target;
        uint32_t sp;
        uint32_t ra;
        uint32_t extra;
    };

    struct BranchTrace
    {
        std::vector<BranchEvent> events;
        size_t next = 0u;
        bool wrapped = false;
    };

    const size_t g_branchTraceSize = []() -> size_t
    {
        const char *value = std::getenv("PS2X_BRANCH_TRACE");
        return value ? static_cast<size_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }();

    BranchTrace *branchTrace()
    {
        const size_t size = g_branchTraceSize;
        if (size == 0u)
        {
            return nullptr;
        }
        thread_local BranchTrace trace;
        if (trace.events.empty())
        {
            trace.events.resize(size);
        }
        return &trace;
    }

    void pushBranchEvent(uint32_t kind, uint32_t source, uint32_t target, R5900Context *ctx, uint32_t extra)
    {
        BranchTrace *trace = branchTrace();
        if (!trace)
        {
            return;
        }
        trace->events[trace->next] = {kind, source, target, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)),
                                      static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), extra};
        trace->next = (trace->next + 1u) % trace->events.size();
        if (trace->next == 0u)
        {
            trace->wrapped = true;
        }
    }

    void dumpBranchTrace()
    {
        BranchTrace *trace = branchTrace();
        if (!trace)
        {
            return;
        }
        const char *path = std::getenv("PS2X_BRANCH_TRACE_FILE");
        FILE *file = path ? std::fopen(path, "w") : nullptr;
        FILE *out = file ? file : stderr;
        const size_t count = trace->wrapped ? trace->events.size() : trace->next;
        static const char *const kKinds[] = {"call", "ret", "jump", "sched"};
        for (size_t i = 0; i < count; ++i)
        {
            const BranchEvent &e = trace->events[(trace->next + trace->events.size() - count + i) % trace->events.size()];
            std::fprintf(out, "%s src=%08x dst=%08x sp=%08x ra=%08x x=%08x\n", kKinds[e.kind & 3u], e.source, e.target, e.sp, e.ra, e.extra);
        }
        if (file)
        {
            std::fclose(file);
        }
    }

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

void ps2xHostVsyncSetWindow(void *window) noexcept; // Kernel/host_vsync.cpp
void ps2xEeFlushDeferredCycles() noexcept; // Kernel/EeScheduler.cpp

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight,
                        bool hashImages, GSSharedPresent::Frame &diagnostic)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static std::vector<uint8_t> s_scratch;
    static uint64_t s_lastHash = 0u;
    diagnostic.contentHash = s_lastHash;
    diagnostic.hashValid = hashImages && s_lastWidth != 0u && s_lastHeight != 0u;

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    if (currentTick == s_lastPresentationTick)
    {
        outWidth = s_lastWidth;
        outHeight = s_lastHeight;
        return;
    }
    s_lastPresentationTick = currentTick;
    rt->gs().latchHostPresentationFrame();
    if (GSSharedPresent::Active())
    {
        outWidth = outHeight = 0u;
        return;
    }

    uint32_t width = 0u;
    uint32_t height = 0u;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch, width, height) || width == 0u || height == 0u)
    {
        diagnostic.hashValid = false;
        outWidth = s_lastWidth = 0u;
        outHeight = s_lastHeight = 0u;
        return;
    }

    if (tex.id == 0 || static_cast<uint32_t>(tex.width) != width || static_cast<uint32_t>(tex.height) != height)
    {
        if (tex.id != 0)
            UnloadTexture(tex);
        Image image = GenImageColor(static_cast<int>(width), static_cast<int>(height), BLACK);
        tex = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    }
    UpdateTexture(tex, s_scratch.data());
    if (hashImages)
    {
        const auto mix = [](uint32_t value)
        {
            value ^= value >> 16u;
            value *= 0x7feb352du;
            value ^= value >> 15u;
            value *= 0x846ca68bu;
            return value ^ (value >> 16u);
        };
        uint32_t sum = 0u;
        uint32_t hashXor = 0u;
        for (uint32_t i = 0u; i < width * height; ++i)
        {
            uint32_t pixel;
            std::memcpy(&pixel, s_scratch.data() + static_cast<size_t>(i) * 4u, sizeof(pixel));
            const uint32_t value = mix(pixel ^ mix(i + 1u));
            sum += value;
            hashXor ^= mix(value ^ 0x9e3779b9u);
        }
        diagnostic.contentHash = s_lastHash = static_cast<uint64_t>(sum) | (static_cast<uint64_t>(hashXor) << 32u);
        diagnostic.width = width;
        diagnostic.height = height;
        diagnostic.hashValid = true;
    }
    outWidth = s_lastWidth = width;
    outHeight = s_lastHeight = height;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = ps2x::kernel_memory::kCallbackStackFloor;
    m_asyncCallbackStackTop = ps2x::kernel_memory::kCallbackStackTop;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            m_audioBackend.stopAll();
            m_audioBackend.setAudioReady(false);
            CloseAudioDevice();
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    // SifCallRpc gives each call its own token and files a pending NOWAIT call under it; the IOP reports the end with
    // the token it was given. Overwriting it with the scope's lost every asynchronous completion (the client stayed
    // busy forever; PROGRESS §66).
    if (request.callToken == 0u)
        request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    m_iopSubsystem->runEeCycles(eeCycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    ps2xSetLocalToHostGs(&m_gs);
    ps2x::asyncvif::setGifProcessor([this](const uint8_t *data, uint32_t size)
                                     { m_gs.processGIFPacket(data, size); });
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    {
                                        if (!ps2x::asyncvif::submitGif(data, size))
                                            m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    ps2x::asyncvif::setFbrstProvider([this]() -> uint32_t
                                     {
                                         R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         return (context ? context : &m_cpuContext)->vu0_fbrst; });
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     const bool onVuThread = ps2x::asyncvif::onWorker();
                                     R5900Context *cpuContext = nullptr;
                                     if (!onVuThread)
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                             cpuContext = &m_cpuContext;
                                     }
                                     const uint32_t fbrst = onVuThread ? ps2x::asyncvif::currentFbrst() : cpuContext->vu0_fbrst;
                                     m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     if (cpuContext)
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    ps2x::asyncvif::setVu1Timing({[this]() -> uint64_t
                                  { return m_eeScheduler ? m_eeScheduler->currentCycle() : 0u; },
                                  [this]() -> uint64_t
                                  { return m_vu1.state().cycles; },
                                  [this](uint64_t cycles) -> uint64_t
                                  { return m_eeScheduler ? m_eeScheduler->consumeCycles(cycles) : 0u; }});
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     const bool onVuThread = ps2x::asyncvif::onWorker();
                                     R5900Context *cpuContext = nullptr;
                                     if (!onVuThread)
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                             cpuContext = &m_cpuContext;
                                     }
                                     const uint32_t fbrst = onVuThread ? ps2x::asyncvif::currentFbrst() : cpuContext->vu0_fbrst;
                                     m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     if (cpuContext)
                                         cpuContext->vu0_vpu_stat =
                                             (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                             (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                             (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        // Real vsync (PS2X_VSYNC=0 goes back to the software 60 FPS limiter): the limiter was not aligned with the
        // monitor's refresh, so presents landed at a drifting phase and frames were shown for 1 or 3 refreshes.
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN | (ps2_host_display::vsyncEnabled() ? FLAG_VSYNC_HINT : 0u));
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        if (ps2_host_display::startMaximized)
            MaximizeWindow();
        if (ps2_host_display::startFullscreen)
            ToggleBorderlessWindowed();
        SetExitKey(KEY_NULL);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        GSSharedPresent::CaptureHostContext();
        ps2xHostVsyncSetWindow(GetWindowHandle()); // the vblank lock follows the window's monitor (Kernel/host_vsync.cpp)
        GSPcsx2Backend::AttachHostWindow(GetWindowHandle()); // PCSX2's GS: its child window over the game area
#endif
        const bool coreReady = syncCoreSubsystems();
        GSSharedPresent::RestoreHostContext();
        if (!coreReady)
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
        SetTargetFPS(ps2_host_display::vsyncEnabled() ? 0 : 60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        m_asyncCallbackStackFloor = ps2x::kernel_memory::kCallbackStackFloor;
        m_asyncCallbackStackTop = ps2x::kernel_memory::kCallbackStackTop;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

void ps2xTraceSchedulerDispatch(R5900Context *ctx)
{
    pushBranchEvent(3u, 0u, ctx->pc, ctx, 0u);
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
        dumpBranchTrace();
        if (const char *dumpPath = std::getenv("PS2X_DUMP_RAM_ON_MISSING"); dumpPath && *dumpPath && rdram)
        {
            if (FILE *file = std::fopen(dumpPath, "wb"))
            {
                std::fwrite(rdram, 1, PS2_RAM_SIZE, file);
                std::fclose(file);
            }
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        return false;
    }

    if (!isCall)
    {
        pushBranchEvent(2u, sourcePc, targetPc, ctx, 0u);
        ++g_guestJumpSerial;
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    static int s_singleLookup = -1; // plain global, not a guarded static (checked on every call)
    if (s_singleLookup < 0)
    {
        const char *setting = std::getenv("PS2X_EE_SINGLE_LOOKUP");
        s_singleLookup = (!setting || std::string_view(setting) != "0") ? 1 : 0;
    }
    const bool singleLookup = s_singleLookup != 0;
    RecompiledFunction targetFn = nullptr;
    if (singleLookup)
    {
        uint32_t slot = 0u;
        if (generatedFunctionTableSlot(targetPc, slot))
            targetFn = g_ps2RecompiledFunctionTable[slot];
    }
    else if (hasFunction(targetPc))
        targetFn = lookupFunction(targetPc);
    if (!targetFn)
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    if (singleLookup)
        pushDispatchPc(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint64_t jumpSerial = g_guestJumpSerial;
    pushBranchEvent(0u, sourcePc, targetPc, ctx, fallthroughPc);
    targetFn(rdram, ctx, this);
    pushBranchEvent(1u, targetPc, ctx->pc, ctx, fallthroughPc);

    if (m_eeScheduler->transferPending() || m_eeScheduler->unwinding() || isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (ctx->pc == entryPc && jumpSerial == g_guestJumpSerial)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void ps2xVuResetStateForExecute(VU1Interpreter &vu); // vu/ps2_vu1_core.cpp
void ps2xVu0LoadRegisters(VU1Interpreter &vu, const __m128 *vf, __m128 acc);
void ps2xVu0ExecuteLean(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize, GS &gs,
                        PS2Memory *memory, uint32_t startPC, uint32_t itop, uint32_t maxCycles);

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    // Settings, read on the first call. Plain statics (only the EE thread gets here): a function-local static const
    // costs a thread-safe initialization check on each of the ~4500 calls per frame.
    // PS2X_VU0_LOG=<n>: for the first n calls of each microprogram address, log its first code words and
    // VF1-VF8 before/after (diagnosing VU0 micro mode, e.g. Urban Reign's skinning).
    // PS2X_VU0_LEAN=0: the previous per-call preparation (see below).
    // PS2X_VU0_BACKING=0: run with the 4 KB sizes (see below).
    static bool settingsRead = false;
    static uint32_t vu0LogLimit = 0u;
    static bool leanCall = true;
    static uint32_t vu0RunSize = 0x4000u;
    if (!settingsRead)
    {
        const char *log = std::getenv("PS2X_VU0_LOG");
        vu0LogLimit = log ? static_cast<uint32_t>(std::strtoul(log, nullptr, 10)) : 0u;
        const char *lean = std::getenv("PS2X_VU0_LEAN");
        leanCall = !(lean && *lean == '0');
        const char *backing = std::getenv("PS2X_VU0_BACKING");
        vu0RunSize = (backing && *backing == '0') ? PS2_VU0_CODE_SIZE : 0x4000u;
        settingsRead = true;
    }
    bool logThis = false;
    if (vu0LogLimit != 0u)
    {
        static std::unordered_map<uint32_t, uint32_t> vu0LogCounts;
        uint32_t &count = vu0LogCounts[startPC];
        logThis = count++ < vu0LogLimit;
    }
    auto dumpVf = [&](const char *when)
    {
        std::fprintf(stderr, "[vu0] %s pc=0x%03x", when, startPC);
        for (int r = 1; r <= 8; ++r)
        {
            alignas(16) float f[4];
            _mm_store_ps(f, ctx->vu0_vf[r]);
            std::fprintf(stderr, " vf%d=(%g,%g,%g,%g)", r, f[0], f[1], f[2], f[3]);
        }
        std::fprintf(stderr, "\n");
    };
    if (logThis)
    {
        uint32_t words[4];
        std::memcpy(words, vu0Code + startPC, sizeof(words));
        std::fprintf(stderr, "[vu0] call pc=0x%03x from=0x%x code=%08x %08x %08x %08x\n", startPC, ctx->pc,
                     words[0], words[1], words[2], words[3]);
        dumpVf("in ");
    }

    // PS2X_VU0_LEAN=0: the previous per-call preparation. Lean (default): no clearing of the whole VU0 state, only the
    // VF/ACC registers the EE changed are reloaded and renormalized, and the interpreter's scheduler arrays are cleared
    // only when the interpreter used them. Urban Reign makes thousands of VCALLMS per frame to a ~174-pair program, so
    // this preparation cost more than the program itself (docs/PROGRESS.md §62).
    if (leanCall)
    {
        ps2xVu0LoadRegisters(m_vu0, ctx->vu0_vf, ctx->vu0_acc);
        VU1State &state = m_vu0.state();
        for (uint32_t i = 1; i < 16u; ++i)
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        state.vi[0] = 0;
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        state.r = 0x3F800000u | (static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(ctx->vu0_r))) & 0x007FFFFFu);
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.cycles = 0u;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;
    }
    else
    {
        ps2xVuResetStateForExecute(m_vu0); // execute() below resets the scheduler; reset() did that a second time
        copyVu0ContextToState(ctx, m_vu0.state());
    }
    // The VU0 memories are backed by 16 KB (ps2_memory.cpp): run with the VU1 sizes so the ahead-of-time compiled
    // VU0 image (generated/vu1, found by hash) can be used. PS2X_VU0_BACKING=0 runs with the 4 KB sizes (interpreter).
    if (leanCall)
        ps2xVu0ExecuteLean(m_vu0, vu0Code, vu0RunSize, vu0Data, vu0RunSize, m_gs, &m_memory, startPC, ctx->vu0_itop, 4096);
    else
        m_vu0.execute(vu0Code, vu0RunSize,
                      vu0Data, vu0RunSize,
                      m_gs, &m_memory,
                      startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
    if (logThis)
        dumpVf("out");
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

bool PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    return handleSyscall(rdram, ctx, 0);
}

bool PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    ps2xEeFlushDeferredCycles(); // kernel calls see the exact EE cycle (EeScheduler::checkpointDue)
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    EeScheduler::DeferredTransferScope deferTransfers(*m_eeScheduler);
    if (!ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
    }
    return !m_eeScheduler->transferPending();
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    // No limit given: the conservative default end. An explicit limit (SetupHeap up to the main thread stack) may go
    // up to the end of RAM.
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u)
    {
        return hardLimit;
    }
    return std::min(guestLimit, PS2_RAM_SIZE);
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr & ~0xFu);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr & ~0xFu, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr & ~0xFu, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

// Budget of the deferred EE checkpoints (Kernel/EeScheduler.cpp). Checked here, before the call into the scheduler:
// it was tried inline in the generated code (in ps2_runtime.h), and the extra code in tens of thousands of loops made
// the game thread ~4 points slower overall (PROGRESS §76).
extern uint64_t g_ps2xEeDeferred;
extern std::atomic<uint64_t> g_ps2xEeDeferBudget;
extern uint32_t g_ps2xEeLoopCycles;

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    const uint64_t deferred = g_ps2xEeDeferred + (cycles == 32u ? g_ps2xEeLoopCycles : cycles);
    if (deferred < g_ps2xEeDeferBudget.load(std::memory_order_relaxed))
    {
        g_ps2xEeDeferred = deferred;
        return false;
    }
    // PS2X_EE_LOOP_CYCLES=<n>: EE cycles charged per backward branch of generated code (default 32). A plain global
    // read once: a function-local static costs a thread-local guard check on every backward branch.
    static uint32_t s_loopCycles = 0u;
    if (s_loopCycles == 0u)
    {
        const char *v = std::getenv("PS2X_EE_LOOP_CYCLES");
        s_loopCycles = v && *v ? static_cast<uint32_t>(std::max(1, std::atoi(v))) : 32u;
        g_ps2xEeLoopCycles = s_loopCycles; // the fast path above
    }
    if (cycles == 32u)
        cycles = s_loopCycles;
    return m_eeScheduler->checkpointDue(cycles);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSyncResume(currentTick + waitTicks - 1u, 0, resumePc);
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    const uint32_t presenceIndex = syscallNumber + 256u;
    if (presenceIndex < 512u)
    {
        if ((m_eeSyscallOverridePresence[presenceIndex >> 6u].load(std::memory_order_acquire) & (1ull << (presenceIndex & 63u))) == 0u)
        {
            return false;
        }
    }
    else if (m_eeSyscallOverridesOutOfRange.load(std::memory_order_acquire) == 0u)
    {
        return false;
    }
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    const uint32_t presenceIndex = syscallNumber + 256u;
    if (presenceIndex < 512u)
    {
        const uint64_t bit = 1ull << (presenceIndex & 63u);
        if (handler == 0u)
            m_eeSyscallOverridePresence[presenceIndex >> 6u].fetch_and(~bit, std::memory_order_acq_rel);
        else
            m_eeSyscallOverridePresence[presenceIndex >> 6u].fetch_or(bit, std::memory_order_acq_rel);
    }
    else
    {
        uint32_t outOfRange = 0u;
        for (const auto &[number, value] : m_eeSyscallOverrides)
        {
            (void)value;
            if (number + 256u >= 512u)
                ++outOfRange;
        }
        m_eeSyscallOverridesOutOfRange.store(outOfRange, std::memory_order_release);
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    Texture2D frameTex{};
    ps2_host_display::updateWindowAspect();

    std::atomic<bool> gameThreadFinished{false};
    ps2_host_input::setFieldCounter(&m_memory.gs().vsyncTick);
    ps2_host_input::sample();

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::Highest);
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::AboveNormal);
    bool restoreMaximized = false;
    uint64_t tick = 0;
    std::ofstream frameTimes;
    // With PCSX2's GS the presents happen on the GS thread: GSPcsx2Backend writes this file instead.
    if (const char *path = std::getenv("PS2X_FRAME_TIMES"); path && *path && !GSPcsx2Backend::Active())
    {
        frameTimes.open(path);
        if (frameTimes)
            frameTimes << "seconds,field,source_sequence,render_revision,new_source,interval_ms,submit_ms,swap_ms,shared,hash_valid,image_hash,image_changed\n";
        else
            std::cerr << "[gs-present] cannot write frame times: " << path << '\n';
    }
    using PresentClock = std::chrono::steady_clock;
    const auto presentStart = PresentClock::now();
    auto lastSwap = presentStart;
    const bool tracingFrames = frameTimes.is_open() && frameTimes.good();
    std::vector<uint64_t> screenshotFields;
    if (const char *fields = std::getenv("PS2X_SCREENSHOT_FIELDS"))
    {
        std::istringstream input(fields);
        std::string value;
        while (std::getline(input, value, ','))
            screenshotFields.push_back(std::strtoull(value.c_str(), nullptr, 10));
        std::sort(screenshotFields.rbegin(), screenshotFields.rend());
    }
    uint64_t lastSource = 0u;
    const char *hashSetting = std::getenv("PS2X_FRAME_HASH");
    const bool hashImages = hashSetting && *hashSetting == '1';
    uint64_t lastHash = 0u;
    uint32_t lastHashWidth = 0u;
    uint32_t lastHashHeight = 0u;
    bool hadHash = false;
    ps2_host_display::FpsOverlay fpsOverlay;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        if (IsWindowMinimized())
        {
            PollInputEvents();
            ps2_host_input::sample();
            if (WindowShouldClose())
            {
                requestStop();
                break;
            }
            WaitTime(1.0 / 120.0);
            continue;
        }
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        if (ps2_host_display::presentSyncEnabled())
        {
            static uint64_t lastPresentedField = 0u;
            const auto waitStart = std::chrono::steady_clock::now();
            while (eeScheduler().currentVSyncTick() == lastPresentedField && !isStopRequested() &&
                   std::chrono::steady_clock::now() - waitStart < std::chrono::milliseconds(50))
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            lastPresentedField = eeScheduler().currentVSyncTick();
        }
        ps2_host_display::updateWindowAspect();
        const auto submitStart = tracingFrames ? PresentClock::now() : presentStart;
        uint32_t presentWidth = 0u;
        uint32_t presentHeight = 0u;
        GSSharedPresent::Frame sharedFrame;
        // PS2X_MTVU_TRACE: the host present, with the display buffer it is about to show and the field.
        ps2x::asyncvif::traceEvent('R', static_cast<uint32_t>(m_memory.gs().dispfb1 & 0x1FFu) |
                                            (static_cast<uint32_t>(m_memory.gs().dispfb2 & 0x1FFu) << 9u) |
                                            (static_cast<uint32_t>(m_memory.gs().pmode & 3u) << 18u) |
                                            static_cast<uint32_t>(eeScheduler().currentVSyncTick() << 20u));
        // PCSX2's GS presents into its child window on the GS thread; this loop then only keeps the window, the input
        // and the pacing.
        const bool pcsx2Gs = GSPcsx2Backend::Active();
        if (pcsx2Gs)
        {
            GSPcsx2Backend::HostTick();
            // The overlays fpsOverlay.draw() and the save-state message draw with raylib below would be hidden under
            // the GS window: the GS draws them (same texts, same places).
            std::string top;
            uint32_t topRgba = 0xCC000000u;
            float topAlpha = 0.0f;
            if (ps2x::savestate::overlayMessage(top, topAlpha) || ps2x::pnach::overlayMessage(top, topAlpha))
                topRgba = 0x99000000u; // black, 60%
            else
            {
                std::lock_guard<std::mutex> lock(ps2_host_display::overlayMutex);
                top = ps2_host_display::overlayNote;
                topAlpha = top.empty() ? 0.0f : 1.0f;
                topRgba = 0xCC3721BEu; // raylib MAROON (190, 33, 55), 80%
            }
            GSPcsx2Backend::SetOsd(top.c_str(), topRgba, topAlpha, fpsOverlay.visible ? fpsOverlay.text.c_str() : "");
        }
        else
            UploadFrame(frameTex, this, presentWidth, presentHeight, hashImages, sharedFrame);
        Texture2D displayTex = frameTex;
        const bool shared = GSSharedPresent::Active();
        if (shared)
        {
            sharedFrame = {};
            displayTex = {};
            presentWidth = presentHeight = 0u;
            if (GSSharedPresent::Acquire(sharedFrame))
            {
                displayTex = Texture2D{sharedFrame.texture, static_cast<int>(sharedFrame.width),
                                       static_cast<int>(sharedFrame.height), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                presentWidth = sharedFrame.width;
                presentHeight = sharedFrame.height;
            }
        }
        else
        {
            sharedFrame.width = presentWidth;
            sharedFrame.height = presentHeight;
        }

        // With PCSX2's GS the image is the child window's; this window presents nothing of its own unless the F1
        // debugger (drawn by raylib) is open. Its black frames covered the child in fullscreen: a window as big as the
        // monitor gets its swap chain flipped straight to the screen, skipping the composition that puts the child on
        // top (PROGRESS §98). PS2X_HOST_PRESENT=1 always presents, as before.
        static const bool alwaysPresent = []
        { const char *v = std::getenv("PS2X_HOST_PRESENT"); return v && *v == '1'; }();
        const bool hostPresents = !pcsx2Gs || alwaysPresent || GSPcsx2Backend::HostUiVisible();
        if (!hostPresents)
            PollInputEvents();
        else
        {
        BeginDrawing();
        ClearBackground(BLACK);
        if (!pcsx2Gs && displayTex.id != 0 && presentWidth != 0u && presentHeight != 0u)
        {
            const float screenWidth = static_cast<float>(GetScreenWidth());
            const float screenHeight = static_cast<float>(GetScreenHeight());
            const float displayAspect = ps2_host_display::aspect();
            float dstWidth = screenWidth;
            float dstHeight = screenHeight;
            if (!ps2_host_display::fitWindow.load(std::memory_order_relaxed))
            {
                dstHeight = screenWidth / displayAspect;
                if (dstHeight > screenHeight)
                {
                    dstHeight = screenHeight;
                    dstWidth = screenHeight * displayAspect;
                }
            }
            const Rectangle srcRect{0.0f, 0.0f, static_cast<float>(presentWidth), static_cast<float>(presentHeight)};
            const Rectangle dstRect{(screenWidth - dstWidth) * 0.5f, (screenHeight - dstHeight) * 0.5f, dstWidth, dstHeight};
            DrawTexturePro(displayTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        {
            std::string stateText;
            float stateAlpha = 0.0f;
            if (ps2x::savestate::overlayMessage(stateText, stateAlpha))
            {
                const int fontSize = std::max(18, GetScreenHeight() / 30);
                const int width = MeasureText(stateText.c_str(), fontSize);
                DrawRectangle(12, 12, width + 24, fontSize + 16, Fade(BLACK, 0.6f * stateAlpha));
                DrawText(stateText.c_str(), 24, 20, fontSize, Fade(RAYWHITE, stateAlpha));
            }
        }
        if (!screenshotFields.empty() && eeScheduler().currentVSyncTick() >= screenshotFields.back())
        {
            if (const char *directory = std::getenv("PS2X_SCREENSHOT_DIR"))
            {
                const auto path = std::filesystem::path(directory) / ("field-" + std::to_string(eeScheduler().currentVSyncTick()) + ".png");
                rlDrawRenderBatchActive();
                const Image screen = LoadImageFromScreen();
                ExportImage(screen, path.string().c_str());
                UnloadImage(screen);
            }
            screenshotFields.pop_back();
        }
        fpsOverlay.draw();
        }
        const auto swapStart = tracingFrames ? PresentClock::now() : presentStart;
        if (hostPresents)
            EndDrawing();
        // The image shown: the GS thread's shared-present sequence, or (copy path) the field it was uploaded for.
        ps2_host_display::notePresent(eeScheduler().currentVSyncTick(), shared ? sharedFrame.sequence : eeScheduler().currentVSyncTick());
        fpsOverlay.present(eeScheduler().currentVSyncTick());
        if (tracingFrames)
        {
            const auto now = PresentClock::now();
            const uint64_t field = eeScheduler().currentVSyncTick();
            const uint64_t source = shared ? sharedFrame.sequence : field;
            frameTimes << std::chrono::duration<double>(now - presentStart).count() << ',' << field << ','
                       << source << ',' << sharedFrame.renderSequence << ','
                       << (shared ? static_cast<int>(source != 0u && source != lastSource) : -1) << ','
                       << std::chrono::duration<double, std::milli>(now - lastSwap).count() << ','
                       << std::chrono::duration<double, std::milli>(swapStart - submitStart).count() << ','
                       << std::chrono::duration<double, std::milli>(now - swapStart).count() << ',' << shared;
            const bool imageChanged = sharedFrame.hashValid && (!hadHash || sharedFrame.contentHash != lastHash ||
                                      sharedFrame.width != lastHashWidth || sharedFrame.height != lastHashHeight);
            frameTimes << ',' << sharedFrame.hashValid << ',' << sharedFrame.contentHash << ','
                       << (sharedFrame.hashValid ? static_cast<int>(imageChanged) : -1) << '\n';
            hadHash = sharedFrame.hashValid;
            lastHash = sharedFrame.contentHash;
            lastHashWidth = sharedFrame.width;
            lastHashHeight = sharedFrame.height;
            lastSource = source;
            lastSwap = now;
        }
#if !defined(PLATFORM_VITA)
        if (IsKeyPressed(KEY_F11))
        {
            ps2_host_input::releaseMouse();
            const bool leavingFullscreen = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
            if (!leavingFullscreen)
                restoreMaximized = IsWindowMaximized();
            ToggleBorderlessWindowed();
            if (leavingFullscreen && restoreMaximized)
                MaximizeWindow();
        }
#endif
        ps2_host_input::sample();
        if (IsKeyPressed(KEY_F4))
            ps2x::pnach::requestReload(); // reload the enabled .pnach files
        ps2x::menu::hostTick();           // F2: the in-game menu
        if (IsKeyPressed(KEY_F5))
            ps2x::savestate::requestQuickSave();
        if (IsKeyPressed(KEY_F9))
            ps2x::savestate::requestQuickLoad();
        if (IsKeyPressed(KEY_F3))
            fpsOverlay.visible = !fpsOverlay.visible;
        if (IsKeyPressed(KEY_F8))
        {
            // Debug capture: VU1 trace of the next 2 fields (vu1trace_<tick>.bin) plus the EE RAM (ram_<tick>.bin).
            const unsigned long long tick = static_cast<unsigned long long>(m_memory.gs().vsyncTick.load());
            char name[64];
            std::snprintf(name, sizeof(name), "ram_%llu.bin", tick);
            if (std::FILE *file = std::fopen(name, "wb"))
            {
                std::fwrite(m_memory.getRDRAM(), 1, PS2_RAM_SIZE, file);
                std::fclose(file);
            }
            ps2xRequestVu1Trace(2u);
            std::fprintf(stderr, "[F8] wrote %s, VU1 trace requested\n", name);
        }

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    if (frameTex.id != 0)
        UnloadTexture(frameTex);
    GSSharedPresent::ShutdownHost();
    ps2_host_input::releaseMouse();
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
