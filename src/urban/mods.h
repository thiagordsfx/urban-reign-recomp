#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Mods: the PCSX2 .pnach files in the "mods" folder next to urban.exe, switched on by config.ini [Mods] Enabled
// ("a.pnach;b.pnach"). The runtime reloads them while the game runs (ps2_pnach.cpp). DLL mods: mod_loader.h.
namespace urban::mods
{
    // config.ini [Mods] Enabled as the file has it now (a launcher may change it while the game runs).
    std::string readEnabled(const std::filesystem::path &configPath);
    // The files of an Enabled list that exist in the mods folder (log = report the missing ones).
    std::vector<std::string> pnachFiles(const std::filesystem::path &configPath, const std::string &enabled, bool log);
}
