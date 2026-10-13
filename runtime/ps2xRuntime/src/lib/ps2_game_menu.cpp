#include "runtime/ps2_game_menu.h"

#include "ps2_host_backend.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/gs/gs_pcsx2_backend.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define PS2X_HAS_GAME_MENU 1
#endif

namespace ps2x::menu
{
    namespace
    {
        struct Panel
        {
            std::string title;
            DrawPanel draw;
        };

        std::mutex g_mutex; // panels and texts (registered before the menu can open, drawn on the GS thread)
        std::vector<Panel> g_panels;
        std::string g_title = "Menu";
        std::string g_hint;
        std::string g_empty;
        std::atomic<bool> g_open{false};
        std::atomic<bool> g_closedByWindow{false}; // the window's close button (GS thread)

        bool enabled()
        {
            static const bool on = []
            {
                const char *value = std::getenv("PS2X_GAME_MENU");
                return !(value && *value == '0');
            }();
            return on;
        }

#if defined(PS2X_HAS_GAME_MENU)
        // GS thread, at every present while the menu is open.
        void drawMenu(const Pcsx2GsUi *ui, void *)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            int open = 1;
            if (ui->begin_window(g_title.c_str(), &open, 760.0f, 540.0f))
            {
                if (!g_hint.empty())
                    ui->text_disabled(g_hint.c_str());
                if (g_panels.empty())
                {
                    ui->spacing();
                    ui->text_wrapped(g_empty.c_str());
                }
                else if (ui->begin_tab_bar("##panels"))
                {
                    for (const Panel &panel : g_panels)
                    {
                        if (ui->begin_tab(panel.title.c_str()))
                        {
                            panel.draw(ui);
                            ui->end_tab();
                        }
                    }
                    ui->end_tab_bar();
                }
            }
            ui->end_window();
            if (!open)
                g_closedByWindow.store(true, std::memory_order_relaxed);
        }

        // Keyboard, mouse and any controller, for the menu's navigation.
        Pcsx2GsMenuInput readInput()
        {
            Pcsx2GsMenuInput input{};
            const Vector2 mouse = GetMousePosition();
            input.mouse_x = mouse.x;
            input.mouse_y = mouse.y;
            input.mouse_buttons = (IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 1u : 0u) | (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ? 2u : 0u) |
                                  (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ? 4u : 0u);
            input.wheel = GetMouseWheelMove();
            const struct { int key; uint32_t bit; } keys[] = {
                {KEY_UP, PCSX2GS_KEY_UP}, {KEY_DOWN, PCSX2GS_KEY_DOWN}, {KEY_LEFT, PCSX2GS_KEY_LEFT},
                {KEY_RIGHT, PCSX2GS_KEY_RIGHT}, {KEY_ENTER, PCSX2GS_KEY_ENTER}, {KEY_KP_ENTER, PCSX2GS_KEY_ENTER},
                {KEY_SPACE, PCSX2GS_KEY_SPACE}, {KEY_ESCAPE, PCSX2GS_KEY_ESCAPE}, {KEY_TAB, PCSX2GS_KEY_TAB},
                {KEY_LEFT_SHIFT, PCSX2GS_KEY_SHIFT}, {KEY_RIGHT_SHIFT, PCSX2GS_KEY_SHIFT},
                {KEY_LEFT_CONTROL, PCSX2GS_KEY_CTRL}, {KEY_RIGHT_CONTROL, PCSX2GS_KEY_CTRL}};
            for (const auto &entry : keys)
                if (IsKeyDown(entry.key))
                    input.keys |= entry.bit;
            using namespace ps2_host_gamepads;
            for (int device = 0; device < kMaxDevices; ++device)
            {
                if (!connected(device))
                    continue;
                const float x = axis(device, LeftX);
                const float y = axis(device, LeftY);
                if (button(device, DPadUp) || y < -0.5f)
                    input.pad |= PCSX2GS_PAD_UP;
                if (button(device, DPadDown) || y > 0.5f)
                    input.pad |= PCSX2GS_PAD_DOWN;
                if (button(device, DPadLeft) || x < -0.5f)
                    input.pad |= PCSX2GS_PAD_LEFT;
                if (button(device, DPadRight) || x > 0.5f)
                    input.pad |= PCSX2GS_PAD_RIGHT;
                if (button(device, South))
                    input.pad |= PCSX2GS_PAD_ACCEPT;
                if (button(device, East))
                    input.pad |= PCSX2GS_PAD_BACK;
                if (button(device, LeftShoulder))
                    input.pad |= PCSX2GS_PAD_L1;
                if (button(device, RightShoulder))
                    input.pad |= PCSX2GS_PAD_R1;
            }
            return input;
        }

        void setOpen(bool open)
        {
            g_open.store(open, std::memory_order_relaxed);
            ps2_host_input::setBlocked(open);
            GSPcsx2Backend::SetMenu(open ? reinterpret_cast<void *>(&drawMenu) : nullptr, nullptr);
            std::fprintf(stderr, "[menu] %s\n", open ? "opened" : "closed");
        }
#endif
    }

    void addPanel(std::string title, DrawPanel draw)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_panels.push_back({std::move(title), std::move(draw)});
    }

    void setTitle(std::string title, std::string hint)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_title = std::move(title);
        g_hint = std::move(hint);
    }

    void setEmptyText(std::string text)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_empty = std::move(text);
    }

    bool isOpen() noexcept
    {
        return g_open.load(std::memory_order_relaxed);
    }

    void hostTick()
    {
#if defined(PS2X_HAS_GAME_MENU)
        if (!enabled() || !GSPcsx2Backend::Active())
            return;
        if (IsKeyPressed(KEY_F2))
            setOpen(!isOpen());
        else if (g_closedByWindow.exchange(false, std::memory_order_relaxed) && isOpen())
            setOpen(false);
        if (isOpen())
        {
            const Pcsx2GsMenuInput input = readInput();
            GSPcsx2Backend::SetMenuInput(&input);
        }
#endif
    }
}
