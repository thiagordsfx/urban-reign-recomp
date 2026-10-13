#pragma once

#include <functional>
#include <string>

struct Pcsx2GsUi; // pcsx2gs_api.h: the UI calls a panel draws with

// In-game menu (F2) over the image of PCSX2's GS: one window with a tab per registered panel. The game keeps running;
// while the menu is open the controllers and the keyboard drive the menu instead of the game. Panels draw on the GS
// thread with the DLL's UI table (Pcsx2GsUi). PS2X_GAME_MENU=0 turns it off.
namespace ps2x::menu
{
    using DrawPanel = std::function<void(const Pcsx2GsUi *ui)>;

    // Register before the game loop starts; tabs appear in this order.
    void addPanel(std::string title, DrawPanel draw);
    // The window title (e.g. "Urban Reign") and the hint line under the tabs.
    void setTitle(std::string title, std::string hint);
    // Shown instead of the tabs when no panel is registered (e.g. "no mod is on").
    void setEmptyText(std::string text);
    [[nodiscard]] bool isOpen() noexcept;
    // Window thread, once per host frame: F2 opens/closes it and the input goes to the menu while it is open.
    void hostTick();
}
