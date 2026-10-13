#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// DLL mods (modsdk/include/urban_mod_api.h): game\mods\<Name>\<Name>.dll, switched on by config.ini [Mods] Dlls.
// Each loaded mod gets a tab in the F2 menu ("MODS / PATCHES") with its patch groups and its own options.
namespace urban::modloader
{
    // Loads the mods named in `dlls` ("A;B") from modsDir. rdram = the PS2's main memory (32 MB). Call after the ELF is
    // loaded and before the game starts. Returns how many loaded.
    size_t loadAll(const std::filesystem::path &modsDir, const std::string &dlls, const std::string &language, uint8_t *rdram);
}
