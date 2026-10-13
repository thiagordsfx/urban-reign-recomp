#include "runtime/ps2_save_state.h"

#include "ps2_runtime.h"
#include "ps2x/iop/iop_subsystem.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_backend_state.h"
#include "runtime/ps2_async_vif.h"
#include "Kernel/Stubs/CD.h"
#include "Kernel/Stubs/SIF.h"
#include "GuestKernelMemory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <variant>

void saveVuState(VU1Interpreter &vu, ps2x::state::Writer &out);
void loadVuState(VU1Interpreter &vu, ps2x::state::Reader &in);
void saveSyscallHostState(ps2x::state::Writer &out);
void loadSyscallHostState(ps2x::state::Reader &in, uint8_t *rdram);

namespace ps2x::savestate
{
    namespace
    {
        constexpr uint64_t kMagic = 0x3145544154535832ull;
        constexpr uint32_t kVersion = 1u;

        struct Extension
        {
            std::string name;
            SaveFn save;
            LoadFn load;
        };

        struct Request
        {
            bool load = false;
            std::filesystem::path path;
        };

        struct Registry
        {
            std::mutex mutex;
            std::vector<Extension> extensions;
            std::optional<Request> request;
            std::filesystem::path quickSlot;
            std::string message;
            std::chrono::steady_clock::time_point messageTime{};
            uint32_t deferred = 0u;
        };

        Registry &registry()
        {
            static Registry instance;
            return instance;
        }

        std::atomic<bool> s_requestPending{false};

        struct TimedRequest
        {
            uint64_t field = ~0ull;
            std::filesystem::path path;
        };

        TimedRequest parseTimed(const char *name)
        {
            TimedRequest result;
            const char *value = std::getenv(name);
            if (!value || !*value)
                return result;
            const std::string text(value);
            const size_t colon = text.find(':');
            if (colon == std::string::npos || colon == 0u)
                return result;
            result.field = std::strtoull(text.substr(0, colon).c_str(), nullptr, 10);
            result.path = text.substr(colon + 1u);
            return result;
        }

        TimedRequest &timedSave()
        {
            static TimedRequest request = parseTimed("PS2X_STATE_SAVE_AT");
            return request;
        }

        TimedRequest &timedLoad()
        {
            static TimedRequest request = parseTimed("PS2X_STATE_LOAD_AT");
            return request;
        }

        void post(Request request)
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            r.request = std::move(request);
            r.deferred = 0u;
            s_requestPending.store(true, std::memory_order_release);
        }

        void notify(const std::string &text)
        {
            std::cerr << "[savestate] " << text << std::endl;
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            r.message = text;
            r.messageTime = std::chrono::steady_clock::now();
        }

        template <size_t I = 0, typename V>
        void emplaceIndex(V &value, size_t index)
        {
            if constexpr (I < std::variant_size_v<V>)
            {
                if (index == I)
                    value.template emplace<I>();
                else
                    emplaceIndex<I + 1>(value, index);
            }
            else
            {
                throw ps2x::state::Error("state variant index out of range");
            }
        }
    }

    void registerExtension(std::string name, SaveFn save, LoadFn load)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        for (Extension &extension : r.extensions)
        {
            if (extension.name == name)
            {
                extension.save = std::move(save);
                extension.load = std::move(load);
                return;
            }
        }
        r.extensions.push_back(Extension{std::move(name), std::move(save), std::move(load)});
    }

    void setQuickSlotPath(std::filesystem::path path)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.quickSlot = std::move(path);
    }

    std::atomic<uint32_t> g_loadCount{0};

    uint32_t loadCount()
    {
        return g_loadCount.load(std::memory_order_acquire);
    }

    std::filesystem::path quickSlotPath()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        return r.quickSlot.empty() ? std::filesystem::path("states/quick.state") : r.quickSlot;
    }

    void requestSave(std::filesystem::path path)
    {
        post(Request{false, std::move(path)});
    }

    void requestLoad(std::filesystem::path path)
    {
        post(Request{true, std::move(path)});
    }

    void requestQuickSave()
    {
        requestSave(quickSlotPath());
    }

    void requestQuickLoad()
    {
        requestLoad(quickSlotPath());
    }

    bool overlayMessage(std::string &text, float &alpha)
    {
        constexpr float kVisibleSeconds = 2.5f;
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (r.message.empty())
            return false;
        const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - r.messageTime).count();
        if (age >= kVisibleSeconds)
            return false;
        text = r.message;
        alpha = age < kVisibleSeconds - 0.5f ? 1.0f : (kVisibleSeconds - age) / 0.5f;
        return true;
    }
}

struct GSStateAccess
{
    template <typename Ar>
    static void serialize(GS &gs, Ar &ar)
    {
        using namespace ps2x::state;
        std::lock_guard<std::recursive_mutex> lock(gs.m_stateMutex);
        std::lock_guard<std::mutex> lifetime(gs.m_backendLifetimeMutex);
        ar.section(fourcc("GSFE"));
        fields(ar, gs.m_ctx, gs.m_prim, gs.m_primRegister, gs.m_prmodeRegister, gs.m_curR, gs.m_curG, gs.m_curB, gs.m_curA, gs.m_curQ,
               gs.m_curS, gs.m_curT, gs.m_curU, gs.m_curV, gs.m_curFog, gs.m_fogR, gs.m_fogG, gs.m_fogB, gs.m_prmodecont, gs.m_pabe,
               gs.m_scanmsk, gs.m_dimx, gs.m_dthe, gs.m_colclamp, gs.m_texa, gs.m_texclut, gs.m_bitbltbuf, gs.m_trxpos, gs.m_trxreg,
               gs.m_trxdir, gs.m_vtxQueue, gs.m_vtxCount, gs.m_vtxIndex, gs.m_lastDisplayBaseBytes, gs.m_preferredDisplaySourceFrame,
               gs.m_preferredDisplayDestFbp, gs.m_hasPreferredDisplaySource);

        ar.section(fourcc("GSVR"));
        std::vector<uint8_t> vram;
        GSBackendState backendState;
        auto *access = dynamic_cast<GSBackendStateAccess *>(gs.m_backend.get());
        if constexpr (!Ar::kLoading)
        {
            if (gs.m_backend)
            {
                gs.m_backend->Flush();
                gs.m_backend->SnapshotVram(vram);
                if (access)
                    access->ExportState(backendState);
            }
            else if (gs.m_localMemoryStorage)
            {
                vram.assign(gs.m_localMemoryStorage, gs.m_localMemoryStorage + gs.m_localMemorySize);
            }
        }
        fields(ar, vram, backendState.clut, backendState.clutCbp, backendState.cachePageBase, backendState.cacheBytes,
               backendState.transfer, backendState.transferState, backendState.localToHost, backendState.localToHostReadPos);
        if constexpr (Ar::kLoading)
        {
            if (!gs.m_localMemoryStorage || vram.size() != gs.m_localMemorySize)
                throw Error("state VRAM size does not match");
            std::memcpy(gs.m_localMemoryStorage, vram.data(), vram.size());
            if (gs.m_backend)
            {
                gs.m_backend->Initialize(gs.m_localMemoryStorage, gs.m_localMemorySize);
                if (access)
                    access->ImportState(backendState);
            }
        }
    }
};

struct PS2VfsStateAccess
{
    struct SavedDescriptor
    {
        int32_t descriptor = -1;
        std::string path;
        int64_t position = 0;

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, descriptor, path, position);
        }
    };

    static void save(PS2Vfs &vfs, ps2x::state::Writer &out)
    {
        std::vector<SavedDescriptor> saved;
        int32_t next = 0;
        {
            std::lock_guard<std::mutex> lock(vfs.m_mutex);
            for (auto &[descriptor, open] : vfs.m_descriptors)
            {
                SavedDescriptor item;
                item.descriptor = descriptor;
                item.path = open.path;
                item.position = open.file ? open.file->seek(0, SEEK_CUR) : 0;
                saved.push_back(std::move(item));
            }
            next = vfs.m_nextDescriptor;
        }
        std::sort(saved.begin(), saved.end(), [](const SavedDescriptor &a, const SavedDescriptor &b)
                  { return a.descriptor < b.descriptor; });
        ps2x::state::fields(out, saved, next);
    }

    static void load(PS2Vfs &vfs, ps2x::state::Reader &in, const PS2RomDevice &rom)
    {
        std::vector<SavedDescriptor> saved;
        int32_t next = 0;
        ps2x::state::fields(in, saved, next);
        const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
        const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
        {
            std::lock_guard<std::mutex> lock(vfs.m_mutex);
            vfs.m_descriptors.clear();
        }
        for (const SavedDescriptor &item : saved)
        {
            int32_t opened = vfs.open(item.path, PS2_FIO_O_RDONLY, mounts, rom);
            if (opened < 0)
                opened = vfs.open(item.path, PS2_FIO_O_RDWR, mounts, rom);
            if (opened < 0)
            {
                std::cerr << "[savestate] could not reopen " << item.path << std::endl;
                continue;
            }
            std::lock_guard<std::mutex> lock(vfs.m_mutex);
            auto node = vfs.m_descriptors.extract(opened);
            node.key() = item.descriptor;
            if (node.mapped().file)
                (void)node.mapped().file->seek(item.position, SEEK_SET);
            vfs.m_descriptors.insert(std::move(node));
        }
        std::lock_guard<std::mutex> lock(vfs.m_mutex);
        vfs.m_nextDescriptor = next;
    }
};

template <typename Ar>
void serialize(Ar &ar, EeSemaphore &value)
{
    ps2x::state::fields(ar, value.id, value.count, value.maxCount, value.initCount, value.attr, value.option, value.waiters);
}

template <typename Ar>
void serialize(Ar &ar, EeEventFlag &value)
{
    ps2x::state::fields(ar, value.id, value.attr, value.option, value.initBits, value.bits, value.waiters);
}

template <typename Ar>
void serialize(Ar &ar, GuestThread &value)
{
    using namespace ps2x::state;
    fields(ar, value.id, value.context, value.entry, value.stack, value.stackSize, value.gp, value.attr, value.option, value.arg,
           value.initialPriority, value.currentPriority, value.status, value.suspendCount, value.wakeupCount, value.ownsStack,
           value.tlsBase, value.wait.reason, value.wait.resume, value.pendingResume);
    uint64_t index = value.wait.payload.index();
    field(ar, index);
    if constexpr (Ar::kLoading)
        ps2x::savestate::emplaceIndex(value.wait.payload, static_cast<size_t>(index));
    std::visit([&](auto &payload)
               { field(ar, payload); },
               value.wait.payload);
}

template <typename Ar>
void serialize(Ar &ar, PS2Memory::PendingTransfer &value)
{
    ps2x::state::fields(ar, value.fromScratchpad, value.srcAddr, value.qwc, value.chainData);
}

template <typename Ar>
void serialize(Ar &ar, PS2Memory::CodeRegion &value)
{
    ps2x::state::fields(ar, value.start, value.end, value.modified);
}

namespace
{
    struct SavedDeadline
    {
        uint64_t deadlineCycle = 0;
        int64_t hostOffsetNs = 0;
        EeEvent event{};
        uint64_t sequence = 0;
    };

    struct SavedModule
    {
        std::string name;
        uint32_t baseAddress = 0;
        uint64_t size = 0;
        bool active = false;

        template <typename Ar>
        void serialize(Ar &ar)
        {
            ps2x::state::fields(ar, name, baseAddress, size, active);
        }
    };
}

template <typename Ar>
void EeScheduler::serializeMachine(Ar &ar)
{
    using namespace ps2x::state;
    PS2Runtime &rt = m_runtime;
    PS2Memory &mem = rt.m_memory;

    ar.section(fourcc("EMEM"));
    ar.raw(mem.m_rdram, PS2_RAM_SIZE);
    ar.raw(mem.m_scratchpad, PS2_SCRATCHPAD_SIZE);
    ar.raw(mem.iop_ram, 2u * 1024u * 1024u);
    ar.raw(&mem.gs_regs, sizeof(GSRegisters));
    ar.raw(mem.m_vu0Code, PS2_VU0_CODE_SIZE);
    ar.raw(mem.m_vu0Data, PS2_VU0_DATA_SIZE);
    ar.raw(mem.m_vu1Code, PS2_VU1_CODE_SIZE);
    ar.raw(mem.m_vu1Data, PS2_VU1_DATA_SIZE);
    fields(ar, mem.m_seenGifCopy, mem.m_ioRegisters, mem.vif0_regs, mem.vif1_regs, mem.dma_regs, mem.m_tlbEntries, mem.m_path3Masked,
           mem.m_vif1PendingPath2ImageQwc, mem.m_vif1PendingPath2DirectHl, mem.m_path3MaskedFifo, mem.m_pendingGifTransfers,
           mem.m_pendingVif0Transfers, mem.m_pendingVif1Transfers, mem.m_codeRegions, mem.m_eeTimers, mem.m_eeTimerPendingCycles,
           mem.m_eeTimerEventBudget, mem.m_gifStatWritten);
    {
        std::lock_guard<std::mutex> lock(mem.m_completedDmacMutex);
        field(ar, mem.m_completedDmacCauses);
    }
    if constexpr (Ar::kLoading)
    {
        mem.markVU0CodeModified();
        mem.markVU1CodeModified();
    }

    ar.section(fourcc("EGS "));
    GSStateAccess::serialize(rt.m_gs, ar);

    ar.section(fourcc("EVU "));
    if constexpr (Ar::kLoading)
    {
        loadVuState(rt.m_vu0, ar);
        loadVuState(rt.m_vu1, ar);
        ps2x::asyncvif::loadTimingState(ar);
    }
    else
    {
        saveVuState(rt.m_vu0, ar);
        saveVuState(rt.m_vu1, ar);
        ps2x::asyncvif::saveTimingState(ar);
    }

    ar.section(fourcc("ERT "));
    field(ar, rt.m_cpuContext);
    {
        std::lock_guard lock(rt.m_eeKernelStateMutex);
        fields(ar, rt.m_eeExitHandlers, rt.m_eeSyscallOverrides, rt.m_eeSyscallMirrorAddresses);
        if constexpr (Ar::kLoading)
        {
            for (auto &word : rt.m_eeSyscallOverridePresence)
                word.store(0u, std::memory_order_relaxed);
            uint32_t outOfRange = 0u;
            for (const auto &[number, handler] : rt.m_eeSyscallOverrides)
            {
                (void)handler;
                const uint32_t presenceIndex = number + 256u;
                if (presenceIndex < 512u)
                    rt.m_eeSyscallOverridePresence[presenceIndex >> 6u].fetch_or(1ull << (presenceIndex & 63u), std::memory_order_acq_rel);
                else
                    ++outOfRange;
            }
            rt.m_eeSyscallOverridesOutOfRange.store(outOfRange, std::memory_order_release);
        }
    }
    {
        std::lock_guard lock(rt.m_guestHeapMutex);
        fields(ar, rt.m_guestHeapBlocks, rt.m_guestHeapBase, rt.m_guestHeapEnd, rt.m_guestHeapLimit, rt.m_guestHeapSuggestedBase,
               rt.m_guestHeapConfigured);
    }
    {
        std::lock_guard lock(rt.m_asyncCallbackStackMutex);
        fields(ar, rt.m_asyncCallbackStackFloor, rt.m_asyncCallbackStackTop);
    }
    {
        std::vector<SavedModule> modules;
        if constexpr (!Ar::kLoading)
        {
            for (const auto &module : rt.m_loadedModules)
                modules.push_back(SavedModule{module.name, module.baseAddress, module.size, module.active});
        }
        field(ar, modules);
        if constexpr (Ar::kLoading)
        {
            rt.m_loadedModules.clear();
            for (const SavedModule &module : modules)
                rt.m_loadedModules.push_back({module.name, module.baseAddress, static_cast<size_t>(module.size), module.active});
        }
    }

    ar.section(fourcc("ESCH"));
    {
        uint32_t vblankPeriod = m_vblankPeriodMicroseconds.load(std::memory_order_relaxed);
        field(ar, vblankPeriod);
        if constexpr (Ar::kLoading)
            m_vblankPeriodMicroseconds.store(vblankPeriod, std::memory_order_relaxed);
    }
    fields(ar, m_readyQueues, m_threads, m_semaphores, m_eventFlags, m_alarms, m_intcHandlers, m_dmacHandlers, m_nextThreadId,
           m_nextInvocationThreadId, m_nextSemaphoreId, m_nextEventFlagId, m_nextAlarmId, m_nextIntcHandlerId, m_nextDmacHandlerId,
           m_intcHeadOrder, m_intcTailOrder, m_dmacHeadOrder, m_dmacTailOrder, m_enabledIntcMask, m_enabledDmacMask, m_currentThreadId,
           m_rescheduleRequested, m_sharedVu0RandomValid, m_sharedVu0Random, m_timeSliceExpired, m_pendingEeTimerInterrupts,
           m_gsInterruptLatched, m_eeCycle, m_sliceEndCycle, m_eventSequence, m_cycleWaitSequence, m_invocationSequence, m_vsyncTick,
           m_vsyncFlagAddress, m_vsyncTickAddress, m_gsVSyncCallback, m_gsVSyncCallbackGp, m_gsVSyncCallbackSp, m_invocationStackTops);
    m_pacingRefTime = {};
    m_pacingCheckCycle = 0;
    {
        std::lock_guard lock(m_eventMutex);
        const auto now = std::chrono::steady_clock::now();
        std::vector<SavedDeadline> deadlines;
        if constexpr (!Ar::kLoading)
        {
            for (const ScheduledEvent &item : m_deadlines)
            {
                const int64_t offset = std::chrono::duration_cast<std::chrono::nanoseconds>(item.hostDeadline - now).count();
                deadlines.push_back(SavedDeadline{item.deadlineCycle, offset, item.event, item.sequence});
            }
        }
        fields(ar, deadlines, m_events);
        if constexpr (Ar::kLoading)
        {
            m_deadlines.clear();
            for (const SavedDeadline &item : deadlines)
            {
                const auto host = now + std::chrono::nanoseconds(std::max<int64_t>(item.hostOffsetNs, 0));
                m_deadlines.push_back(ScheduledEvent{item.deadlineCycle, host, item.event, item.sequence});
            }
            m_pendingInvocations.clear();
        }
    }
    if constexpr (Ar::kLoading)
    {
        bool legacyCallbackStacks = rt.m_asyncCallbackStackTop > ps2x::kernel_memory::kCallbackStackTop;
        for (const auto &[key, top] : m_invocationStackTops)
            legacyCallbackStacks = legacyCallbackStacks || top > ps2x::kernel_memory::kCallbackStackTop;
        if (legacyCallbackStacks)
        {
            m_invocationStackTops.clear();
            std::lock_guard lock(rt.m_asyncCallbackStackMutex);
            rt.m_asyncCallbackStackFloor = ps2x::kernel_memory::kCallbackStackFloor;
            rt.m_asyncCallbackStackTop = ps2x::kernel_memory::kCallbackStackTop;
        }
        for (auto &[id, item] : m_threads)
        {
            if (ps2x::kernel_memory::inLegacyPool(item.tlsBase, ps2x::kernel_memory::kLegacyTlsPoolBase))
                item.tlsBase = 0;
        }
        m_unwinding = false;
        m_insideInterrupt = false;
        m_deferTransfers = false;
        m_transferPending = false;
        m_checkpointPending.store(false, std::memory_order_release);
        m_debugPublishCountdown = 0u;
        for (auto &[id, item] : m_threads)
        {
            item.wait.completion = {};
            item.resumeCompletion = {};
            item.invocations.clear();
        }
        updateNextDeadline();
        std::lock_guard lock(m_snapshotMutex);
        m_snapshotDirty = true;
    }

    ar.section(fourcc("EIOP"));
    if (rt.m_iopSubsystem)
    {
        if constexpr (Ar::kLoading)
            rt.m_iopSubsystem->loadState(ar);
        else
            rt.m_iopSubsystem->saveState(ar);
    }

    ar.section(fourcc("EVFS"));
    if constexpr (Ar::kLoading)
        PS2VfsStateAccess::load(rt.m_vfs, ar, rt.m_romDevice);
    else
        PS2VfsStateAccess::save(rt.m_vfs, ar);

    ar.section(fourcc("ESTB"));
    if constexpr (Ar::kLoading)
    {
        ps2_stubs::loadCdStubState(ar);
        ps2_stubs::loadSifStubState(ar);
        loadSyscallHostState(ar, mem.m_rdram);
    }
    else
    {
        ps2_stubs::saveCdStubState(ar);
        ps2_stubs::saveSifStubState(ar);
        saveSyscallHostState(ar);
    }
}

bool ps2xGsStallActive();                       // gs/gs_frontend.cpp
void ps2xGsResetStall();                        // gs/gs_frontend.cpp
bool ps2xGsSignalQueuePending();                // ps2_memory.cpp
void ps2xGsResetSignalState();                  // ps2_memory.cpp
void ps2xGsResetAsyncReadback();                // gs/gs_threaded_backend.cpp
void ps2xSchedulerResetGsTransientState() noexcept; // Kernel/EeScheduler.cpp

bool EeScheduler::serviceStateRequests()
{
    using namespace ps2x::savestate;
    const uint64_t field = m_vsyncTick;
    std::optional<Request> request;
    bool fromTimer = false;
    if (s_requestPending.load(std::memory_order_acquire))
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        request = r.request;
    }
    else if (timedLoad().field <= field)
    {
        request = Request{true, timedLoad().path};
        fromTimer = true;
    }
    else if (timedSave().field <= field)
    {
        request = Request{false, timedSave().path};
        fromTimer = true;
    }
    if (!request)
        return false;

    PS2Memory &mem = m_runtime.m_memory;
    // The IOP worker thread may have effects for the EE still deferred (SIF commands, EE memory writes). Deliver them
    // now, before the safe-point checks and before the EE is serialized: saveState flushed them only when it reached
    // the IOP section, after the EE memory and scheduler had been written, so whatever they produced on the EE side
    // was missing from the file (a load then left CRI's sound manager waiting for an IOP response forever: no music;
    // the pad data written by PADMAN went the same way).
    if (m_runtime.m_iopSubsystem)
        m_runtime.m_iopSubsystem->flushPendingWork();
    std::string blocker;
    if (!m_pendingInvocations.empty())
        blocker = "pending guest invocation";
    for (const auto &[id, item] : m_threads)
    {
        if (!blocker.empty())
            break;
        if (!item.invocations.empty())
            blocker = "thread " + std::to_string(id) + " is running a handler";
        else if (item.resumeCompletion || item.wait.completion)
            blocker = "thread " + std::to_string(id) + " waits on a host completion";
    }
    if (blocker.empty())
    {
        ps2x::asyncvif::sync(mem);
        if (mem.m_gifArbiter && !mem.m_gifArbiter->empty())
            blocker = "GIF arbiter has queued packets";
        // MTVU: GIF data the GS holds at a pending SIGNAL is not saved; wait (a few ms) until the game's handler clears it.
        else if (!request->load && (ps2xGsStallActive() || ps2xGsSignalQueuePending()))
            blocker = "GS SIGNAL stall pending";
        // A memory card operation still running on the IOP (its completion is not in the file).
        else if (!request->load && m_runtime.m_iopSubsystem && m_runtime.m_iopSubsystem->hasPendingRpc())
            blocker = "IOP RPC pending";
    }
    if (!blocker.empty())
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (++r.deferred == 2000u)
            std::cerr << "[savestate] still waiting for a safe point: " << blocker << std::endl;
        return false;
    }

    auto finish = [&]
    {
        if (fromTimer)
        {
            if (request->load)
                timedLoad().field = ~0ull;
            else
                timedSave().field = ~0ull;
            return;
        }
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.request.reset();
        s_requestPending.store(false, std::memory_order_release);
    };

    const std::filesystem::path path = request->path;
    if (!request->load)
    {
        try
        {
            ps2x::state::Writer out;
            uint64_t magic = kMagic;
            uint32_t version = kVersion;
            uint64_t tick = field;
            ps2x::state::fields(out, magic, version, tick);
            serializeMachine(out);
            out.section(ps2x::state::fourcc("EEXT"));
            {
                Registry &r = registry();
                std::vector<Extension> extensions;
                {
                    std::lock_guard<std::mutex> lock(r.mutex);
                    extensions = r.extensions;
                }
                uint64_t count = extensions.size();
                ps2x::state::field(out, count);
                for (Extension &extension : extensions)
                {
                    ps2x::state::Writer blob;
                    if (extension.save)
                        extension.save(blob);
                    ps2x::state::fields(out, extension.name, blob.data());
                }
            }
            out.section(ps2x::state::fourcc("END "));
            std::error_code ec;
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path(), ec);
            const std::filesystem::path temp = path.string() + ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char *>(out.data().data()), static_cast<std::streamsize>(out.data().size()));
                if (!file)
                    throw ps2x::state::Error("could not write " + temp.string());
            }
            std::filesystem::rename(temp, path, ec);
            if (ec)
                throw ps2x::state::Error("could not replace " + path.string() + ": " + ec.message());
            notify("State saved (field " + std::to_string(field) + ")");
        }
        catch (const std::exception &error)
        {
            notify(std::string("Save failed: ") + error.what());
        }
        finish();
        return false;
    }

    std::vector<uint8_t> bytes;
    {
        std::ifstream file(path, std::ios::binary);
        if (file)
            bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    finish();
    if (bytes.empty())
    {
        notify("No saved state at " + path.string());
        return false;
    }
    try
    {
        ps2x::state::Reader in(bytes.data(), bytes.size());
        uint64_t magic = 0;
        uint32_t version = 0;
        uint64_t tick = 0;
        ps2x::state::fields(in, magic, version, tick);
        if (magic != kMagic || version != kVersion)
            throw ps2x::state::Error("not a compatible save state");
        serializeMachine(in);
        in.section(ps2x::state::fourcc("EEXT"));
        uint64_t count = 0;
        ps2x::state::field(in, count);
        std::vector<Extension> extensions;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            extensions = r.extensions;
        }
        for (uint64_t i = 0; i < count; ++i)
        {
            std::string name;
            std::vector<uint8_t> blob;
            ps2x::state::fields(in, name, blob);
            for (Extension &extension : extensions)
            {
                if (extension.name == name && extension.load)
                {
                    ps2x::state::Reader blobReader(blob.data(), blob.size());
                    extension.load(blobReader);
                }
            }
        }
        in.section(ps2x::state::fourcc("END "));
        // Transient MTVU state of the running session (not in the file; the workers were drained above): a SIGNAL stall,
        // queued SIGNAL/FINISH deliveries, interrupt serials, a deferred deadline, an async readback in progress. Left
        // in place they replayed old GIF data or kept the frame-done SIGNAL from ever arriving (F9 mid-fight froze the
        // game at 0 FPS while the music played on).
        ps2xGsResetStall();
        ps2xGsResetSignalState();
        ps2xGsResetAsyncReadback();
        ps2xSchedulerResetGsTransientState();
    }
    catch (const std::exception &error)
    {
        notify(std::string("Load failed, emulation state is undefined: ") + error.what());
        m_runtime.requestStop();
        requestStop();
        return true;
    }
    ps2x::savestate::g_loadCount.fetch_add(1u, std::memory_order_acq_rel);
    publishSnapshotNow();
    notify("State loaded (field " + std::to_string(m_vsyncTick) + ")");
    return true;
}
