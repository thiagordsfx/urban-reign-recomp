#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_memory.h"
#include "ipu/ps2_ipu_compat.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_log.h"

// Defined in gs/gs_frontend.cpp: replays GIF data deferred at a stalled GS SIGNAL.
void ps2xGsReleaseStall();
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <vector>

// GS -> EE image download through the VIF1 FIFO (TRXDIR = 1). Games that read the FIFO themselves, like Urban
// Reign's libgraph store-image loop (polls VIF1_STAT.FQC, then LQ from 0x10005000), never saw any data: only the
// sceGsExecStoreImage HLE consumed the GS backend's local->host buffer. A small FIFO (16 qwords, like the hardware)
// is refilled from the GS; VIF1_STAT reports its count in FQC and FDR = 1 while data is pending.
// PS2X_VIF1_DOWNLOAD=0 disables it.
extern std::atomic<uint32_t> g_gsDownloadSerial; // gs_frontend.cpp
void ps2xWakeEeForGsInterrupt() noexcept;          // EeScheduler.cpp

void ps2xEeFlushDeferredCycles() noexcept; // Kernel/EeScheduler.cpp
void ps2xEeClearLatchedDmac(uint32_t channels); // Kernel/EeScheduler.cpp
bool ps2xGsInterruptUndelivered() noexcept;      // Kernel/EeScheduler.cpp
bool ps2xGsStallActive();                       // gs/gs_frontend.cpp
bool ps2xGsSignalQueuePending();

namespace
{
    // MTVU: a GIF DMA the EE starts is over for the EE at once, but its data reaches the GS later, on the GIF thread,
    // and the SIGNAL/FINISH it carries reaches the EE later still. On hardware D2_CHCR.STR stays set until the GS has
    // taken the data, and the SIGNAL interrupt comes before the channel is free. Urban Reign's GS handler (0x10bc38)
    // finds the next chunk of the frame's GIF chain from D2_TADR; the movie player, seeing the channel free, sent its
    // picture in the middle of a frame (the VU worker had started late), the handler then kicked the movie's chain
    // instead of the frame's, the frame's last SIGNAL never came and the intro movie froze (1 boot in 2 on Linux).
    // So the EE reads STR = 1 while the VU/GIF workers have work, GIF data waits at a SIGNAL, or a GS interrupt has not
    // been delivered. PS2X_GIF_BUSY_UNTIL_GS=0 turns it off. If it lasts 100 ms the EE sees the real value until the
    // workers are idle once (a guest that polls with interrupts off could otherwise never let the GS go on).
    bool gifStillRunningOnGs(PS2Memory &memory)
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GIF_BUSY_UNTIL_GS"); return !v || *v != '0'; }();
        if (!enabled || !ps2x::asyncvif::active(memory))
            return false;
        static std::chrono::steady_clock::time_point busySince{};
        static bool gaveUp = false;
        const bool running = ps2x::asyncvif::pipelineBusy(memory) || ps2xGsStallActive() || ps2xGsSignalQueuePending() ||
                             ps2xGsInterruptUndelivered();
        if (!running)
        {
            busySince = {};
            gaveUp = false;
            return false;
        }
        if (gaveUp)
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (busySince == std::chrono::steady_clock::time_point{})
            busySince = now;
        else if (now - busySince > std::chrono::milliseconds(100))
        {
            gaveUp = true;
            static std::atomic<uint32_t> s_gaveUp{0u};
            if (s_gaveUp.fetch_add(1u) < 10u)
                std::fprintf(stderr, "[mtvu] D2_CHCR busy for 100 ms; showing the EE the real STR\n");
            return false;
        }
        return true;
    }
}

namespace
{
    GS *g_localToHostGs = nullptr;
    std::mutex g_vif1DownloadMutex;
    std::deque<uint8_t> g_vif1DownloadFifo;
    uint32_t g_vif1DownloadSeen = 0u;   // last g_gsDownloadSerial handled
    bool g_vif1DownloadActive = false;  // GS buffer may still hold bytes of the current download
    // VIF1_STAT.FDR as written by the EE (bit 23): libgraph sets it before a download and clears it after. Data is
    // only visible while it is set, and clearing it drops whatever the game left unread, so a short read can never
    // leave VIF1_STAT reporting a non-empty FIFO (which made every path sync time out and the game crawl).
    std::atomic<bool> g_vif1Fdr{false};

    // Reads of the GS privileged registers (CSR, SIGLBLID, IMR...) used to wait for the async VIF/VU1/GIF workers to
    // drain. The GS runs asynchronously on hardware too and these registers are updated atomically by the GIF worker
    // before it raises the interrupt, so the EE can read the current value at once. Urban Reign's SIGNAL handler
    // (0x10bc38, many calls per frame) spent ~40% of the game thread waiting there. PS2X_GS_READ_SYNC=1 restores it.
    // CSR.FIFO (bits 14-15) as the EE reads it: 01 = empty. GIF packets reach the GS front end synchronously here, so
    // whenever the EE looks the FIFO has drained (PCSX2 reports empty too). With 00 ("neither empty nor almost full")
    // Urban Reign's wait-for-GS loop 0x122ca0 never saw the GS idle and spun until the next field before falling back
    // to sceGsSyncPath: ~12% of the game thread in fights. PS2X_GS_CSR_FIFO=0 reports the raw value.
    uint64_t csrForRead(uint64_t csr)
    {
        static const bool report = [] { const char *v = std::getenv("PS2X_GS_CSR_FIFO"); return !v || *v != '0'; }();
        return report ? (csr & ~0xC000ull) | 0x4000ull : csr;
    }

    // DMA chain buffers (PendingTransfer::chainData) recycled instead of freed: each chain DMA used to allocate a
    // fresh vector the size of the whole chain and free it once processed. For buffers that big the heap returns
    // the pages to the OS every time (RtlFreeHeap -> ZwFreeVirtualMemory): ~4% of the game thread in fights.
    thread_local std::vector<std::vector<uint8_t>> t_chainPool;

    std::vector<uint8_t> takeChainBuffer()
    {
        if (!t_chainPool.empty())
        {
            std::vector<uint8_t> buffer = std::move(t_chainPool.back());
            t_chainPool.pop_back();
            buffer.clear();
            return buffer;
        }
        return ps2x::asyncvif::takeBuffer();
    }

    void recycleChainBuffer(std::vector<uint8_t> &&buffer)
    {
        if (buffer.capacity() != 0u && t_chainPool.size() < 16u)
        {
            buffer.clear();
            t_chainPool.push_back(std::move(buffer));
        }
    }

    bool gsPrivReadSync()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GS_READ_SYNC"); return v && *v == '1'; }();
        return enabled;
    }

    // Writes to CSR, IMR and SIGLBLID (the SIGNAL/FINISH handshake, ~2,300/s in Urban Reign) do not wait for the async
    // workers: the GIF thread holds its own queue at a SIGNAL and the EE releases it with the CSR write (replaying a
    // stalled GIF stream is exclusive, see ps2xGsReleaseStall). The hangs this used to cause were lost releases and
    // the SIGLBLID read-modify-write race in the game's GS handler, both fixed (docs/PROGRESS.md §43). Without MTVU
    // there is nothing to wait for anyway. PS2X_GS_WRITE_SYNC=1 makes every privileged write synchronize again.
    bool gsPrivWriteNeedsSync(uint32_t regOff)
    {
        static const bool relaxed = [] { const char *v = std::getenv("PS2X_GS_WRITE_SYNC"); return !(v && *v == '1'); }();
        return !relaxed || (regOff != 0x1000u && regOff != 0x1010u && regOff != 0x1080u);
    }

    bool vif1DownloadEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_VIF1_DOWNLOAD"); return !v || *v != '0'; }();
        return enabled;
    }

    // Caller holds g_vif1DownloadMutex. Returns the number of whole qwords in the FIFO.
    uint32_t refillVif1DownloadFifo()
    {
        // Host-side buffer, refilled in large requests (one GS-worker round trip each); VIF1_STAT.FQC still reports at
        // most 16 qwords like the hardware FIFO.
        constexpr size_t kFifoBytes = 64u * 1024u;
        const uint32_t serial = g_gsDownloadSerial.load(std::memory_order_acquire);
        if (serial != g_vif1DownloadSeen)
        {
            // A new download replaces whatever the game left unread of the previous one.
            g_vif1DownloadSeen = serial;
            g_vif1DownloadFifo.clear();
            g_vif1DownloadActive = true;
        }
        if (g_vif1DownloadActive && g_localToHostGs && g_vif1DownloadFifo.size() < 256u) // refill only when nearly empty
        {
            static thread_local std::vector<uint8_t> chunkBuffer(kFifoBytes);
            uint8_t *chunk = chunkBuffer.data();
            const uint32_t want = static_cast<uint32_t>(kFifoBytes - g_vif1DownloadFifo.size());
            const uint32_t got = g_localToHostGs->consumeLocalToHostBytes(chunk, want);
            g_vif1DownloadFifo.insert(g_vif1DownloadFifo.end(), chunk, chunk + got);
            if (got < want)
                g_vif1DownloadActive = false; // GS side drained
        }
        return static_cast<uint32_t>((g_vif1DownloadFifo.size() + 15u) / 16u);
    }
}

// Lock-free pre-check: nothing to do unless a download started since the last one finished, or bytes are buffered.
static bool vif1DownloadMayHaveData()
{
    if (!g_vif1Fdr.load(std::memory_order_acquire))
        return false;
    if (g_gsDownloadSerial.load(std::memory_order_acquire) != g_vif1DownloadSeen)
        return true;
    std::lock_guard<std::mutex> lock(g_vif1DownloadMutex);
    return g_vif1DownloadActive || !g_vif1DownloadFifo.empty();
}

void ps2xSetLocalToHostGs(GS *gs)
{
    g_localToHostGs = gs;
}


namespace
{
    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline bool isAsyncVifRegister(uint32_t addr)
    {
        return (addr >= 0x10003000u && addr < 0x10003100u) || (addr >= 0x10003C00u && addr < 0x10004000u) ||
               (addr >= 0x10005000u && addr < 0x10007000u);
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;

    // GS SIGNAL stall emulation. On hardware, a SIGNAL that arrives while CSR.SIGNAL is still set stops the GS until the
    // EE clears the bit, so every SIGNAL in a packet produces its own interrupt with its own SIGLBLID. The runtime does
    // not stall GIF processing; instead such SIGNALs are queued and delivered one at a time as the EE clears CSR.SIGNAL.
    // Draws that follow a queued SIGNAL are not held back. g_gsSignalSerial counts deliveries so the scheduler sees a
    // new interrupt edge even when the bit is re-set before it could observe it clear.
    struct QueuedGsSignal
    {
        uint32_t id;
        uint32_t mask;
        bool finish; // FINISH queued behind pending SIGNALs keeps its place in the packet order
    };
    std::mutex g_gsSignalMutex;
    std::deque<QueuedGsSignal> g_gsSignalQueue;
    std::atomic<uint32_t> g_gsSignalSerial{0u};
    // Same for FINISH: with MTVU the GIF thread can set FINISH again right after the EE cleared it, before the
    // scheduler looked; the bit then reads 1 -> 1 and only the serial shows the new interrupt.
    std::atomic<uint32_t> g_gsFinishSerial{0u};
    // Set while the game's GS interrupt handler runs (ps2xGsSetHandlerBusy, MTVU only). Urban Reign's handler does a
    // read-modify-write of SIGLBLID; a SIGNAL the GIF thread applied in between was wiped and the frame-done flag never
    // came (hang at 0x1298a0). On hardware the handler is over long before the GS reaches the next SIGNAL, so SIGNAL
    // and FINISH wait for it like for a pending CSR.SIGNAL.
    std::atomic<bool> g_gsHandlerBusy{false};

    bool gsSignalBlocked(const GSRegisters &gs)
    {
        return (gs.csr.load() & 0x1u) != 0u || g_gsHandlerBusy.load(std::memory_order_acquire);
    }

    void setGsFinish(GSRegisters &gs)
    {
        gs.csr.fetch_or(0x2u);
        g_gsFinishSerial.fetch_add(1u, std::memory_order_acq_rel);
        ps2x::asyncvif::traceEvent('N');
        if (ps2x::asyncvif::onAsyncThread())
            ps2xWakeEeForGsInterrupt();
    }

    // PS2X_GS_SIGNAL_LOG=2: unlimited log of every SIGNAL delivery/queueing and SIGLBLID write, with the result.
    bool gsSignalTraceAll()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_GS_SIGNAL_LOG"); return v && *v == '2'; }();
        return enabled;
    }

    void applyGsSignal(GSRegisters &gs, uint32_t id, uint32_t mask)
    {
        uint32_t lo = static_cast<uint32_t>(gs.siglblid & 0xFFFFFFFFull);
        lo = (lo & ~mask) | (id & mask);
        gs.siglblid = (gs.siglblid & 0xFFFFFFFF00000000ull) | lo;
        if (gsSignalTraceAll())
            std::fprintf(stderr, "[gs-sig2] apply id=%x mask=%x -> SIGID=%x csr=0x%llx\n", id, mask, lo,
                         static_cast<unsigned long long>(gs.csr.load()));
        gs.csr.fetch_or(0x1u);
        g_gsSignalSerial.fetch_add(1u, std::memory_order_acq_rel);
        ps2x::asyncvif::traceEvent('I', lo);
        if (ps2x::asyncvif::onAsyncThread())
            ps2xWakeEeForGsInterrupt();
    }

    void releaseQueuedGsSignalLocked(GSRegisters &gs);

    void releaseQueuedGsSignal(GSRegisters &gs)
    {
        releaseQueuedGsSignalLocked(gs);
        // Then replay GIF data the GS deferred at a stalled SIGNAL (gs_frontend.cpp), outside g_gsSignalMutex. With
        // MTVU the GIF stage thread does it (the GS front end must not run on two threads at once).
        if (!gsSignalBlocked(gs) && !ps2x::asyncvif::postGifStallRelease())
            ps2xGsReleaseStall();
    }

    void releaseQueuedGsSignalLocked(GSRegisters &gs)
    {
        std::lock_guard<std::mutex> lock(g_gsSignalMutex);
        // Deliver queued events until a SIGNAL is pending again (the GS would stall there once more).
        while (!gsSignalBlocked(gs) && !g_gsSignalQueue.empty())
        {
            const QueuedGsSignal next = g_gsSignalQueue.front();
            g_gsSignalQueue.pop_front();
            if (next.finish)
            {
                setGsFinish(gs);
                if (gsSignalTraceAll())
                    std::fprintf(stderr, "[gs-sig2] finish (queued) released\n");
                if (const char *v = std::getenv("PS2X_GS_SIGNAL_LOG"); v && *v == '1')
                    std::fprintf(stderr, "[gs-signal] queued FINISH released csr=0x%llx\n", static_cast<unsigned long long>(gs.csr.load()));
            }
            else
                applyGsSignal(gs, next.id, next.mask);
        }
    }

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Bits 0..4 of the low dword (SIGNAL/FINISH/
    // HSINT/VSINT/EDWINT) are write-one-to-clear; everything else is a plain merge. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(GSRegisters &gs, uint32_t off, uint32_t value)
    {
        std::atomic<uint64_t> &csr = gs.csr;
        constexpr uint32_t kW1cMask = 0x1Fu;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t mergedLow = (oldLow & kW1cMask) | (value & ~kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(mergedLow);
                desired &= ~static_cast<uint64_t>(value & kW1cMask);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
        if (off == 0u && (value & 0x1u) != 0u)
            releaseQueuedGsSignal(gs);
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write (bits 0..4 are still
    // write-one-to-clear against the current value).
    inline void writeCsrFull(GSRegisters &gs, uint64_t value)
    {
        std::atomic<uint64_t> &csr = gs.csr;
        constexpr uint64_t kW1cMask = 0x1Full;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = (expected & kW1cMask) | (value & ~kW1cMask);
            desired &= ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
        if ((value & 0x1u) != 0u)
            releaseQueuedGsSignal(gs);
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        // H-BLANK source. Measured in PCSX2 (Urban Reign, NTSC interlaced, kernel-owned timer 3): 525 ticks per field,
        // ~31468 Hz, i.e. both half-line H-BLANKs of an interlaced field are counted.
        31468ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

    constexpr uint32_t kEeTimerBacklogLimit = 1u << 20;

    struct EeTimerBacklog
    {
        std::array<uint32_t, 4> overflows{};
        std::array<uint32_t, 4> compares{};
        uint32_t undelivered = 0u;
    };

    std::mutex g_eeTimerBacklogMutex;
    std::unordered_map<const void *, EeTimerBacklog> g_eeTimerBacklogs;

    EeTimerBacklog &eeTimerBacklog(const void *memory)
    {
        std::lock_guard<std::mutex> lock(g_eeTimerBacklogMutex);
        return g_eeTimerBacklogs[memory];
    }

    uint32_t addBacklog(uint32_t pending, uint64_t events)
    {
        return static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(pending) + events, kEeTimerBacklogLimit));
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    ps2x::asyncvif::stop(*this);
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        // VU0 memories are 4 KB, backed by 16 KB (zero above 4 KB) so VU0 microprograms can run through the same
        // ahead-of-time compiled path as VU1 (see executeVU0Microprogram / lookupCompiledVu0Program).
        constexpr uint32_t kVu0Backing = 0x4000u;
        m_vu0Code = new uint8_t[kVu0Backing];
        m_vu0Data = new uint8_t[kVu0Backing];
        std::memset(m_vu0Code, 0, kVu0Backing);
        std::memset(m_vu0Data, 0, kVu0Backing);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    eeTimerBacklog(this) = {};
    m_eeTimers = {};
    // The BIOS kernel leaves timer 3 free-running on the H-BLANK clock (it backs SetAlarm). The runtime implements
    // alarms natively, but games also read T3_COUNT as a time base (Urban Reign's VSync handler and timeouts do), so
    // keep it counting. No compare/overflow interrupt is enabled, so it never raises TIM3.
    m_eeTimers[3].mode = kEeTimerModeCue | 3u;
    m_eeTimerPendingCycles = 0u;
    m_eeTimerEventBudget = computeCyclesUntilNextEeTimerInterrupt();
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    if (m_gifStatWritten)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        auto gifStatIt = m_ioRegisters.find(kGifStat);
        if (gifStatIt != m_ioRegisters.end())
            gifStatIt->second &= ~kGifFqcMask;
        m_gifStatWritten = false;
    }

    m_eeTimerPendingCycles += eeCycles;
    if (m_eeTimerPendingCycles < m_eeTimerEventBudget)
    {
        return 0u;
    }
    return flushEeTimers();
}

uint32_t PS2Memory::flushEeTimers() noexcept
{
    const uint64_t eeCycles = m_eeTimerPendingCycles;
    m_eeTimerPendingCycles = 0u;
    EeTimerBacklog &backlog = eeTimerBacklog(this);
    uint32_t interruptMask = backlog.undelivered;
    backlog.undelivered = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            backlog.overflows[index] = 0u;
            backlog.compares[index] = 0u;
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;
        const uint64_t comparePeriod = zeroReturn && compare != 0u ? compare : 0x10000ull;
        const uint64_t compareEvents = compareReached ? 1u + (ticks - compareDistance) / comparePeriod : 0u;
        uint64_t overflowEvents = 0u;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            overflowEvents = overflowReached ? (compare == 0u ? 1u + (ticks - overflowDistance) / 0x10000ull : 1u) : 0u;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            overflowEvents = (static_cast<uint64_t>(oldCount) + ticks) >> 16u;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareEvents != 0u && (timer.mode & kEeTimerModeCmpe) != 0u)
        {
            uint64_t missed = compareEvents;
            if ((timer.mode & kEeTimerModeEquf) == 0u)
            {
                timer.mode |= kEeTimerModeEquf;
                interruptMask |= 1u << index;
                --missed;
            }
            backlog.compares[index] = addBacklog(backlog.compares[index], missed);
        }
        if (overflowEvents != 0u && (timer.mode & kEeTimerModeOvfe) != 0u)
        {
            uint64_t missed = overflowEvents;
            if ((timer.mode & kEeTimerModeOvff) == 0u)
            {
                timer.mode |= kEeTimerModeOvff;
                interruptMask |= 1u << index;
                --missed;
            }
            backlog.overflows[index] = addBacklog(backlog.overflows[index], missed);
        }
    }
    m_eeTimerEventBudget = computeCyclesUntilNextEeTimerInterrupt();
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    if (m_eeTimerEventBudget == std::numeric_limits<uint64_t>::max())
    {
        return m_eeTimerEventBudget;
    }
    return m_eeTimerEventBudget - m_eeTimerPendingCycles;
}

uint64_t PS2Memory::computeCyclesUntilNextEeTimerInterrupt() const noexcept
{
    if (eeTimerBacklog(this).undelivered != 0u)
    {
        return 0u;
    }
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    if (physAddr >= PS2_VU1_CODE_BASE && physAddr < PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE)
        ps2x::asyncvif::sync(const_cast<PS2Memory &>(*this));
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        if (gsPrivReadSync())
            ps2x::asyncvif::sync(*this);
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = csrForRead(gs_regs.csr.load());
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

// IPU (MPEG decoder): ipu/ps2_ipu.c, from the Ace Combat 5 recompilation, decodes; this side is its DMA, ported from
// that runtime's ps2_hw.c. CRI Sofdec (Urban Reign's FMVs) programs the IPU directly: commands go to IPU_CMD, the
// bitstream comes through toIPU (channel 4, normal or chain) and the pictures leave through fromIPU (channel 3). A
// command runs when it is written; when it runs out of bitstream it pulls more from toIPU, and its output is drained
// into fromIPU as it is produced. PS2X_IPU=0 restores the old placeholder registers (the FMVs stay black).
namespace
{
    constexpr uint32_t kFromIpuChannel = 0x1000B000u;
    constexpr uint32_t kToIpuChannel = 0x1000B400u;
    PS2Memory *g_ipuMemory = nullptr;
    bool g_ipuTagEnd = false;
    uint32_t g_ipuPullLeft = 0u;

    bool ipuEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_IPU"); return !v || *v != '0'; }();
        return enabled;
    }

    // PS2X_IPU_TRACE=<n>: the first n IPU register, FIFO and DMA accesses.
    void ipuTrace(const char *what, uint32_t address, uint32_t value)
    {
        static long left = [] { const char *v = std::getenv("PS2X_IPU_TRACE"); return v ? std::atol(v) : 0L; }();
        if (left <= 0)
            return;
        --left;
        std::fprintf(stderr, "[ipu-trace] %s %08x = %08x\n", what, address, value);
    }

    // PS2X_IPU_PICLOG=<n>: n lines of picture-level decoding: each start code an FDEC returns (slice 01-AF, picture
    // 00, sequence B3...) with the BDECs since the previous one, each CSC, and each BCLR.
    void ipuPictureLog(uint32_t command)
    {
        static long left = [] { const char *v = std::getenv("PS2X_IPU_PICLOG"); return v ? std::atol(v) : 0L; }();
        static uint32_t bdecs = 0u;
        if (left <= 0)
            return;
        const uint32_t code = command >> 28;
        if (code == 2u)
        {
            ++bdecs;
            return;
        }
        const bool busy = (ps2_ipu_read32(0x10002010u) >> 31) != 0u;
        static const bool all = [] { const char *v = std::getenv("PS2X_IPU_PICLOG_ALL"); return v && *v == '1'; }();
        if (all && code != 7u && code != 0u)
        {
            std::fprintf(stderr, "[ipu-pic]   cmd %08x -> %08x ctrl %08x%s\n", command, ps2_ipu_read32(0x10002000u),
                         ps2_ipu_read32(0x10002010u), busy ? " busy" : "");
            --left;
        }
        if (code == 4u)
        {
            if (busy)
            {
                std::fprintf(stderr, "[ipu-pic] FDEC %08x busy (bdec %u)\n", command, bdecs);
                --left;
                return;
            }
            const uint32_t data = ps2_ipu_read32(0x10002000u);
            if ((data >> 8) == 1u)
            {
                std::fprintf(stderr, "[ipu-pic] start %02x after %u bdec\n", data & 0xFFu, bdecs);
                bdecs = 0u;
                --left;
            }
        }
        else if (code == 7u || code == 0u)
        {
            auto &io = g_ipuMemory->m_ioRegisters;
            std::fprintf(stderr, "[ipu-pic] %s %08x (bdec %u)%s toIPU %08x/%x fromIPU %08x/%x out %u in %u\n",
                         code == 7u ? "CSC" : "BCLR", command, bdecs, busy ? " busy" : "", io[0x1000B410u],
                         io[0x1000B420u], io[0x1000B010u], io[0x1000B020u], ps2_ipu_out_avail(), ps2_ipu_in_space());
            --left;
        }
    }

    // PS2X_IPU_STATS=1: per second, the IPU commands by type with the host time they took (a command runs when it is
    // written, or when the toIPU/fromIPU DMA start resumes it: kinds 14/15), total and longest.
    void ipuCommandStats(uint32_t kind, std::chrono::steady_clock::time_point started)
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_IPU_STATS"); return v && *v == '1'; }();
        if (!enabled)
            return;
        static uint32_t counts[16] = {};
        static double totalMs[16] = {};
        static double maxMs[16] = {};
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - started).count();
        kind &= 15u;
        ++counts[kind];
        totalMs[kind] += ms;
        maxMs[kind] = std::max(maxMs[kind], ms);
        if (now - last < std::chrono::seconds(1))
            return;
        last = now;
        static const char *const names[16] = {"BCLR", "IDEC", "BDEC", "VDEC", "FDEC", "SETIQ", "SETVQ", "CSC",
                                               "PACK", "SETTH", "?", "?", "?", "?", "toIPU", "fromIPU"};
        std::string line = "[ipu-stats]";
        for (uint32_t i = 0u; i < 16u; ++i)
        {
            if (!counts[i])
                continue;
            char item[96];
            std::snprintf(item, sizeof(item), " %s %u %.1fms (max %.2f)", names[i], counts[i], totalMs[i], maxMs[i]);
            line += item;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
        std::memset(counts, 0, sizeof(counts));
        std::memset(totalMs, 0, sizeof(totalMs));
        std::memset(maxMs, 0, sizeof(maxMs));
    }

    // Host time of the last decoding command (BDEC/IDEC/CSC/PACK), for ps2xIpuActiveRecently.
    std::atomic<int64_t> g_ipuLastDecodeNs{0};

    void ipuAttach(PS2Memory &memory)
    {
        static bool initialized = false;
        if (!initialized)
        {
            initialized = true;
            ps2_ipu_init();
        }
        g_ipuMemory = &memory;
    }

    // Host pointer for a DMA address (MADR/TADR; bit 31 = scratchpad) and how many bytes follow it in that memory.
    uint8_t *ipuDmaPointer(PS2Memory &memory, uint32_t address, uint32_t &room)
    {
        room = 0u;
        if ((address & 0x80000000u) != 0u || (address >= 0x70000000u && address < 0x70004000u))
        {
            const uint32_t offset = address & (PS2_SCRATCHPAD_SIZE - 1u) & ~15u;
            room = PS2_SCRATCHPAD_SIZE - offset;
            return memory.getScratchpad() + offset;
        }
        const uint32_t offset = address & 0x01FFFFF0u;
        if (offset >= PS2_RAM_SIZE)
            return nullptr;
        room = PS2_RAM_SIZE - offset;
        return memory.getRDRAM() + offset;
    }

    // toIPU: feeds the IPU up to g_ipuPullLeft qwords (and only what its FIFO takes), walking the DMA chain in chain
    // mode; the channel ends at the end of the chain.
    void ipuToAdvance(PS2Memory &memory)
    {
        auto &regs = memory.m_ioRegisters;
        uint32_t &chcr = regs[kToIpuChannel];
        uint32_t &madr = regs[kToIpuChannel + 0x10u];
        uint32_t &qwc = regs[kToIpuChannel + 0x20u];
        uint32_t &tadr = regs[kToIpuChannel + 0x30u];
        uint32_t tags = 0u;
        while ((chcr & 0x100u) != 0u)
        {
            if ((qwc & 0xFFFFu) != 0u)
            {
                uint32_t space = ps2_ipu_in_space();
                if (space > g_ipuPullLeft)
                    space = g_ipuPullLeft;
                if (space == 0u)
                    return;
                uint32_t n = std::min(qwc & 0xFFFFu, space);
                uint32_t room = 0u;
                const uint8_t *src = ipuDmaPointer(memory, madr, room);
                if (room / 16u != 0u && n > room / 16u)
                    n = room / 16u;
                if (!src || n == 0u)
                {
                    qwc = 0u;
                    continue;
                }
                ps2_ipu_dma_in(src, n);
                madr += n * 16u;
                qwc -= n;
                g_ipuPullLeft -= n;
                continue;
            }
            if (((chcr >> 2) & 3u) != 1u || g_ipuTagEnd || ++tags > 4096u)
                break;
            uint32_t room = 0u;
            const uint8_t *tagBytes = ipuDmaPointer(memory, tadr, room);
            if (!tagBytes || room < 16u)
                break;
            uint64_t tag = 0u;
            std::memcpy(&tag, tagBytes, sizeof(tag));
            qwc = static_cast<uint32_t>(tag & 0xFFFFu);
            const uint32_t id = static_cast<uint32_t>((tag >> 28) & 7u);
            const bool irq = ((tag >> 31) & 1u) != 0u;
            const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0xFFFFFFF0u);
            if (addr == 0u && qwc == 0u && id == 0u)
            {
                g_ipuTagEnd = true; // an empty tag: the chain already ended
                break;
            }
            switch (id)
            {
            case 0: // refe
                madr = addr;
                tadr += 16u;
                g_ipuTagEnd = true;
                break;
            case 1: // cnt
                madr = tadr + 16u;
                tadr += 16u + qwc * 16u;
                break;
            case 2: // next
                madr = tadr + 16u;
                tadr = addr;
                break;
            case 3: // ref
            case 4: // refs
                madr = addr;
                tadr += 16u;
                break;
            case 7: // end
                madr = tadr + 16u;
                g_ipuTagEnd = true;
                break;
            default:
                qwc = 0u;
                g_ipuTagEnd = true;
                break;
            }
            if (irq)
                g_ipuTagEnd = true;
        }
        if ((chcr & 0x100u) == 0u)
            return;
        g_ipuTagEnd = false;
        memory.completeDmacChannel(kToIpuChannel, 4u);
    }

    // fromIPU: moves the decoded output to memory; the channel ends when its QWC is done.
    void ipuFromAdvance(PS2Memory &memory)
    {
        auto &regs = memory.m_ioRegisters;
        uint32_t &chcr = regs[kFromIpuChannel];
        uint32_t &madr = regs[kFromIpuChannel + 0x10u];
        uint32_t &qwc = regs[kFromIpuChannel + 0x20u];
        while ((chcr & 0x100u) != 0u && (qwc & 0xFFFFu) != 0u)
        {
            const uint32_t avail = ps2_ipu_out_avail();
            if (avail == 0u)
                return;
            uint32_t n = std::min(qwc & 0xFFFFu, avail);
            uint32_t room = 0u;
            uint8_t *dst = ipuDmaPointer(memory, madr, room);
            if (room / 16u != 0u && n > room / 16u)
                n = room / 16u;
            if (!dst || n == 0u)
            {
                qwc = 0u;
                break;
            }
            ps2_ipu_dma_out(dst, n);
            if ((madr & 0x80000000u) == 0u)
                memory.markModified(madr & 0x01FFFFF0u, n * 16u);
            madr += n * 16u;
            qwc -= n;
        }
        if ((chcr & 0x100u) == 0u || (qwc & 0xFFFFu) != 0u)
            return;
        memory.completeDmacChannel(kFromIpuChannel, 3u);
    }
}

// An FMV is being decoded (a decoding command in the last 0.5 s): EeScheduler then lets late vblanks catch up.
bool ps2xIpuActiveRecently() noexcept
{
    static const bool enabled = [] { const char *v = std::getenv("PS2X_IPU_CATCHUP"); return !v || *v != '0'; }();
    if (!enabled)
        return false;
    const int64_t last = g_ipuLastDecodeNs.load(std::memory_order_relaxed);
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return last != 0 && now - last < 500000000LL;
}

extern "C" void ps2_dmac_ipu_pull(void)
{
    if (!g_ipuMemory)
        return;
    const uint32_t want = ps2_ipu_want();
    g_ipuPullLeft = want > 8u ? want : 8u;
    ipuToAdvance(*g_ipuMemory);
    g_ipuPullLeft = 0u;
}

extern "C" void ps2_dmac_ipu_drain(void)
{
    if (g_ipuMemory)
        ipuFromAdvance(*g_ipuMemory);
}

extern "C" void ps2_dmac_ipu_report(u32 *chcr, u32 *madr, u32 *qwc, u32 *tadr)
{
    if (!g_ipuMemory)
    {
        *chcr = *madr = *qwc = *tadr = 0u;
        return;
    }
    auto &regs = g_ipuMemory->m_ioRegisters;
    *chcr = regs[kToIpuChannel];
    *madr = regs[kToIpuChannel + 0x10u];
    *qwc = regs[kToIpuChannel + 0x20u];
    *tadr = regs[kToIpuChannel + 0x30u];
}

extern "C" void ps2_dmac_ipu_history(void)
{
}

extern "C" void ps2_dmac_ipu_out_history(void)
{
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        if (gsPrivReadSync())
            ps2x::asyncvif::sync(*this);
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return csrForRead(gs_regs.csr.load());
        }
        uint64_t *reg = gsRegPtr(gs_regs, address);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // IPU_CMD / IPU_TOP: the BUSY bit is in the high word.
    if (physAddr >= 0x10002000u && physAddr < 0x10002040u && ipuEnabled())
    {
        ipuAttach(*this);
        return static_cast<uint64_t>(ps2_ipu_read32(physAddr)) | (static_cast<uint64_t>(ps2_ipu_read32(physAddr + 4u)) << 32);
    }
    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    if (physAddr == 0x10007000u && ipuEnabled()) // IPU_out_FIFO
    {
        ipuAttach(*this);
        ipuTrace("fifo-r", physAddr, ps2_ipu_out_avail());
        alignas(16) uint8_t qword[16] = {};
        ps2_ipu_dma_out(qword, 1u);
        return _mm_load_si128(reinterpret_cast<const __m128i *>(qword));
    }
    if (physAddr == 0x10005000u && vif1DownloadEnabled() && g_localToHostGs && vif1DownloadMayHaveData())
    {
        alignas(16) uint8_t qword[16] = {};
        std::lock_guard<std::mutex> lock(g_vif1DownloadMutex);
        refillVif1DownloadFifo();
        for (uint32_t i = 0; i < 16u && !g_vif1DownloadFifo.empty(); ++i)
        {
            qword[i] = g_vif1DownloadFifo.front();
            g_vif1DownloadFifo.pop_front();
        }
        return _mm_load_si128(reinterpret_cast<const __m128i *>(qword));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        uint32_t off = address & 7;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (gsPrivWriteNeedsSync(regOff))
            ps2x::asyncvif::sync(*this);
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 of the low dword are write-one-to-clear status bits.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
            if ((regOff == 0x70u || regOff == 0x90u) && off == 0u)
                ps2x::asyncvif::traceEvent('D', (regOff == 0x90u ? 0x1000u : 0u) | (value & 0x1FFu)); // display flip
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    if (isGsPrivReg(address))
    {
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        // PS2X_GS_PRIV_WRITE_HIST=1: count of EE writes per GS privileged register, every 2^16 writes.
        static const bool privHist = [] { const char *v = std::getenv("PS2X_GS_PRIV_WRITE_HIST"); return v && *v == '1'; }();
        if (privHist)
        {
            static std::map<uint32_t, uint64_t> hist;
            static uint64_t total = 0u;
            ++hist[regOff];
            if ((++total & 0xFFFFu) == 0u)
            {
                std::fprintf(stderr, "[gs-priv-write]");
                for (const auto &[reg, n] : hist)
                    std::fprintf(stderr, " %04x=%llu", reg, static_cast<unsigned long long>(n));
                std::fputc('\n', stderr);
                hist.clear();
            }
        }
        if (gsPrivWriteNeedsSync(regOff))
            ps2x::asyncvif::sync(*this);
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: bits 0..1 are write-one-to-clear status bits. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs, value);
            if (gsSignalTraceAll())
                std::fprintf(stderr, "[gs-sig2] EE CSR write 0x%llx -> csr=0x%llx\n", static_cast<unsigned long long>(value),
                             static_cast<unsigned long long>(gs_regs.csr.load()));
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            if (gsSignalTraceAll() && regOff == 0x1080u)
                std::fprintf(stderr, "[gs-sig2] EE SIGLBLID write 0x%llx (was 0x%llx)\n", static_cast<unsigned long long>(value),
                             static_cast<unsigned long long>(*reg));
            *reg = value;
            if (regOff == 0x70u || regOff == 0x90u)
                ps2x::asyncvif::traceEvent('D', (regOff == 0x90u ? 0x1000u : 0u) | static_cast<uint32_t>(value & 0x1FFu)); // display flip
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (!scratch && physAddr == 0x10007010u && ipuEnabled()) // IPU_in_FIFO
    {
        ipuAttach(*this);
        ipuTrace("fifo-w", physAddr, ps2_ipu_in_space());
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        ps2_ipu_dma_in(fifoData, 1u);
        return;
    }
    if (!scratch && physAddr == 0x10004000u) // VIF0_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        processVIF0Data(fifoData, sizeof(fifoData));
        return;
    }
    if (!scratch && physAddr == 0x10005000u) // VIF1_FIFO
    {
        alignas(16) uint8_t fifoData[16];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(fifoData), value);
        if (ps2x::asyncvif::active(*this))
        {
            std::vector<ps2x::asyncvif::Item> items(1);
            items[0].kind = ps2x::asyncvif::ItemKind::Vif1;
            items[0].data.assign(fifoData, fifoData + sizeof(fifoData));
            ps2x::asyncvif::enqueue(*this, std::move(items));
            return;
        }
        processVIF1Data(fifoData, sizeof(fifoData));
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

namespace
{
    // INTC_STAT (0x1000F000): interrupt sources latch their bit, the guest clears bits by writing 1s (W1C). Games that
    // poll it (e.g. libkernel's VSync wait: write 4, then spin until bit 2 is set) need real latch semantics; a plain
    // storage register reads back the 4 just written and the wait falls through immediately.
    constexpr uint32_t kIntcStatAddress = 0x1000F000u;
    std::atomic<uint32_t> g_intcStat{0u};
}

void ps2xRaiseIntcStat(uint32_t bits) noexcept
{
    g_intcStat.fetch_or(bits, std::memory_order_acq_rel);
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    ps2xEeFlushDeferredCycles(); // the exact EE cycle for timers, DMA and the scheduler (EeScheduler::checkpointDue)
    if (address == kIntcStatAddress)
    {
        g_intcStat.fetch_and(~value, std::memory_order_acq_rel);
        return true;
    }
    if (isAsyncVifRegister(address))
        ps2x::asyncvif::sync(*this);
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimerBacklog &backlog = eeTimerBacklog(this);
        backlog.undelivered |= flushEeTimers();
        struct BudgetRefresh
        {
            PS2Memory &memory;
            ~BudgetRefresh() { memory.m_eeTimerEventBudget = memory.computeCyclesUntilNextEeTimerInterrupt(); }
        } refresh{*this};
        EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) & ~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            if ((timer.mode & kEeTimerModeCue) == 0u || (timer.mode & kEeTimerModeOvfe) == 0u)
            {
                backlog.overflows[timerIndex] = 0u;
            }
            if ((timer.mode & kEeTimerModeCue) == 0u || (timer.mode & kEeTimerModeCmpe) == 0u)
            {
                backlog.compares[timerIndex] = 0u;
            }
            if ((timer.mode & kEeTimerModeOvff) == 0u && backlog.overflows[timerIndex] != 0u)
            {
                --backlog.overflows[timerIndex];
                timer.mode |= kEeTimerModeOvff;
                backlog.undelivered |= 1u << timerIndex;
            }
            if ((timer.mode & kEeTimerModeEquf) == 0u && backlog.compares[timerIndex] != 0u)
            {
                --backlog.compares[timerIndex];
                timer.mode |= kEeTimerModeEquf;
                backlog.undelivered |= 1u << timerIndex;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        ps2x::asyncvif::sync(*this);
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs, off, value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            if ((regOff == 0x70u || regOff == 0x90u) && off == 0u)
                ps2x::asyncvif::traceEvent('D', (regOff == 0x90u ? 0x1000u : 0u) | (value & 0x1FFu)); // display flip
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address < 0x10002040 && ipuEnabled())
    {
        ipuAttach(*this);
        ipuTrace("w", address, value);
        const auto started = std::chrono::steady_clock::now();
        if ((address & 0x3Cu) == 0u)
        {
            const uint32_t code = value >> 28;
            if (code == 1u || code == 2u || code == 7u || code == 8u)
                g_ipuLastDecodeNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count(),
                                        std::memory_order_relaxed);
        }
        ps2_ipu_write32(address, value);
        if ((address & 0x3Cu) == 0u)
        {
            ipuCommandStats(value >> 28, started);
            ipuPictureLog(value);
        }
        return true;
    }
    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;

        // Clearing a channel's status also clears a completion the scheduler latched while that channel's cause was
        // disabled (DisableDmac): on hardware it is the same status bit, so EnableDmac after the clear raises nothing.
        // libmpeg's split CSC (0x322148) clears fromIPU's status, adds a handler that issues the second part, enables
        // the cause, then starts the first part. The latched completion of the last per-macroblock BDEC transfer ran
        // that handler first, so the 97-macroblock part converted macroblocks 0-96 into the end of the picture: every
        // FMV frame after the first came out shifted, with a black band (docs/PROGRESS.md §92). PS2X_DMAC_CLEAR_LATCH=0
        // turns it off.
        static const bool clearLatch = [] { const char *v = std::getenv("PS2X_DMAC_CLEAR_LATCH"); return !v || *v != '0'; }();
        if (clearLatch && (value & 0x3FFu) != 0u)
            ps2xEeClearLatchedDmac(value & 0x3FFu);
        return true;
    }

    m_ioRegisters[address] = value;
    if (address == 0x10003020u)
    {
        m_gifStatWritten = true;
    }

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C00u: // VIF1_STAT: only FDR is writable
        {
            const bool fdr = (value & (1u << 23)) != 0u;
            // A download starts here: with MTVU the TRXDIR = 1 packet may still be queued for the GIF thread, and the
            // FIFO would stay empty forever (the save-load dialog hung on its VRAM download). Drain it first.
            if (fdr && !g_vif1Fdr.load(std::memory_order_acquire))
                ps2x::asyncvif::sync(*this);
            if (g_vif1Fdr.exchange(fdr, std::memory_order_acq_rel) && !fdr)
            {
                std::lock_guard<std::mutex> lock(g_vif1DownloadMutex);
                g_vif1DownloadFifo.clear();
                g_vif1DownloadActive = false;
                g_vif1DownloadSeen = g_gsDownloadSerial.load(std::memory_order_acquire);
            }
            break;
        }
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                const bool wasPath3Masked = m_path3Masked;
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_path3Masked = false;
                if (wasPath3Masked)
                    flushMaskedPath3Packets();
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);

            // VIF1 with CHCR.DIR = 0 runs the other way: GS -> VIF1 FIFO -> memory (libgraph's store-image reads a
            // VRAM download this way, after TRXDIR = 1). The runtime used to treat the destination buffer as VIF
            // codes, which fed garbage to VIF1 and stopped every later frame (Urban Reign's story-mode fighter
            // select froze on its one download). PS2X_VIF1_DOWNLOAD=0 disables it together with the FIFO reads.
            if (channelBase == 0x10009000u && (value & 0x1u) == 0u && ((value >> 2) & 0x3u) == 0u &&
                vif1DownloadEnabled())
            {
                ps2x::asyncvif::sync(*this); // the GS must have seen TRXDIR = 1 (see VIF1_STAT.FDR)
                const uint32_t bytes = (qwc & 0xFFFFu) * 16u;
                const uint32_t dst = madr & 0x7FFFFFF0u;
                uint32_t produced = 0u;
                uint32_t offset = 0u;
                bool inRam = true;
                try
                {
                    offset = translateAddress(dst);
                }
                catch (const std::exception &)
                {
                    inRam = false;
                }
                if (inRam && !isScratchpad(dst) && offset <= PS2_RAM_SIZE && bytes <= PS2_RAM_SIZE - offset)
                {
                    std::lock_guard<std::mutex> lock(g_vif1DownloadMutex);
                    // Bytes already in the FIFO first, then the rest straight from the GS in one request (the FIFO
                    // refills 256 bytes at a time, one GS-worker round trip each: ~840 per 320x224 download).
                    refillVif1DownloadFifo();
                    while (produced < bytes && !g_vif1DownloadFifo.empty())
                    {
                        m_rdram[offset + produced++] = g_vif1DownloadFifo.front();
                        g_vif1DownloadFifo.pop_front();
                    }
                    if (produced < bytes && g_vif1DownloadActive && g_localToHostGs)
                    {
                        const uint32_t got = g_localToHostGs->consumeLocalToHostBytes(m_rdram + offset + produced, bytes - produced);
                        produced += got;
                        if (produced < bytes)
                            g_vif1DownloadActive = false; // GS side drained
                    }
                    if (produced < bytes)
                        std::memset(m_rdram + offset + produced, 0, bytes - produced);
                    markModified(offset, bytes);
                }
                if (const char *v = std::getenv("PS2X_GS_DOWNLOAD_LOG"); v && *v == '1')
                    std::fprintf(stderr, "[gs-download] VIF1 DMA GS->MEM madr=0x%x qwc=0x%x: %u of %u bytes from the GS\n",
                                 dst, qwc & 0xFFFFu, produced, bytes);
                m_ioRegisters[channelBase + 0x10u] = (dst + bytes) & 0x7FFFFFF0u;
                m_ioRegisters[channelBase + 0x20u] = 0u;
                completeDmacChannel(channelBase, 1u);
                return true;
            }

            if ((channelBase == kToIpuChannel || channelBase == kFromIpuChannel) && ipuEnabled())
            {
                ipuAttach(*this);
                ipuTrace("dma", channelBase, value);
                ipuTrace("dma-madr", m_ioRegisters[channelBase + 0x10u], m_ioRegisters[channelBase + 0x20u]);
                if (static const bool picLog = std::getenv("PS2X_IPU_PICLOG") != nullptr; picLog)
                    std::fprintf(stderr, "[ipu-pic] dma %s chcr %08x madr %08x qwc %x\n",
                                 channelBase == kToIpuChannel ? "toIPU" : "fromIPU", value,
                                 m_ioRegisters[channelBase + 0x10u], m_ioRegisters[channelBase + 0x20u]);
                const auto started = std::chrono::steady_clock::now();
                if (channelBase == kToIpuChannel)
                {
                    g_ipuTagEnd = false;
                    g_ipuPullLeft = 8u; // the IPU input FIFO: the rest is pulled by the commands that need it
                    ipuToAdvance(*this);
                    g_ipuPullLeft = 0u;
                }
                else
                    ipuFromAdvance(*this);
                ipuCommandStats(channelBase == kToIpuChannel ? 14u : 15u, started);
                return true;
            }

            if (channelBase == 0x10009000u)
                ps2x::asyncvif::vu1Kick();

            if (tryProcessScratchpadDma(channelBase, value))
            {
                return true;
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) && (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    const int kMaxChainTags = 1 << 20;
                    static thread_local size_t chainSizeHint = 0u;
                    std::vector<uint8_t> chainBuf = takeChainBuffer();
                    chainBuf.clear();
                    chainBuf.reserve(chainSizeHint);

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            bytes -= chunk;
                            src += chunk;
                        }
                    };

                    auto appendVifTagData = [&](uint32_t localTagAddr)
                    {
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr);
                        tagPhys = translateAddress(localTagAddr);

                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // CHCR.TTE sends the DMAtag's upper 64 bits to the channel before
                        // the tag payload. VIF chains use those bytes for two VIFcodes.
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                    };

                    const bool isVifChannel =
                        channelBase == 0x10009000u || channelBase == 0x10008000u;
                    const bool transferTagData = isVifChannel && (chcr & 0x40u) != 0u;

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const bool tagInSPR = isScratchpad(tagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(tagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (transferTagData)
                            appendVifTagData(currentTagAddr);

                        if (hasPayload)
                            appendData(dataAddr, tagQwc);
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                            break;
                    }

                    if (tagsProcessed >= kMaxChainTags)
                    {
                        std::fprintf(stderr, "[dma] chain on 0x%08x stopped after %d tags at 0x%08x\n", channelBase, tagsProcessed, tagAddr);
                        std::fflush(stderr);
                    }
                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    chainSizeHint = std::max(chainSizeHint, chainBuf.size());
                    if (!chainBuf.empty())
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.chainData = std::move(chainBuf);
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

// Scratchpad DMA in interleave mode (CHCR.MOD = 2, only fromSPR/toSPR have it): TQWC qwords are moved, then MADR
// skips SQWC more (D_SQWC), until QWC is done; the scratchpad side is contiguous. As PCSX2's _SPR0/1interleave. Sofdec
// uses it to fetch the reference macroblocks of P/B pictures into the scratchpad (without it the FMVs were green:
// prediction from zeros). PS2X_SPR_INTERLEAVE=0 turns it off.
namespace
{
    bool sprInterleaveEnabled()
    {
        static const bool enabled = [] { const char *v = std::getenv("PS2X_SPR_INTERLEAVE"); return !v || *v != '0'; }();
        return enabled;
    }

    bool processScratchpadInterleave(PS2Memory &memory, uint32_t channelBase)
    {
        constexpr uint32_t kSprFromChannel = 0x1000D000u;
        constexpr uint32_t kDSqwc = 0x1000E030u;
        auto &regs = memory.m_ioRegisters;
        const bool fromScratchpad = channelBase == kSprFromChannel;
        uint32_t qwc = regs[channelBase + 0x20u] & 0xFFFFu;
        uint32_t madr = regs[channelBase + 0x10u] & 0x7FFFFFF0u;
        uint32_t sadr = regs[channelBase + 0x80u] & 0x3FF0u;
        const uint32_t sqwcReg = regs.count(kDSqwc) ? regs[kDSqwc] : 0u;
        const uint32_t sqwc = sqwcReg & 0xFFu;
        uint32_t tqwc = (sqwcReg >> 16u) & 0xFFu;
        if (tqwc == 0u)
            tqwc = qwc;
        while (qwc != 0u)
        {
            const uint32_t n = std::min(tqwc, qwc);
            uint32_t offset = 0u;
            try
            {
                offset = memory.translateAddress(madr);
            }
            catch (const std::exception &)
            {
                return false;
            }
            if (offset > PS2_RAM_SIZE || n * 16u > PS2_RAM_SIZE - offset)
                return false;
            for (uint32_t i = 0u; i < n; ++i)
            {
                uint8_t *ram = memory.getRDRAM() + offset + i * 16u;
                uint8_t *spr = memory.getScratchpad() + ((sadr + i * 16u) & (PS2_SCRATCHPAD_SIZE - 16u));
                if (fromScratchpad)
                    std::memcpy(ram, spr, 16u);
                else
                    std::memcpy(spr, ram, 16u);
            }
            if (fromScratchpad)
                memory.markModified(offset, n * 16u);
            qwc -= n;
            sadr = (sadr + n * 16u) & (PS2_SCRATCHPAD_SIZE - 16u);
            madr += (n + sqwc) * 16u;
        }
        regs[channelBase + 0x10u] = madr & 0x7FFFFFF0u;
        regs[channelBase + 0x20u] = 0u;
        regs[channelBase + 0x80u] = sadr;
        memory.completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
        return true;
    }
}

// toSPR in source chain mode (CHCR.MOD = 1): DMA tags in main memory, their data copied to the scratchpad at SADR.
// Sofdec fetches the reference blocks of P/B pictures this way. The chain ends at refe/end, at ret with an empty
// stack, or at a tag with IRQ when CHCR.TIE is set.
namespace
{
    bool processToScratchpadChain(PS2Memory &memory, uint32_t channelBase, uint32_t chcr)
    {
        auto &regs = memory.m_ioRegisters;
        uint32_t tadr = regs[channelBase + 0x30u] & 0x7FFFFFF0u;
        uint32_t asr[2] = {regs[channelBase + 0x40u], regs[channelBase + 0x50u]};
        uint32_t asp = (chcr >> 4u) & 3u;
        uint32_t sadr = regs[channelBase + 0x80u] & 0x3FF0u;
        uint32_t madr = regs[channelBase + 0x10u] & 0x7FFFFFF0u;
        uint32_t lastTag = 0u;
        const bool tie = (chcr & 0x80u) != 0u;
        // Data still pending from a previous start (QWC != 0) goes first, from MADR.
        uint32_t pendingQwc = regs[channelBase + 0x20u] & 0xFFFFu;
        auto copyToSpr = [&](uint32_t address, uint32_t qwc) -> bool
        {
            for (uint32_t i = 0u; i < qwc; ++i)
            {
                uint32_t offset = 0u;
                try
                {
                    offset = memory.translateAddress((address + i * 16u) & 0x7FFFFFF0u);
                }
                catch (const std::exception &)
                {
                    return false;
                }
                if (offset > PS2_RAM_SIZE - 16u)
                    return false;
                std::memcpy(memory.getScratchpad() + ((sadr + i * 16u) & (PS2_SCRATCHPAD_SIZE - 16u)), memory.getRDRAM() + offset, 16u);
            }
            sadr = (sadr + qwc * 16u) & (PS2_SCRATCHPAD_SIZE - 16u);
            return true;
        };
        if (pendingQwc != 0u && !copyToSpr(madr, pendingQwc))
            return false;
        madr += pendingQwc * 16u;
        bool done = false;
        for (int tags = 0; !done && tags < 0x10000; ++tags)
        {
            uint32_t tagOffset = 0u;
            try
            {
                tagOffset = memory.translateAddress(tadr);
            }
            catch (const std::exception &)
            {
                return false;
            }
            if (tagOffset > PS2_RAM_SIZE - 16u)
                return false;
            uint64_t tag = 0u;
            std::memcpy(&tag, memory.getRDRAM() + tagOffset, sizeof(tag));
            lastTag = static_cast<uint32_t>(tag);
            const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
            const uint32_t id = static_cast<uint32_t>((tag >> 28u) & 7u);
            const bool irq = ((tag >> 31u) & 1u) != 0u;
            const uint32_t addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFF0u);
            uint32_t data = tadr + 16u;
            switch (id)
            {
            case 0: // refe
                data = addr;
                tadr += 16u;
                done = true;
                break;
            case 1: // cnt
                tadr = data + qwc * 16u;
                break;
            case 2: // next
                tadr = addr;
                break;
            case 3: // ref
            case 4: // refs
                data = addr;
                tadr += 16u;
                break;
            case 5: // call
                if (asp < 2u)
                    asr[asp++] = data + qwc * 16u;
                tadr = addr;
                break;
            case 6: // ret
                if (asp > 0u)
                    tadr = asr[--asp];
                else
                    done = true;
                break;
            default: // end
                done = true;
                break;
            }
            if (!copyToSpr(data, qwc))
                return false;
            madr = data + qwc * 16u;
            if (irq && tie)
                done = true;
        }
        regs[channelBase + 0x10u] = madr & 0x7FFFFFF0u;
        regs[channelBase + 0x20u] = 0u;
        regs[channelBase + 0x30u] = tadr & 0x7FFFFFF0u;
        regs[channelBase + 0x40u] = asr[0];
        regs[channelBase + 0x50u] = asr[1];
        regs[channelBase + 0x80u] = sadr;
        regs[channelBase] = (chcr & 0x0000FFCFu) | (asp << 4u) | (lastTag & 0xFFFF0000u);
        memory.completeDmacChannel(channelBase, 9u);
        return true;
    }
}

bool PS2Memory::tryProcessScratchpadDma(uint32_t channelBase, uint32_t chcr)
{
    static constexpr uint32_t kSprFromChannel = 0x1000D000u;
    static constexpr uint32_t kSprToChannel = 0x1000D400u;
    if (channelBase != kSprFromChannel && channelBase != kSprToChannel)
        return false;

    const uint32_t mode = (chcr >> 2u) & 0x3u;
    if (mode == 2u && sprInterleaveEnabled())
        return processScratchpadInterleave(*this, channelBase);
    if (mode == 1u && channelBase == kSprToChannel && sprInterleaveEnabled())
        return processToScratchpadChain(*this, channelBase, chcr);
    if (mode != 0u)
    {
        static int said = 0;
        if (said++ < 4)
            std::fprintf(stderr, "[dma] scratchpad channel 0x%08x mode %u not handled (chcr 0x%08x)\n", channelBase, mode, chcr);
        return false;
    }

    const uint32_t qwc = m_ioRegisters[channelBase + 0x20u] & 0xFFFFu;
    const uint32_t byteCount = qwc * 16u;
    const uint32_t originalMadr = m_ioRegisters[channelBase + 0x10u] & 0x7FFFFFF0u;
    const uint32_t originalSadr = m_ioRegisters[channelBase + 0x80u] & 0x3FF0u;

    uint32_t mainOffset = 0u;
    try
    {
        mainOffset = translateAddress(originalMadr);
    }
    catch (const std::exception &)
    {
        return false;
    }

    if (mainOffset > PS2_RAM_SIZE || byteCount > PS2_RAM_SIZE - mainOffset)
        return false;

    const bool fromScratchpad = channelBase == kSprFromChannel;
    uint32_t scratchOffset = originalSadr;
    uint32_t bytesLeft = byteCount;
    uint32_t copied = 0u;
    while (bytesLeft != 0u)
    {
        const uint32_t scratchChunk = PS2_SCRATCHPAD_SIZE - scratchOffset;
        const uint32_t chunk = std::min(bytesLeft, scratchChunk);
        if (fromScratchpad)
        {
            std::memcpy(m_rdram + mainOffset + copied, m_scratchpad + scratchOffset, chunk);
            markModified(mainOffset + copied, chunk);
        }
        else
        {
            std::memcpy(m_scratchpad + scratchOffset, m_rdram + mainOffset + copied, chunk);
        }

        copied += chunk;
        bytesLeft -= chunk;
        scratchOffset = (scratchOffset + chunk) & (PS2_SCRATCHPAD_SIZE - 1u);
    }

    m_ioRegisters[channelBase + 0x10u] = (originalMadr + byteCount) & 0x7FFFFFF0u;
    m_ioRegisters[channelBase + 0x20u] = 0u;
    m_ioRegisters[channelBase + 0x80u] = (originalSadr + byteCount) & 0x3FF0u;
    completeDmacChannel(channelBase, fromScratchpad ? 8u : 9u);
    return true;
}

void PS2Memory::completeDmacChannel(uint32_t channelBase, uint32_t cause)
{
    static constexpr uint32_t kDStat = 0x1000E010u;
    m_ioRegisters[channelBase] &= ~0x100u;

    uint32_t dstat = m_ioRegisters.count(kDStat) ? m_ioRegisters[kDStat] : 0u;
    dstat |= 1u << cause;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[kDStat] = dstat;
    queueCompletedDmacCause(cause);
}

void PS2Memory::processPendingTransfers()
{
    const bool asyncVif = ps2x::asyncvif::active(*this);
    std::vector<ps2x::asyncvif::Item> asyncItems;
    auto asyncCopy = [&](ps2x::asyncvif::ItemKind kind, const uint8_t *data, uint32_t size)
    {
        ps2x::asyncvif::Item item;
        item.kind = kind;
        item.data = ps2x::asyncvif::takeBuffer();
        item.data.assign(data, data + size);
        asyncItems.push_back(std::move(item));
    };
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint32_t observedGifQwc = 0u;
    for (const auto &transfer : m_pendingGifTransfers)
    {
        const uint64_t transferQwc = !transfer.chainData.empty()
                                         ? (transfer.chainData.size() / 16u)
                                         : transfer.qwc;
        observedGifQwc = static_cast<uint32_t>(std::min<uint64_t>(16u, static_cast<uint64_t>(observedGifQwc) + transferQwc));
    }
    if (observedGifQwc != 0u)
    {
        constexpr uint32_t kGifStat = 0x10003020u;
        constexpr uint32_t kGifFqcMask = 0x1F000000u;
        uint32_t &gifStat = m_ioRegisters[kGifStat];
        gifStat = (gifStat & ~kGifFqcMask) | (observedGifQwc << 24u);
        m_gifStatWritten = true;
    }

    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        if (!p.chainData.empty())
        {
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            if (asyncVif)
            {
                ps2x::asyncvif::Item item;
                item.kind = ps2x::asyncvif::ItemKind::GifPath3;
                item.data = std::move(p.chainData);
                asyncItems.push_back(std::move(item));
            }
            else
                submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    if (asyncVif)
                        asyncCopy(ps2x::asyncvif::ItemKind::GifPath3, m_scratchpad + srcPhys, chunk);
                    else
                        submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    if (asyncVif)
                        asyncCopy(ps2x::asyncvif::ItemKind::GifPath3, m_rdram + srcPhys, chunk);
                    else
                        submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    for (auto &transfer : m_pendingGifTransfers)
        recycleChainBuffer(std::move(transfer.chainData));
    m_pendingGifTransfers.clear();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    for (auto &transfer : m_pendingVif0Transfers)
        recycleChainBuffer(std::move(transfer.chainData));
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    for (auto &p : m_pendingVif1Transfers)
    {
        if (!p.chainData.empty())
        {
            if (asyncVif)
            {
                ps2x::asyncvif::Item item;
                item.kind = ps2x::asyncvif::ItemKind::Vif1;
                item.data = std::move(p.chainData);
                asyncItems.push_back(std::move(item));
            }
            else
                processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    if (asyncVif)
                        asyncCopy(ps2x::asyncvif::ItemKind::Vif1, m_scratchpad + srcPhys, chunk);
                    else
                        processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    if (asyncVif)
                        asyncCopy(ps2x::asyncvif::ItemKind::Vif1, m_rdram + srcPhys, chunk);
                    else
                        processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    for (auto &transfer : m_pendingVif1Transfers)
        recycleChainBuffer(std::move(transfer.chainData));
    m_pendingVif1Transfers.clear();

    if (asyncVif)
        ps2x::asyncvif::enqueue(*this, std::move(asyncItems));
    else if (m_gifArbiter)
        m_gifArbiter->drain();

    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;
    static constexpr uint32_t VIF0_CHANNEL = 0x10008000;
    static constexpr uint32_t VIF1_CHANNEL = 0x10009000;
    static constexpr uint32_t D_STAT = 0x1000E010u;

    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= (1u << channelBit);

        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= (1u << 31);
        else
            dstat &= ~(1u << 31);

        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u); // GIF channel
        queueCompletedDmacCause(2u);
        m_ioRegisters[GIF_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[GIF_CHANNEL + 0x20] = 0;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u); // VIF0 channel
        queueCompletedDmacCause(0u);
        m_ioRegisters[VIF0_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF0_CHANNEL + 0x20] = 0;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u); // VIF1 channel
        queueCompletedDmacCause(1u);
        m_ioRegisters[VIF1_CHANNEL + 0x00] &= ~0x100u;
        m_ioRegisters[VIF1_CHANNEL + 0x20] = 0;
    }
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(cause);
}

std::vector<uint32_t> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<uint32_t> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

extern thread_local int g_gsTracePath; // gs_frontend.cpp, for PS2X_GS_TRACE
extern uint32_t g_gsTraceSrc;

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;
    struct TracePath
    {
        int saved;
        explicit TracePath(int path) : saved(g_gsTracePath) { g_gsTracePath = path; }
        ~TracePath() { g_gsTracePath = saved; }
    } tracePath(static_cast<int>(pathId));
    if (data >= m_rdram && data < m_rdram + PS2_RAM_SIZE)
        g_gsTraceSrc = static_cast<uint32_t>(data - m_rdram);
    else if (data >= m_scratchpad && data < m_scratchpad + PS2_SCRATCHPAD_SIZE)
        g_gsTraceSrc = 0x70000000u + static_cast<uint32_t>(data - m_scratchpad);
    else
        g_gsTraceSrc = 0xFFFFFFFFu;
    // PS2X_GIF_SUBMIT_LOG=<n>: log the first n scratchpad-sourced GIF packets (size + whole packet, up to 512 bytes).
    static uint32_t submitLogLeft = []
    {
        const char *value = std::getenv("PS2X_GIF_SUBMIT_LOG");
        return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    if (submitLogLeft != 0u && (g_gsTraceSrc & 0xF0000000u) == 0x70000000u)
    {
        --submitLogLeft;
        std::string line = "[gif-submit] path=" + std::to_string(static_cast<int>(pathId)) + " src=" +
                           std::to_string(g_gsTraceSrc) + " bytes=" + std::to_string(sizeBytes);
        char hex[48];
        for (uint32_t q = 0; q + 16u <= sizeBytes && q < 512u; q += 16u)
        {
            uint32_t w[4];
            std::memcpy(w, data + q, 16);
            std::snprintf(hex, sizeof(hex), " | %08x %08x %08x %08x", w[0], w[1], w[2], w[3]);
            line += hex;
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter && drainImmediately && m_gifArbiter->empty() && ps2x::asyncvif::submitGif(data, sizeBytes))
        return;
    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    if (ps2x::asyncvif::active(*this))
        return false;
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return false;
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return false;

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return false;

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return false;
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return false;

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    if (gifTagFlg(imageTagLo) != GIF_FMT_IMAGE)
        return false;

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return false;

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return false;
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return false;
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return false;
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    if (ps2x::asyncvif::active(*this))
        return false;
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    g_gsTracePath = 30; // native packed PATH3 DMA
    const bool nativeOk = gs.processNativePackedGIFPacket(payload, payloadBytes);
    g_gsTracePath = -1;
    if (!nativeOk)
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] = ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= (1u << 2u);
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= (1u << 31u);
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u);
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    ps2xEeFlushDeferredCycles(); // the exact EE cycle for timer counts (EeScheduler::checkpointDue)
    // PS2X_IO_READ_HIST=1: the 12 most read I/O registers every 2^22 reads (finds guest polling loops).
    static const bool readHist = [] { const char *v = std::getenv("PS2X_IO_READ_HIST"); return v && *v == '1'; }();
    if (readHist)
    {
        static std::unordered_map<uint32_t, uint64_t> hist;
        static uint64_t total = 0u;
        ++hist[address];
        if ((++total & 0x3FFFFFu) == 0u)
        {
            std::vector<std::pair<uint64_t, uint32_t>> top;
            for (const auto &[reg, n] : hist)
                top.emplace_back(n, reg);
            std::sort(top.rbegin(), top.rend());
            std::fprintf(stderr, "[io-read-hist]");
            for (size_t i = 0; i < top.size() && i < 12u; ++i)
                std::fprintf(stderr, " %08x=%.1f%%", top[i].second, 100.0 * top[i].first / 0x400000);
            std::fputc('\n', stderr);
            hist.clear();
        }
    }
    if (address == 0x10003C00u && vif1DownloadEnabled() && g_localToHostGs && vif1DownloadMayHaveData())
    {
        uint32_t fqc = 0u;
        {
            std::lock_guard<std::mutex> lock(g_vif1DownloadMutex);
            fqc = refillVif1DownloadFifo();
        }
        if (fqc != 0u)
        {
            const uint32_t base = m_ioRegisters.count(address) ? m_ioRegisters[address] : vif1_regs.stat;
            return (base & ~0x1F000000u) | (std::min<uint32_t>(fqc, 16u) << 24) | (1u << 23);
        }
    }
    if (address == kIntcStatAddress)
        return g_intcStat.load(std::memory_order_acquire);
    if (address == 0x10009000u && ps2x::asyncvif::vu1Busy(*this))
        return (m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u) | 0x100u;
    if (address == 0x1000A000u && gifStillRunningOnGs(*this))
        return (m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u) | 0x100u;
    if (isAsyncVifRegister(address))
        ps2x::asyncvif::sync(*this);
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const uint32_t interrupts = flushEeTimers();
        if (interrupts != 0u)
        {
            eeTimerBacklog(this).undelivered |= interrupts;
            m_eeTimerEventBudget = 0u;
        }
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        ps2x::asyncvif::sync(*this);
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((csrForRead(gs_regs.csr.load()) >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address < 0x10002040 && ipuEnabled())
    {
        ipuAttach(*this);
        const uint32_t value = ps2_ipu_read32(address);
        ipuTrace("r", address, value);
        return value;
    }
    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }

    if (address == 0x10003020u) // GIF_STAT
    {
        uint32_t stat = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        const uint32_t mode = m_ioRegisters.count(0x10003010u) ? m_ioRegisters[0x10003010u] : 0u;
        const uint32_t ctrl = m_ioRegisters.count(0x10003000u) ? m_ioRegisters[0x10003000u] : 0u;

        // M3R and IMT mirror GIF_MODE, PSE mirrors GIF_CTRL, and M3P is the
        // effective PATH3 mask controlled by the VIF1 MSKPATH3 command.
        stat = (stat & ~0xFu) |
               (mode & 0x1u) |
               (m_path3Masked ? 0x2u : 0u) |
               (mode & 0x4u) |
               (ctrl & 0x8u);
        return stat;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}

// Called by the GS frontend for every SIGNAL register write (see the SIGNAL stall notes above).
void ps2xGsSignal(GSRegisters &gs, uint32_t id, uint32_t mask)
{
    std::lock_guard<std::mutex> lock(g_gsSignalMutex);
    if (gsSignalBlocked(gs) || !g_gsSignalQueue.empty())
    {
        g_gsSignalQueue.push_back({id, mask, false});
        if (gsSignalTraceAll())
            std::fprintf(stderr, "[gs-sig2] queue id=%x mask=%x depth=%zu\n", id, mask, g_gsSignalQueue.size());
        return;
    }
    applyGsSignal(gs, id, mask);
}

// Called by the GS frontend for FINISH: delivered after any SIGNALs still queued ahead of it, and after a SIGNAL that
// is still pending. The runtime runs GIF DMA synchronously, so a list the GS SIGNAL handler kicks would otherwise post
// its FINISH before that handler acknowledges the SIGNAL with a read-modify-write of CSR, which clears FINISH too. On
// hardware the transfer takes time and the FINISH arrives as its own interrupt.
void ps2xGsFinish(GSRegisters &gs)
{
    std::lock_guard<std::mutex> lock(g_gsSignalMutex);
    if (!g_gsSignalQueue.empty() || gsSignalBlocked(gs))
    {
        g_gsSignalQueue.push_back({0u, 0u, true});
        if (gsSignalTraceAll())
            std::fprintf(stderr, "[gs-sig2] queue finish depth=%zu\n", g_gsSignalQueue.size());
        return;
    }
    setGsFinish(gs);
    if (gsSignalTraceAll())
        std::fprintf(stderr, "[gs-sig2] finish\n");
}

// Save states: queued SIGNAL/FINISH deliveries are not part of the state (see ps2xGsStallActive in gs_frontend.cpp).
bool ps2xGsSignalQueuePending()
{
    std::lock_guard<std::mutex> lock(g_gsSignalMutex);
    return !g_gsSignalQueue.empty();
}

void ps2xGsResetSignalState()
{
    std::lock_guard<std::mutex> lock(g_gsSignalMutex);
    g_gsSignalQueue.clear();
    g_gsHandlerBusy.store(false, std::memory_order_release);
}

// The GS front end stalls GIF data at a SIGNAL while this is true (see g_gsHandlerBusy).
bool ps2xGsSignalBlocked(const GSRegisters &gs)
{
    return gsSignalBlocked(gs);
}

void ps2xGsSetHandlerBusy(GSRegisters &gs, bool busy)
{
    g_gsHandlerBusy.store(busy, std::memory_order_release);
    if (!busy)
        releaseQueuedGsSignal(gs);
}

uint32_t ps2xGsFinishSerial() noexcept
{
    return g_gsFinishSerial.load(std::memory_order_acquire);
}

uint32_t ps2xGsSignalSerial() noexcept
{
    return g_gsSignalSerial.load(std::memory_order_acquire);
}

// The serial counters themselves, for EeScheduler::accountCycles to load without a call per checkpoint.
const std::atomic<uint32_t> *ps2xGsSignalSerialAddress() noexcept
{
    return &g_gsSignalSerial;
}

const std::atomic<uint32_t> *ps2xGsFinishSerialAddress() noexcept
{
    return &g_gsFinishSerial;
}
