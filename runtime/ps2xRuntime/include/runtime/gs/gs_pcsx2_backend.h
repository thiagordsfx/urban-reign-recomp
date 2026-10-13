#pragma once

#include "runtime/gs/gs_backend.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

// PCSX2's GS (hardware renderer, pcsx2gs.dll) as the raster back end (docs/GS_PCSX2.md, phase 2). It takes the raw GIF
// stream and the vsyncs, and presents into a child window over the game window on its own (no PresentationFrame).
// Runs inside GSThreadedBackend: every call below happens on the GS thread, except the static ones.
// The default renderer; PS2X_GS_RENDERER=nativo (config.ini [Video] Renderizador=nativo) keeps the port's GS, and
// without the DLL the GPU back end stays.
class GSPcsx2Backend final : public GSRasterBackend
{
public:
    // Main thread: true when PS2X_GS_RENDERER asks for it and pcsx2gs.dll loads.
    static bool Requested();
    // Main thread, after the game window exists: creates the child window the GS renders into.
    static void AttachHostWindow(void *parentHwnd);
    // Main thread, once per host frame: keeps the child window over the parent's client area.
    static void HostTick();
    // The child window exists (the host loop then leaves the screen to it).
    static bool Active();
    // Any thread: the host overlays drawn over the GS image (top: message box with background rgba and fade; bottom:
    // the FPS line). Empty = none.
    static void SetOsd(const char *top, uint32_t topRgba, float topAlpha, const char *bottom);
    // Any thread: the in-game menu (pcsx2gs_api.h set_menu: draw is a Pcsx2GsMenuFn, NULL hides it) and its input
    // (a Pcsx2GsMenuInput).
    static void SetMenu(void *draw, void *user);
    static void SetMenuInput(const void *input);
    // Main thread: hides the GS image while a host UI drawn by raylib (the F1 debugger) is open.
    static void SetHostUiVisible(bool visible);
    static bool HostUiVisible();
    // Any thread: fields presented so far (the F3 counter's "Screen").
    static uint64_t Presents();

    GSPcsx2Backend();
    ~GSPcsx2Backend() override;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override {}
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override {}
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override {}

    void Flush() override {}
    void TextureFlush() override {}
    void Sync(GSSyncReason reason) override {}
    PresentationFrame Present(const GSPresentationRequest &request) override { return {}; }

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override { return false; }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override { return 0u; }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override {}
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // GS thread: the transfer's area as it is now, outside the GIF stream's own transfer (the async readback cache of
    // GSThreadedBackend refreshes its entries with it). False if the GS is not open.
    bool ReadTransfer(const GSTransferCommand &command, std::vector<uint8_t> &out);

    bool WantsRawGif() const override { return true; }
    void RawGif(const uint8_t *data, uint32_t sizeBytes) override;
    void VSync(const GSVSyncRegs &regs) override;
    void GsReset() override;

private:
    bool EnsureOpen();
    void WriteFrameTime(std::chrono::steady_clock::time_point presentStart);

    bool m_open = false;
    bool m_failed = false;
    GSTransferCommand m_download{};
    uint32_t m_downloadLeft = 0u; // bytes of the current local -> host transfer not read yet
    uint32_t m_width = 0u, m_height = 0u;
    std::vector<uint8_t> m_pendingVram; // local memory to load once the GS is open
    uint32_t m_vramSize = 4u * 1024u * 1024u;
    uint64_t m_fields = 0u;              // vsyncs presented
    uint64_t m_gifBytes = 0u;
    uint64_t m_gifBytesAtPresent = 0u;
    std::chrono::steady_clock::time_point m_lastPresent{};
};
