// What the port tells PCSX2's GS about its host (docs/GS_PCSX2.md). Implemented in HostStubs.cpp.
#pragma once

#include "common/WindowInfo.h"
#include "pcsx2gs_api.h"

#include <cstdint>
#include <string>

namespace pcsx2gs
{
    // The window the GS renders into (Host::AcquireRenderWindow returns it).
    void SetRenderWindow(const WindowInfo &window);
    // Folders: resources = the folder holding PCSX2's "shaders/" (dx11, dx12...); cache = shader cache.
    void SetFolders(const std::string &resources, const std::string &cache);
    // Replacement textures: the folder holding "replacements/" (any name; it stands for the serial folder).
    void SetTextureFolder(const std::string &folder);
    // Field rate the GS paces for (GetVerticalFrequency), e.g. 59.94.
    void SetVerticalFrequency(double hz);
    // Texts ImGuiManager::RenderOSD draws (pcsx2gs_api.h set_osd); thread-safe.
    void SetOsd(const char *top, uint32_t topRgba, float topAlpha, const char *bottom);
    // In-game menu (pcsx2gs_api.h set_menu / menu_input); thread-safe.
    void SetMenu(Pcsx2GsMenuFn draw, void *user);
    void MenuInput(const Pcsx2GsMenuInput *input);
}
