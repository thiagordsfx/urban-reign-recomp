#pragma once

#include "ps2x/iop/iop_pad.h"

#include <atomic>
#include <cstdint>

namespace ps2_host_input
{
    constexpr uint32_t kPadPorts = 2u;
    // Two controller ports, each with a multitap (PS2X_MULTITAP): up to 8 players.
    constexpr uint32_t kMaxPlayers = 8u;

    void setFieldCounter(const std::atomic<uint64_t> *counter);
    void sample();
    void releaseMouse();
    // The pad at a controller port and multitap slot (0 = A, the pad plugged straight in).
    [[nodiscard]] ps2x::iop::PadInputState padState(uint32_t port, uint32_t slot = 0u);
    // The motors the game set on that pad (any thread); played on the controllers of its LargeMotor/SmallMotor bindings.
    void setVibration(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor);
    // While blocked (the in-game menu is open) every pad stays plugged in but idle, so the game keeps running.
    void setBlocked(bool blocked);
}
