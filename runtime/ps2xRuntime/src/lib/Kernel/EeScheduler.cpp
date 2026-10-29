#include "runtime/ee_scheduler.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_pnach.h"
#include "GuestKernelMemory.h"

// Defined in ps2_memory.cpp: latches bits in INTC_STAT (W1C register).
void ps2xRaiseIntcStat(uint32_t bits) noexcept;
// Defined in ps2_memory.cpp: number of GS SIGNALs delivered so far.
uint32_t ps2xGsSignalSerial() noexcept;
bool ps2xIpuActiveRecently() noexcept; // ps2_memory.cpp
uint32_t ps2xGsFinishSerial() noexcept;
const std::atomic<uint32_t> *ps2xGsSignalSerialAddress() noexcept;
const std::atomic<uint32_t> *ps2xGsFinishSerialAddress() noexcept;
// host_vsync.cpp: the host display's last vblank and refresh period.
bool ps2xHostVsync(std::chrono::steady_clock::time_point &lastVblank, std::chrono::nanoseconds &period) noexcept;

namespace
{
    // Last SIGNAL/FINISH delivery serials the scheduler dispatched (ps2_memory.cpp). A change makes accountCycles
    // request a checkpoint even when the CSR bits read the same as when last latched (cleared and set again by the
    // GIF thread in between), and dispatchGsInterrupt raises it as a new interrupt.
    uint32_t g_seenSignalSerial = 0u;
    uint32_t g_seenFinishSerial = 0u;

    // checkpointDue's deferred accounting (PS2X_EE_DEFER_CYCLES): EE thread only.
    int g_deferCycles = -1;
    bool g_inlineCheckpoint = true; // PS2X_EE_INLINE_CHECKPOINT=0: the budget stays 0 (every checkpoint calls in)
    uint64_t g_deferUntil = 0u; // EE cycle up to which checkpoints may only add to g_ps2xEeDeferred
    EeScheduler *g_deferScheduler = nullptr;
    thread_local bool t_onEeThread = false;

    // accountCycles (PS2X_EE_FAST_ACCOUNT, PS2X_EE_GS_POLL_CYCLES): game thread only.
    int g_fastAccount = -1;
    uint64_t g_gsPollCycles = 256u;
    uint64_t g_gsPollStash = 0u;
    uint64_t g_iopStash = 0u;
    const std::atomic<uint32_t> *g_signalSerial = nullptr;
    const std::atomic<uint32_t> *g_finishSerial = nullptr;

    // The pacing wait of processDueDeadlines (published there): the GIF thread wakes it when it raises SIGNAL/FINISH.
    std::atomic<std::mutex *> g_pacingMutex{nullptr};
    std::atomic<std::condition_variable *> g_pacingCv{nullptr};

    // Host time of the deadline processDueDeadlines took off the checkpoints for an interrupt handler (zero: none).
    std::chrono::steady_clock::time_point g_deadlinesDeferredUntil{};

    bool hasInterruptInvocation(const std::deque<GuestInvocation> &pending, const GuestThread *running)
    {
        for (const GuestInvocation &invocation : pending)
            if (invocation.kind == GuestInvocationKind::Interrupt)
                return true;
        if (running)
            for (const GuestInvocation &invocation : running->invocations)
                if (invocation.kind == GuestInvocationKind::Interrupt)
                    return true;
        return false;
    }

    // PS2X_VSYNC_LOCK (default on): phase- and frequency-lock the emulated vblank (real-time side only; emulated cycles are
    // untouched) to the host display, when its refresh is within 2% of the emulated field rate or of a whole multiple of
    // it (120/180/240 Hz: every 2nd/3rd/4th host vblank, §97). Free-running at 59.94 Hz
    // against a 60 Hz monitor, the two edges crossed every ~6-8 s: the field came just after a host vsync, the present
    // missed it (33 ms) and the next image followed 0.6 ms later (PROGRESS §67). Each field the next host deadline is
    // pulled 1/8 of the way (at most 0.25 ms) toward 7 ms after a host vblank (PS2X_VSYNC_PHASE_MS; 1 ms before §96);
    // the 0.25 ms step also absorbs the rate difference (60 / 59.94 -> the game runs 0.1% faster). The windowed swap
    // chain is mailbox (no wait): the compositor takes the latest finished image shortly after each vblank, so a
    // present 1-3 ms after it raced that pickup (~5 images/s held twice and ~5 never shown); at 6-8 ms each image is
    // shown exactly once (tools/present_stats.py).
    // PS2X_VSYNC_LOCK_STATS=1 logs the phase error every ~5 s.
    std::chrono::steady_clock::time_point lockToHostVsync(std::chrono::steady_clock::time_point target) noexcept
    {
        static int s_lock = -1;
        static bool s_stats = false;
        if (s_lock < 0)
        {
            const char *value = std::getenv("PS2X_VSYNC_LOCK");
            const char *stats = std::getenv("PS2X_VSYNC_LOCK_STATS");
            s_stats = stats && *stats == '1';
            s_lock = (value && *value == '0') ? 0 : 1;
        }
        if (s_lock == 0)
            return target;
        std::chrono::steady_clock::time_point lastVblank;
        std::chrono::nanoseconds period{};
        if (!ps2xHostVsync(lastVblank, period))
            return target;
        constexpr int64_t kEmulatedFieldNs = 16683400; // NTSC field (1001/60000 s)
        const int64_t periodNs = period.count();
        // Refreshes per emulated field: 1 on a 60 Hz display, 2/3/4 at 120/180/240 Hz (each image then stays exactly
        // that many refreshes). 0 = no whole multiple within 2% (75, 144, 165 Hz...): those cannot show 60 images a
        // second evenly without VRR, so the clock is left alone.
        int64_t multiple = 0;
        for (int64_t n = 1; n <= 8 && multiple == 0; ++n)
            if (periodNs * n >= kEmulatedFieldNs * 98 / 100 && periodNs * n <= kEmulatedFieldNs * 102 / 100)
                multiple = n;
        static int64_t s_reportedMultiple = -1;
        if (multiple != s_reportedMultiple)
        {
            s_reportedMultiple = multiple;
            if (multiple == 0)
                std::fprintf(stderr, "[vsync-lock] host display %.3f Hz: not a multiple of 59.94 Hz, not locked\n",
                             1e9 / static_cast<double>(periodNs));
            else
                std::fprintf(stderr, "[vsync-lock] host display %.3f Hz: emulated vblank locked to it, %lld refresh(es) per field (PS2X_VSYNC_LOCK=0 turns this off)\n",
                             1e9 / static_cast<double>(periodNs), static_cast<long long>(multiple));
        }
        if (multiple == 0)
            return target;
        // Where the emulated vblank (and so the present) sits in the host refresh. PS2X_VSYNC_PHASE_MS=<ms after the
        // host vblank> at 60 Hz (PROGRESS §96); at 120/180/240 Hz the same fraction of the shorter refresh, so the
        // present stays clear of the compositor's pickup after each vblank (§97).
        static const double kTargetPhase60Ns = []
        {
            const char *value = std::getenv("PS2X_VSYNC_PHASE_MS");
            return (value && *value ? std::atof(value) : 7.0) * 1e6;
        }();
        const int64_t kTargetPhaseNs = static_cast<int64_t>(kTargetPhase60Ns * static_cast<double>(periodNs) / static_cast<double>(kEmulatedFieldNs));
        constexpr int64_t kMaxStepNs = 250000;
        int64_t phase = std::chrono::duration_cast<std::chrono::nanoseconds>(target - lastVblank).count() % periodNs;
        if (phase < 0)
            phase += periodNs;
        int64_t error = phase - kTargetPhaseNs;
        if (error > periodNs / 2)
            error -= periodNs;
        else if (error < -periodNs / 2)
            error += periodNs;
        const int64_t step = std::clamp<int64_t>(-error / 8, -kMaxStepNs, kMaxStepNs);
        if (s_stats)
        {
            static uint64_t fields = 0u;
            static double sumAbs = 0.0, worst = 0.0;
            ++fields;
            sumAbs += std::abs(static_cast<double>(error)) / 1e6;
            worst = std::max(worst, std::abs(static_cast<double>(error)) / 1e6);
            if (fields % 300u == 0u)
            {
                std::fprintf(stderr, "[vsync-lock] host period %.4f ms (%.3f Hz), phase error mean %.3f ms, worst %.3f ms\n",
                             static_cast<double>(periodNs) / 1e6, 1e9 / static_cast<double>(periodNs), sumAbs / 300.0, worst);
                sumAbs = 0.0;
                worst = 0.0;
            }
        }
        return target + std::chrono::nanoseconds(step);
    }

    bool gsInterruptUnseen() noexcept
    {
        return ps2xGsSignalSerial() != g_seenSignalSerial || ps2xGsFinishSerial() != g_seenFinishSerial;
    }
}

// checkpointDue's deferred accounting, shared with the inline fast path of PS2Runtime::eeCheckpointDue (ps2_runtime.h):
// cycles charged by checkpoints and not yet accounted, and how far they may grow before a checkpoint must call in
// (g_deferUntil - m_eeCycle, set by the full path; 0 forces the call). The budget is at most PS2X_EE_DEFER_CYCLES, so
// an event posted from another thread waits at most that long even if its zeroing of the budget is overwritten.
uint64_t g_ps2xEeDeferred = 0u;
std::atomic<uint64_t> g_ps2xEeDeferBudget{0u};
uint32_t g_ps2xEeLoopCycles = 32u;

// True while a SIGNAL/FINISH the GS raised has not been through dispatchGsInterrupt yet (EE thread only).
bool ps2xGsInterruptUndelivered() noexcept
{
    return gsInterruptUnseen();
}

// After a save state is loaded: the GS interrupts seen so far and a deferred deadline belong to the session before it.
void ps2xSchedulerResetGsTransientState() noexcept
{
    g_seenSignalSerial = ps2xGsSignalSerial();
    g_seenFinishSerial = ps2xGsFinishSerial();
    g_deadlinesDeferredUntil = {};
    // Cycles checkpointDue deferred before the load belong to the old session.
    g_ps2xEeDeferred = 0u;
    g_deferUntil = 0u;
    g_ps2xEeDeferBudget.store(0u, std::memory_order_relaxed);
}

// With MTVU the GS finishes drawing on the GIF thread while the EE may already sleep at a due vblank deadline,
// waiting for real time. Without a wake-up the guest saw that vblank before the FINISH/SIGNAL of a frame drawn in
// time, missed the buffer swap and ran at half or a third of the rate (menus 60 -> 30, fights 34 -> 20).
void ps2xWakeEeForGsInterrupt() noexcept
{
    std::mutex *mutex = g_pacingMutex.load(std::memory_order_acquire);
    std::condition_variable *cv = g_pacingCv.load(std::memory_order_acquire);
    if (!mutex || !cv)
        return;
    {
        std::lock_guard<std::mutex> lock(*mutex);
    }
    cv->notify_one();
}

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

void ps2xTraceSchedulerDispatch(R5900Context *ctx);

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;
    constexpr int kMaxVBlankCatchUpFields = 2;
    constexpr uint64_t kHostPacingCheckCycles = 65536u;
    // Guest function dispatches so far, and the count at the last host catch-up: a catch-up that is followed by no
    // guest execution means pacing is starving the guest (e.g. a busy IOP eats each skipped field in real time).
    // File-local rather than members so the header (part of every generated file's PCH) stays unchanged.
    uint64_t g_guestDispatches = 0;
    uint64_t g_lastCatchUpDispatches = ~0ull;
    // INTC/DMAC causes that fired while disabled with Disable(Intc|Dmac). Hardware keeps them latched in INTC_STAT /
    // D_STAT and delivers them once the cause is enabled again; dropping them loses interrupts that a handler raised
    // for itself (e.g. a GS SIGNAL released while the GS handler runs with the GS cause disabled).
    uint32_t g_pendingMaskedIntc = 0u;
    uint32_t g_pendingMaskedDmac = 0u;
    std::atomic<bool> s_hostPacingDefault{false};

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    struct SchedulerTrace
    {
        FILE *file = nullptr;
        uint64_t firstTick = 0;
        uint64_t lastTick = 0;
    };

    SchedulerTrace &schedulerTrace()
    {
        static SchedulerTrace trace = []
        {
            SchedulerTrace result;
            const char *value = std::getenv("PS2X_TRACE_SCHED");
            unsigned long long first = 0;
            unsigned long long last = 0;
            char path[512]{};
            if (value && std::sscanf(value, "%llu:%llu:%511[^\n]", &first, &last, path) == 3)
            {
                result.file = std::fopen(path, "w");
                result.firstTick = first;
                result.lastTick = last;
            }
            return result;
        }();
        return trace;
    }

    void traceScheduler(uint64_t tick, uint64_t cycle, const char *format, ...)
    {
        SchedulerTrace &trace = schedulerTrace();
        if (!trace.file || tick < trace.firstTick || tick > trace.lastTick)
        {
            return;
        }
        std::fprintf(trace.file, "%llu %llu ", static_cast<unsigned long long>(tick), static_cast<unsigned long long>(cycle));
        va_list args;
        va_start(args, format);
        std::vfprintf(trace.file, format, args);
        va_end(args);
        std::fputc('\n', trace.file);
        std::fflush(trace.file);
    }

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::setHostPacingDefault(bool enabled) noexcept
{
    s_hostPacingDefault.store(enabled, std::memory_order_relaxed);
}

void EeScheduler::hostCatchUp() noexcept
{
    m_pacingCheckCycle = m_eeCycle + kHostPacingCheckCycles;
    if (m_pacingRefTime == std::chrono::steady_clock::time_point{} || !ps2x::asyncvif::active(m_runtime.memory()))
        return;
    if (ps2x::asyncvif::vblankWaitEnabled())
    {
        // MTVU timing model: no catch-up (the game slows down instead of skipping emulated time), but emulated time may
        // not run ahead of real time while the VU/GIF workers still have work. Otherwise the EE, spinning on the frame-
        // done flag the GS handler sets, burned a whole field of emulated cycles before the render on the other threads
        // finished; the game then saw its frame take a field and missed the next vblank (4-fighter select at 48-58 FPS,
        // frames at +0.01 and +0.97 field). Data the GS deferred at a SIGNAL waits for the EE and does not count as work.
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_pacingRefTime).count();
        if (elapsed <= 0.0)
            return;
        const uint64_t expected = m_pacingRefCycle + static_cast<uint64_t>(elapsed * static_cast<double>(kEeClockHz));
        constexpr uint64_t kSlackCycles = kEeClockHz / 2000u; // 0.5 ms
        if (m_eeCycle <= expected + kSlackCycles || !ps2x::asyncvif::pipelineBusy(m_runtime.memory()))
            return;
        const double aheadUs = static_cast<double>(m_eeCycle - expected) * 1e6 / static_cast<double>(kEeClockHz);
        ps2x::asyncvif::waitPipelineIdle(m_runtime.memory(), std::chrono::microseconds(static_cast<int64_t>(std::min(aheadUs, 1000.0))));
        return;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_pacingRefTime).count();
    if (elapsed <= 0.0)
        return;
    const uint64_t expected = m_pacingRefCycle + static_cast<uint64_t>(elapsed * static_cast<double>(kEeClockHz));
    const uint64_t field = vblankPeriodCycles();
    if (expected <= m_eeCycle + field / 2u)
        return;
    const uint64_t lag = expected - m_eeCycle;
    // Give up on catching up (until the next VBlank re-anchors) when far behind, or when the previous catch-up was
    // not followed by any guest execution. Skipping a field still runs the IOP for that field; if the IOP is busy that
    // costs about a field of real time, the lag never shrinks, and the guest would never run again.
    if (lag > field * 8u || g_lastCatchUpDispatches == g_guestDispatches)
    {
        m_pacingRefTime = {};
        return;
    }
    g_lastCatchUpDispatches = g_guestDispatches;
    consumeCycles(std::min<uint64_t>(lag - field / 4u, field));
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    {
        const char *pacing = std::getenv("PS2X_EE_HOST_PACING");
        m_hostPacing = pacing && *pacing ? *pacing != '0' : s_hostPacingDefault.load(std::memory_order_relaxed);
    }
    m_pacingCheckCycle = 0;
    m_pacingRefTime = {};
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    g_pendingMaskedIntc = 0u;
    g_pendingMaskedDmac = 0u;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_deferTransfers = false;
    m_transferPending = false;
    m_pendingEeTimerInterrupts = 0u;
    m_gsInterruptLatched = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_cycleWaitSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + vblankPeriodCycles(),
                  std::chrono::steady_clock::now() + vblankPeriod(),
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshotNow();
}

namespace
{
    // PS2X_SCHED_STATS=1: once per second, print why the dispatch loop iterated (diagnoses livelocks where guest
    // code never gets to run).
    struct SchedulerLoopStats
    {
        bool enabled = []() { const char *v = std::getenv("PS2X_SCHED_STATS"); return v && *v == '1'; }();
        uint64_t iterations = 0, idleWaits = 0, completions = 0, missing = 0, checkpointSkips = 0, calls = 0, transfers = 0;
        uint32_t lastPc = 0;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

        void maybePrint(uint64_t eeCycle, uint64_t nextDeadline, bool pending, size_t pendingInvocations)
        {
            if (!enabled)
                return;
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(1))
                return;
            last = now;
            std::fprintf(stderr,
                         "[sched-stats] iter=%llu idle=%llu done=%llu missing=%llu ckptSkip=%llu calls=%llu transfers=%llu "
                         "lastPc=0x%x cycle=%llu nextDeadline=%llu ckptPending=%d pendingInv=%zu\n",
                         (unsigned long long)iterations, (unsigned long long)idleWaits, (unsigned long long)completions,
                         (unsigned long long)missing, (unsigned long long)checkpointSkips, (unsigned long long)calls,
                         (unsigned long long)transfers, lastPc, (unsigned long long)eeCycle, (unsigned long long)nextDeadline,
                         pending ? 1 : 0, pendingInvocations);
            iterations = idleWaits = completions = missing = checkpointSkips = calls = transfers = 0;
        }
    };
    SchedulerLoopStats g_loopStats;
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        if (g_loopStats.enabled)
        {
            ++g_loopStats.iterations;
            g_loopStats.maybePrint(m_eeCycle, m_nextDeadlineCycle.load(std::memory_order_acquire),
                                   m_checkpointPending.load(std::memory_order_acquire), m_pendingInvocations.size());
        }
        if (g_deadlinesDeferredUntil != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() >= g_deadlinesDeferredUntil)
        {
            g_deadlinesDeferredUntil = {};
            std::lock_guard lock(m_eventMutex);
            updateNextDeadline();
        }
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }
        if (serviceStateRequests())
        {
            continue;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshotNow();
                ++g_loopStats.idleWaits;
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                ps2x::asyncvif::traceEvent('X', invocation.context.pc);
                ps2x::asyncvif::traceEvent('Y', static_cast<uint32_t>(m_eeCycle >> 10)); // EE cycle / 1024
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->pendingResume.setPc || running->pendingResume.setResult)
        {
            R5900Context &resumed = running->activeContext();
            if (running->pendingResume.setResult)
                setReturnS32(&resumed, running->pendingResume.result);
            if (running->pendingResume.setPc)
                resumed.pc = running->pendingResume.pc;
            running->pendingResume = {};
        }
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshotNow();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                ++g_loopStats.completions;
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                ps2x::asyncvif::traceEvent('x', static_cast<uint32_t>(completed.kind));
                if (g_deadlinesDeferredUntil != std::chrono::steady_clock::time_point{} &&
                    !hasInterruptInvocation(m_pendingInvocations, running))
                {
                    g_deadlinesDeferredUntil = {};
                    std::lock_guard lock(m_eventMutex);
                    updateNextDeadline();
                }
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshotNow();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            ps2x::asyncvif::traceEvent('X', invocation.context.pc);
            ps2x::asyncvif::traceEvent('Y', static_cast<uint32_t>(m_eeCycle >> 10)); // EE cycle / 1024
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            ++g_loopStats.missing;
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        ps2xTraceSchedulerDispatch(&context);
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        g_loopStats.lastPc = context.pc;
        if (checkpointDue(kGuestDispatchCycles))
        {
            ++g_loopStats.checkpointSkips;
            continue;
        }

        try
        {
            ++g_loopStats.calls;
            ++g_guestDispatches;
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            m_unwinding = false;
            loadSharedVu0Random(context);
            function(m_rdram, &context, &m_runtime);
            saveSharedVu0Random(context);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            m_transferPending = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            ++g_loopStats.transfers;
            saveSharedVu0Random(context);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            m_transferPending = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_transferPending = false;
            m_running.store(false, std::memory_order_release);
            publishSnapshotNow();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshotNow();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    g_ps2xEeDeferBudget.store(0u, std::memory_order_relaxed);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
        g_ps2xEeDeferBudget.store(0u, std::memory_order_relaxed);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    // PS2X_EE_DEFER_CYCLES (default 256; 0 = account at every checkpoint as before): the checkpoints of generated code
    // (every backward branch, every call) only add their cycles to g_ps2xEeDeferred while the EE cycle stays short of
    // g_deferUntil, which the full path below sets to the nearest of: 256 cycles, the next deadline, the end of the
    // time slice, the pacing check and the next EE timer event. Nothing due is delayed; asynchronous events (posted by
    // other threads, which set m_checkpointPending) are still seen at once. The total of cycles charged is unchanged.
    // I/O register accesses and syscalls account the deferred cycles first (ps2xEeFlushDeferredCycles), so the game
    // reads its timers at the same cycle as before. The full path was ~7% of the game thread (PROGRESS §71).
    if (g_deferCycles < 0)
    {
        const char *value = std::getenv("PS2X_EE_DEFER_CYCLES");
        g_deferCycles = value && *value ? std::max(0, std::atoi(value)) : 256;
        g_deferScheduler = this;
        const char *inlineValue = std::getenv("PS2X_EE_INLINE_CHECKPOINT");
        g_inlineCheckpoint = !(inlineValue && *inlineValue == '0');
    }
    const uint64_t charge = std::max<uint64_t>(1u, cycles);
    if (g_deferCycles != 0 && m_eeCycle + g_ps2xEeDeferred + charge < g_deferUntil &&
        !m_checkpointPending.load(std::memory_order_acquire) && !m_stopRequested.load(std::memory_order_acquire))
    {
        g_ps2xEeDeferred += charge;
        if (g_inlineCheckpoint)
            g_ps2xEeDeferBudget.store(g_deferUntil - m_eeCycle, std::memory_order_relaxed);
        return false;
    }
    t_onEeThread = true; // the full path runs before any deferral (g_deferUntil starts at 0)
    accountCycles(static_cast<uint32_t>(charge)); // also accounts g_ps2xEeDeferred
    const bool due = [this]() noexcept -> bool
    {
        if (m_hostPacing && m_eeCycle >= m_pacingCheckCycle)
            hostCatchUp();

        if (m_checkpointPending.load(std::memory_order_acquire) ||
            m_stopRequested.load(std::memory_order_acquire))
        {
            m_unwinding = true;
            return true;
        }

        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
        {
            m_checkpointPending.store(true, std::memory_order_release);
            m_unwinding = true;
            return true;
        }

        if (m_eeCycle < m_sliceEndCycle)
        {
            return false;
        }

        const GuestThread *running = currentThread();
        if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
        {
            m_rescheduleRequested = true;
            m_timeSliceExpired = true;
            m_unwinding = true;
            return true;
        }

        renewTimeSlice();
        return false;
    }();
    if (g_deferCycles != 0 && !due)
    {
        uint64_t until = m_eeCycle + static_cast<uint64_t>(g_deferCycles);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (nextEventCycle != 0u)
            until = std::min(until, nextEventCycle);
        until = std::min(until, m_sliceEndCycle);
        if (m_hostPacing)
            until = std::min(until, m_pacingCheckCycle);
        const PS2Memory &memory = m_runtime.memory();
        if (memory.m_gifStatWritten)
            until = m_eeCycle;
        else if (memory.m_eeTimerEventBudget > memory.m_eeTimerPendingCycles)
            until = std::min(until, m_eeCycle + (memory.m_eeTimerEventBudget - memory.m_eeTimerPendingCycles));
        else
            until = m_eeCycle;
        g_deferUntil = until;
    }
    else
        g_deferUntil = 0u;
    g_ps2xEeDeferBudget.store(g_inlineCheckpoint && g_deferUntil > m_eeCycle ? g_deferUntil - m_eeCycle : 0u, std::memory_order_relaxed);
    return due;
}

// I/O register accesses (ps2_memory.cpp) and syscalls (ps2_runtime.cpp): account the cycles checkpointDue deferred, so
// timers, the IOP and the scheduler see the exact EE cycle. Only on the EE thread (the only one that defers).
void ps2xEeFlushDeferredCycles() noexcept
{
    if (g_ps2xEeDeferred != 0u && t_onEeThread && g_deferScheduler)
        g_deferScheduler->accountCycles(0u);
}


void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    // Cycles checkpointDue deferred are accounted with these (a flush passes 0 for them alone).
    uint64_t elapsed = (cycles == 0u && g_ps2xEeDeferred != 0u) ? 0u : std::max<uint64_t>(1u, cycles);
    elapsed += g_ps2xEeDeferred;
    g_ps2xEeDeferred = 0u;
    m_eeCycle += elapsed;
    g_ps2xEeDeferBudget.store(0u, std::memory_order_relaxed); // relative to the old cycle; the next checkpoint renews it
    // PS2X_EE_FAST_ACCOUNT=0: the previous per-checkpoint work. Otherwise:
    // - the common case of PS2Memory::advanceEeTimers (add the cycles, still short of the next timer event) is done
    //   here; advanceEeTimers still runs whenever it has work to do, so timer interrupts land on the same cycle;
    // - the GS interrupt state (CSR and the SIGNAL/FINISH serials, written all the time by the GIF/GS threads) is
    //   looked at every PS2X_EE_GS_POLL_CYCLES EE cycles (default 256, ~1 us) instead of at every checkpoint. Those
    //   events arrive from other threads at no fixed EE cycle anyway; the cache line they share was a miss per check.
    // The settings are plain globals (a function-local static would cost a thread-local guard check per call).
    if (g_fastAccount < 0)
    {
        const char *value = std::getenv("PS2X_EE_FAST_ACCOUNT");
        const char *poll = std::getenv("PS2X_EE_GS_POLL_CYCLES");
        g_gsPollCycles = poll && *poll ? std::max<uint64_t>(1u, std::strtoull(poll, nullptr, 10)) : 256u;
        g_signalSerial = ps2xGsSignalSerialAddress();
        g_finishSerial = ps2xGsFinishSerialAddress();
        g_fastAccount = (value && *value == '0') ? 0 : 1;
    }
    if (g_fastAccount != 0)
    {
        PS2Memory &memory = m_runtime.memory();
        if (!memory.m_gifStatWritten && memory.m_eeTimerPendingCycles + elapsed < memory.m_eeTimerEventBudget)
            memory.m_eeTimerPendingCycles += elapsed;
        else
            m_pendingEeTimerInterrupts |= memory.advanceEeTimers(elapsed);
        g_iopStash += elapsed;
        if (g_iopStash >= 512u)
        {
            m_runtime.advanceIopEeCycles(g_iopStash);
            g_iopStash = 0u;
        }
        bool due = m_pendingEeTimerInterrupts != 0u;
        g_gsPollStash += elapsed;
        if (g_gsPollStash >= g_gsPollCycles)
        {
            g_gsPollStash = 0u;
            due = due || pendingGsInterruptEvents() != m_gsInterruptLatched ||
                  g_signalSerial->load(std::memory_order_acquire) != g_seenSignalSerial ||
                  g_finishSerial->load(std::memory_order_acquire) != g_seenFinishSerial;
        }
        if (due)
            m_checkpointPending.store(true, std::memory_order_release);
        return;
    }
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    // The IOP runs in batches of 8192 EE cycles anyway; handing it the cycles every 512 instead of at every
    // checkpoint saves three out-of-line calls per guest basic block (~2% of the game thread in fights).
    static uint64_t iopCycleStash = 0u;
    iopCycleStash += elapsed;
    if (iopCycleStash >= 512u)
    {
        m_runtime.advanceIopEeCycles(iopCycleStash);
        iopCycleStash = 0u;
    }
    if (m_pendingEeTimerInterrupts != 0u || pendingGsInterruptEvents() != m_gsInterruptLatched ||
        ps2xGsSignalSerial() != g_seenSignalSerial || ps2xGsFinishSerial() != g_seenFinishSerial)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

uint64_t EeScheduler::consumeCycles(uint64_t cycles) noexcept
{
    uint64_t consumed = 0u;
    while (consumed < cycles && !m_checkpointPending.load(std::memory_order_acquire))
    {
        uint64_t step = std::min<uint64_t>(cycles - consumed, kIdleSkipStepCycles * 64u);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (nextEventCycle != 0u)
        {
            if (m_eeCycle >= nextEventCycle)
            {
                m_checkpointPending.store(true, std::memory_order_release);
                break;
            }
            step = std::min(step, nextEventCycle - m_eeCycle);
        }
        step = std::min(step, m_runtime.memory().cyclesUntilNextEeTimerInterrupt());
        if (step == 0u)
        {
            m_checkpointPending.store(true, std::memory_order_release);
            break;
        }
        accountCycles(static_cast<uint32_t>(step));
        consumed += step;
    }
    return consumed;
}

uint64_t EeScheduler::skipIdleCycles() noexcept
{
    const GuestThread *running = currentThread();
    if (running == nullptr || m_insideInterrupt || !running->invocations.empty() ||
        hasReadyAtOrAbovePriority(running->currentPriority))
    {
        return 0u;
    }

    uint64_t skipped = 0u;
    while (!m_checkpointPending.load(std::memory_order_acquire) &&
           !m_rescheduleRequested &&
           m_pendingInvocations.empty())
    {
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (nextEventCycle == 0u || m_eeCycle >= nextEventCycle)
        {
            break;
        }
        const uint64_t step = std::min({nextEventCycle - m_eeCycle,
                                        kIdleSkipStepCycles,
                                        m_runtime.memory().cyclesUntilNextEeTimerInterrupt()});
        if (step == 0u)
        {
            break;
        }
        accountCycles(static_cast<uint32_t>(step));
        skipped += step;
    }
    return skipped;
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    // 0 is a valid EE thread priority (the highest). CRI ADX creates its server threads at 0 and lowers them right
    // after with ChangeThreadPriority; rejecting 0 left Urban Reign without its ADX/file threads.
    if (params.priority < 0 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    requestTransfer();
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    traceScheduler(m_vsyncTick, m_eeCycle, "I kind=%d pc=%x", static_cast<int>(invocation.kind), invocation.context.pc);
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(ps2x::kernel_memory::kCallbackStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            const bool wasDisabled = (mask & (1u << cause)) == 0u;
            mask |= 1u << cause;
            uint32_t &latched = dmac ? g_pendingMaskedDmac : g_pendingMaskedIntc;
            if (wasDisabled && (latched & (1u << cause)) != 0u)
            {
                latched &= ~(1u << cause);
                dispatchIrq(dmac, cause);
            }
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

// D_STAT write-one-to-clear (ps2_memory.cpp): a cleared channel status has no interrupt left to deliver when the cause
// is enabled again (docs/PROGRESS.md §92).
void ps2xEeClearLatchedDmac(uint32_t channels)
{
    g_pendingMaskedDmac &= ~channels;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        (dmac ? g_pendingMaskedDmac : g_pendingMaskedIntc) |= 1u << cause;
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

uint32_t EeScheduler::pendingGsInterruptEvents() const noexcept
{
    const GSRegisters &gs = m_runtime.memory().gs();
    const uint64_t csr = gs.csr.load(std::memory_order_acquire);
    return static_cast<uint32_t>(csr & ~(gs.imr >> 8u) & 0x1Fu);
}

void EeScheduler::dispatchGsInterrupt()
{
    const uint32_t pending = pendingGsInterruptEvents();
    uint32_t raised = pending & ~m_gsInterruptLatched;
    m_gsInterruptLatched = pending;
    // A queued SIGNAL may be delivered right as the EE clears the previous one, so the bit can go 1 -> 0 -> 1 between
    // two checks. Each delivery is a new interrupt.
    const uint32_t signalSerial = ps2xGsSignalSerial();
    if (signalSerial != g_seenSignalSerial)
    {
        g_seenSignalSerial = signalSerial;
        raised |= pending & 0x1u;
    }
    const uint32_t finishSerial = ps2xGsFinishSerial();
    if (finishSerial != g_seenFinishSerial)
    {
        g_seenFinishSerial = finishSerial;
        raised |= pending & 0x2u;
    }
    static const bool logGs = [] { const char *v = std::getenv("PS2X_GS_SIGNAL_LOG"); return v && *v == '1'; }();
    if (logGs && raised != 0u)
        std::fprintf(stderr, "[gs-signal] dispatch GS irq raised=0x%x pending=0x%x intcEnabled=%d\n", raised, pending,
                     (m_enabledIntcMask & 1u) ? 1 : 0);
    if (raised != 0u)
    {
        dispatchIrq(false, 0u);
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

void EeScheduler::setVBlankPeriodMicroseconds(uint32_t microseconds) noexcept
{
    m_vblankPeriodMicroseconds.store(microseconds, std::memory_order_relaxed);
}

std::chrono::microseconds EeScheduler::vblankPeriod() const noexcept
{
    return std::chrono::microseconds(m_vblankPeriodMicroseconds.load(std::memory_order_relaxed));
}

uint64_t EeScheduler::vblankPeriodCycles() const noexcept
{
    return microsecondsToEeCycles(m_vblankPeriodMicroseconds.load(std::memory_order_relaxed));
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    enterWait(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
    throw EeDispatcherTransfer{};
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitUntilCycle(uint64_t cycle, int result, std::function<void(R5900Context &)> completion)
{
    const uint64_t token = ++m_cycleWaitSequence;
    scheduleEvent(cycle, std::chrono::steady_clock::now(), EeEvent{EeEventType::ExternalWake, kCycleWaitType, token});
    waitExternal(EeWaitReason::External, kCycleWaitType, token,
                 [result, completion = std::move(completion)](R5900Context &context)
                 {
                     setReturnS32(&context, result);
                     if (completion)
                     {
                         completion(context);
                     }
                 });
}

[[noreturn]] void EeScheduler::waitVSyncResume(uint64_t afterTick, int fixedResult, uint32_t resumePc)
{
    EeWaitState wait{EeWaitReason::VSync, EeVSyncWait{afterTick, fixedResult}, {}};
    wait.resume.setPc = true;
    wait.resume.pc = resumePc;
    enterWait(std::move(wait));
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::waitUntilCycleResume(uint64_t cycle, int result, uint32_t resumePc)
{
    const uint64_t token = ++m_cycleWaitSequence;
    scheduleEvent(cycle, std::chrono::steady_clock::now(), EeEvent{EeEventType::ExternalWake, kCycleWaitType, token});
    EeWaitState wait{EeWaitReason::External, EeExternalWait{kCycleWaitType, token}, {}};
    wait.resume.setPc = true;
    wait.resume.pc = resumePc;
    wait.resume.setResult = true;
    wait.resume.result = result;
    enterWait(std::move(wait));
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    enterWait(std::move(wait));
    throw EeDispatcherTransfer{};
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    m_snapshotDirty = true;
}

void EeScheduler::publishSnapshotNow()
{
    m_snapshotDirty = false;
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
    traceScheduler(m_vsyncTick, m_eeCycle, "S %d pc=%x", item.id, item.activeContext().pc);
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::enterWait(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    traceScheduler(m_vsyncTick, m_eeCycle, "B %d reason=%d obj=%d pc=%x ra=%x", self->id, static_cast<int>(wait.reason), waitObjectId(wait),
                   self->activeContext().pc, getRegU32(&self->activeContext(), 31));
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    enterWait(std::move(wait));
    requestTransfer();
}

void EeScheduler::requestTransfer()
{
    if (m_deferTransfers)
    {
        m_transferPending = true;
        return;
    }
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    traceScheduler(m_vsyncTick, m_eeCycle, "R %d result=%d by=%d", item.id, result, m_currentThreadId);
    auto completion = std::move(item.wait.completion);
    const EeResume resume = item.wait.resume;
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    item.pendingResume = resume;
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    const uint64_t firstDeadlineCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (firstDeadlineCycle != 0u && m_eeCycle >= firstDeadlineCycle)
    {
        processDueDeadlines();
    }
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    dispatchGsInterrupt();
    if (m_checkpointPending.load(std::memory_order_acquire))
    {
        std::deque<EeEvent> pending;
        {
            std::lock_guard lock(m_eventMutex);
            pending.swap(m_events);
        }
        for (const EeEvent &event : pending)
        {
            processEvent(event);
        }

        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            auto now = std::chrono::steady_clock::now();
            bool vblankDue = false;
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
                vblankDue |= item.deadlineCycle <= m_eeCycle && item.event.type == EeEventType::VBlankStart;
            }
            // MTVU timing model (asyncvif::vblankWaitEnabled): the vblank waits for the VU/GIF workers. An interrupt that
            // comes in meanwhile (the GS handler kicks the next part of the frame) runs first, with the deadline taken
            // off the checkpoints like below; 100 ms without progress is logged and the vblank goes ahead.
            if (vblankDue && ps2x::asyncvif::vblankWaitEnabled() && ps2x::asyncvif::active(m_runtime.memory()))
            {
                lock.unlock();
                const auto waitStart = std::chrono::steady_clock::now();
                bool deferred = false;
                while (ps2x::asyncvif::pipelineBusy(m_runtime.memory()))
                {
                    if (gsInterruptUnseen() || hasInterruptInvocation(m_pendingInvocations, currentThread()))
                    {
                        deferred = true;
                        break;
                    }
                    if (std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(100))
                    {
                        static int logged = 0;
                        if (logged++ < 10)
                            std::fprintf(stderr, "[mtvu] vblank waited 100 ms for the VU/GIF workers; going ahead\n");
                        break;
                    }
                    ps2x::asyncvif::waitPipelineIdle(m_runtime.memory(), std::chrono::microseconds(250));
                }
                lock.lock();
                if (deferred)
                {
                    g_deadlinesDeferredUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
                    m_nextDeadlineCycle.store(0u, std::memory_order_release);
                    return;
                }
                now = std::chrono::steady_clock::now();
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            // An interrupt handler that is queued or running finishes before the EE sleeps for real time. The guest ran
            // ahead to a deadline (vblank) that real time has not reached yet; a GS interrupt that came in meanwhile
            // (MTVU: the GIF thread finished a part of the frame) happened before that vblank. While the deadline is
            // due every checkpoint unwinds back here, so the handler could not run until the vblank had been
            // processed: it kicks the next DMA of the frame, which then always missed the swap (fights at 20 FPS).
            // So the deadline is taken off the checkpoints until the handler returns (run loop) or real time reaches
            // it (safety net in the run loop).
            if (now < pacingDeadline && hasInterruptInvocation(m_pendingInvocations, currentThread()))
            {
                g_deadlinesDeferredUntil = pacingDeadline;
                m_nextDeadlineCycle.store(0u, std::memory_order_release);
                ps2x::asyncvif::traceEvent('Q');
                return;
            }
            if (now < pacingDeadline)
            {
                if (!g_pacingCv.load(std::memory_order_relaxed))
                {
                    g_pacingMutex.store(&m_eventMutex, std::memory_order_release);
                    g_pacingCv.store(&m_eventCv, std::memory_order_release);
                }
                // A GS interrupt raised meanwhile (GIF thread) is dispatched before the due events: it happened first.
                // PS2X_PACING_STATS=1: per second, how often and how long the EE slept here and who woke it.
                static const bool pacingStats = [] { const char *v = std::getenv("PS2X_PACING_STATS"); return v && *v == '1'; }();
                const bool gsBefore = gsInterruptUnseen();
                const auto sleepStart = std::chrono::steady_clock::now();
                ps2x::asyncvif::traceEvent('P');
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() || gsInterruptUnseen() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                ps2x::asyncvif::traceEvent('p', gsInterruptUnseen() ? 1u : 0u);
                if (pacingStats)
                {
                    static uint64_t waits = 0, gsWakes = 0, gsAtEntry = 0, idleWaits = 0;
                    static double sleptMs = 0.0, idleSleptMs = 0.0;
                    static auto windowStart = std::chrono::steady_clock::now();
                    const auto wokeAt = std::chrono::steady_clock::now();
                    const double ms = std::chrono::duration<double, std::milli>(wokeAt - sleepStart).count();
                    ++waits;
                    sleptMs += ms;
                    if (m_currentThreadId == 0)
                    {
                        ++idleWaits;
                        idleSleptMs += ms;
                    }
                    if (gsBefore)
                        ++gsAtEntry;
                    else if (gsInterruptUnseen())
                        ++gsWakes;
                    if (wokeAt - windowStart >= std::chrono::seconds(1))
                    {
                        std::fprintf(stderr, "[pacing] waits=%llu slept=%.0fms (idle guest: %llu, %.0fms) gsWake=%llu gsAtEntry=%llu\n",
                                     static_cast<unsigned long long>(waits), sleptMs, static_cast<unsigned long long>(idleWaits),
                                     idleSleptMs, static_cast<unsigned long long>(gsWakes), static_cast<unsigned long long>(gsAtEntry));
                        waits = gsWakes = gsAtEntry = idleWaits = 0;
                        sleptMs = idleSleptMs = 0.0;
                        windowStart = wokeAt;
                    }
                }
                if (!m_events.empty() || gsInterruptUnseen() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // MTVU timing model: no catch-up beyond 2 ms of jitter. Catching up 2 fields after a late frame fired the
                // next vblank right after this one; a frame that needs ~18 ms of rendering then got 2 vblanks and the next
                // got none, so every image stayed 2 refreshes on screen and the next was skipped (~37 distinct images/s
                // at "60 FPS" in the heaviest map). Late vblanks now re-anchor: the game runs a little below 60 instead,
                // each image shown once, as in PCSX2.
                // While the IPU decodes an FMV its commands run inside the EE's register writes (a full-picture CSC
                // takes ~3 ms) and push vblanks past the 2 ms: the movie ran at ~82% speed. Then late vblanks catch up
                // as in the synchronous mode (ps2_memory.cpp ps2xIpuActiveRecently, PS2X_IPU_CATCHUP=0 turns it off).
                const auto catchUp = ps2x::asyncvif::vblankWaitEnabled() && ps2x::asyncvif::active(m_runtime.memory()) &&
                                             !ps2xIpuActiveRecently()
                                         ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(2))
                                         : std::chrono::duration_cast<std::chrono::steady_clock::duration>(kMaxVBlankCatchUpFields * vblankPeriod());
                const auto hostStart = std::max(scheduled.hostDeadline, std::chrono::steady_clock::now() - catchUp);
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              hostStart + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + vblankPeriodCycles(),
                              lockToHostVsync(hostStart + vblankPeriod()),
                              EeEvent{EeEventType::VBlankStart, 0, 0});
                m_pacingRefCycle = scheduled.deadlineCycle;
                m_pacingRefTime = hostStart;
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ps2x::asyncvif::traceEvent('V', static_cast<uint32_t>(m_eeCycle >> 10)); // EE cycle / 1024
        ps2x::pnach::applyContinuous(m_rdram); // PS2X_PNACH patch=1 lines, at every vblank like PCSX2 (also after a load)
        ps2xRaiseIntcStat(1u << 2); // INTC VBON
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        m_runtime.memory().gs().csr.fetch_or(0x8ull, std::memory_order_acq_rel);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        m_runtime.gs().vblank(); // the field to the GS when it takes the raw GIF stream (PCSX2's GS), before the handlers
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        ps2xRaiseIntcStat(1u << 3); // INTC VBOF
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::loadSharedVu0Random(R5900Context &context)
{
    if (!m_sharedVu0RandomValid)
    {
        saveSharedVu0Random(context);
        return;
    }
    context.vu0_r = m_sharedVu0Random;
}

void EeScheduler::saveSharedVu0Random(const R5900Context &context)
{
    m_sharedVu0Random = context.vu0_r;
    m_sharedVu0RandomValid = true;
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
