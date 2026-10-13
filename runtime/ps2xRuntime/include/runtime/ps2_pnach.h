#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// PCSX2 .pnach patches (patch=<place>,EE,<address>,<type>,<value>), applied to EE RAM like PCSX2 does:
// place 0 once after the ELF is loaded, place 1 at every vblank, place 2 both. Types byte/short/word/double and
// extended: the CodeBreaker codes PCSX2 runs (0/1/2 writes, 3 increment/decrement, 4 serial write, 5 copy,
// 6 pointer write, 7 OR/AND/XOR, D and E conditionals), in file order with PCSX2's line state.
//
// PS2X_PNACH=<file>[;<file>...] lists the enabled files (config.ini [Mods] or [Runtime]); "none" turns it off.
// While the game runs the list and the files are watched (watch()); a change reloads them at once
// (PS2X_PNACH_RELOAD=0 turns this off). F4 reloads by hand.
namespace ps2x::pnach
{
    struct Patch
    {
        uint8_t place = 1u;
        uint8_t bytes = 4u;     // 1, 2, 4 or 8; 0 = an extended (CodeBreaker) line
        uint32_t address = 0u;  // extended: the whole first word, code type included
        uint64_t value = 0u;
    };

    // Loads the files listed in PS2X_PNACH (once; later calls return the cached count). Returns the patches loaded.
    size_t loadFromEnvironment();
    // Loads one file and adds its patches. Returns how many it added; unsupported lines are logged.
    size_t loadFile(const std::string &path);
    void clear();
    [[nodiscard]] bool active() noexcept;

    // Writes the patches of that kind into EE RAM (rdram = 32 MB main memory). Called on the EE thread.
    void applyOnce(uint8_t *rdram);       // place 0 and 2
    void applyContinuous(uint8_t *rdram); // place 1 and 2, every vblank; also runs place 0/2 after a reload
    [[nodiscard]] uint64_t writesApplied() noexcept;

    // Live reload. `files` returns the current list of enabled .pnach files (the port reads config.ini again); it is
    // called about once a second on a thread of its own. When the list or a file's time stamp changes the patches
    // are reloaded and the new place 0/2 lines run at the next vblank.
    void watch(std::function<std::vector<std::string>()> files);
    void requestReload(); // F4

    // Patch groups from mods (pnach text built into a mod DLL): applied with the files' patches while enabled.
    // Returns the group's id (or -1 when the text has no patch line).
    int addGroup(const std::string &name, const std::string &pnachText, bool enabled);
    void setGroupEnabled(int id, bool enabled); // any thread; the place 0/2 lines run again at the next vblank
    [[nodiscard]] bool groupEnabled(int id);
    // Code to run at every vblank on the EE thread, right after the patches (mods' frame callbacks). Register before
    // the game starts.
    void addFrameHook(std::function<void()> hook);
    // The "patches reloaded" line for the on-screen message (true while it is shown; alpha fades it out).
    [[nodiscard]] bool overlayMessage(std::string &text, float &alpha);
}
