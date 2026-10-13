#pragma once

#include "ps2x/state_io.h"

#include <filesystem>
#include <functional>
#include <string>

namespace ps2x::savestate
{
    using SaveFn = std::function<void(ps2x::state::Writer &)>;
    using LoadFn = std::function<void(ps2x::state::Reader &)>;

    void registerExtension(std::string name, SaveFn save, LoadFn load);

    void setQuickSlotPath(std::filesystem::path path);
    [[nodiscard]] std::filesystem::path quickSlotPath();
    void requestSave(std::filesystem::path path);
    void requestLoad(std::filesystem::path path);
    void requestQuickSave();
    void requestQuickLoad();
    // States loaded so far (it changes right after a load, before the game runs again).
    [[nodiscard]] uint32_t loadCount();

    [[nodiscard]] bool overlayMessage(std::string &text, float &alpha);
}
