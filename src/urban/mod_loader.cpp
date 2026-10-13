#include "urban/mod_loader.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"
#include "urban_mod_api.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_HAS_GAME_MENU 1
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(URBAN_HAS_GAME_MENU)
// The mods draw with the GS's UI table: the same functions in the same order.
static_assert(sizeof(UrbanModUi) == sizeof(Pcsx2GsUi), "UrbanModUi must match Pcsx2GsUi");
#endif

namespace urban::modloader
{
    namespace
    {
        constexpr uint32_t kRamSize = 32u * 1024u * 1024u;

        struct PatchGroup
        {
            int id = -1;
            std::string key; // "patch.<n>": its place among the mod's options, the same in every language
            std::string name;
            std::string description;
        };

        struct Mod
        {
            std::string name;
            std::string description; // mod.json
            std::string language;
#if defined(_WIN32)
            HMODULE dll = nullptr;
#endif
            UrbanModHost host{};
            std::vector<PatchGroup> shown;
            UrbanModDrawFn draw = nullptr;
            void *drawUser = nullptr;
            UrbanModFrameFn frame = nullptr;
            void *frameUser = nullptr;
            std::filesystem::path settingsPath;
            std::map<std::string, std::string> settings;
            std::mutex settingsMutex;
            bool failed = false; // a callback crashed: it is not called again
        };

        std::vector<std::unique_ptr<Mod>> g_mods;
        uint8_t *g_rdram = nullptr;

        // The mod whose code is running (host calls carry no mod argument; every call comes from inside a callback).
        thread_local Mod *t_current = nullptr;
        struct CurrentMod
        {
            Mod *previous;
            explicit CurrentMod(Mod *mod) : previous(t_current) { t_current = mod; }
            ~CurrentMod() { t_current = previous; }
        };

        void saveSettings(Mod &mod) // settingsMutex held
        {
            std::ofstream out(mod.settingsPath, std::ios::binary | std::ios::trunc);
            for (const auto &[key, value] : mod.settings)
                out << key << '=' << value << '\n';
        }

        void loadSettings(Mod &mod)
        {
            std::ifstream file(mod.settingsPath);
            std::string line;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const size_t equals = line.find('=');
                if (equals != std::string::npos && equals > 0u)
                    mod.settings[line.substr(0, equals)] = line.substr(equals + 1);
            }
        }

        std::string jsonString(const std::string &json, const char *key)
        {
            const std::regex pattern(std::string("\"") + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
            std::smatch match;
            return std::regex_search(json, match, pattern) ? match[1].str() : std::string();
        }

        // --- the host table (urban_mod_api.h) ---
        void hostLog(const char *text)
        {
            std::cerr << "[mod " << (t_current ? t_current->name : "?") << "] " << (text ? text : "") << std::endl;
        }

        int hostRead(uint32_t address, void *out, uint32_t size)
        {
            address &= 0x01FFFFFFu;
            if (!g_rdram || !out || address + static_cast<uint64_t>(size) > kRamSize)
                return 0;
            std::memcpy(out, g_rdram + address, size);
            return 1;
        }

        int hostWrite(uint32_t address, const void *data, uint32_t size)
        {
            address &= 0x01FFFFFFu;
            if (!g_rdram || !data || address + static_cast<uint64_t>(size) > kRamSize)
                return 0;
            std::memcpy(g_rdram + address, data, size);
            return 1;
        }

        const char *hostGetSetting(const char *key, const char *fallback)
        {
            Mod *mod = t_current;
            if (!mod || !key)
                return fallback;
            std::lock_guard<std::mutex> lock(mod->settingsMutex);
            const auto it = mod->settings.find(key);
            return it != mod->settings.end() ? it->second.c_str() : fallback;
        }

        void hostSetSetting(const char *key, const char *value)
        {
            Mod *mod = t_current;
            if (!mod || !key)
                return;
            std::lock_guard<std::mutex> lock(mod->settingsMutex);
            mod->settings[key] = value ? value : "";
            saveSettings(*mod);
        }

        uint32_t hostStateLoads()
        {
            return ps2x::savestate::loadCount();
        }

        int hostAddPatches(const char *name, const char *description, const char *text, int enabled, int show)
        {
            Mod *mod = t_current;
            if (!mod || !name || !text)
                return -1;
            bool on = enabled != 0;
            const std::string key = "patch." + std::to_string(mod->shown.size());
            if (show)
            {
                // The player's last choice wins over the mod's default.
                const std::string saved = hostGetSetting(key.c_str(), "");
                if (!saved.empty())
                    on = saved == "1";
            }
            const int id = ps2x::pnach::addGroup(mod->name + ": " + name, text, on);
            if (id >= 0 && show)
                mod->shown.push_back({id, key, name, description ? description : ""});
            return id;
        }

        void hostSetPatchesEnabled(int id, int enabled)
        {
            ps2x::pnach::setGroupEnabled(id, enabled != 0);
            if (Mod *mod = t_current)
                for (const PatchGroup &group : mod->shown)
                    if (group.id == id)
                        hostSetSetting(group.key.c_str(), enabled ? "1" : "0");
        }

        int hostPatchesEnabled(int id)
        {
            return ps2x::pnach::groupEnabled(id) ? 1 : 0;
        }

        void hostSetTab(UrbanModDrawFn draw, void *user)
        {
            if (Mod *mod = t_current)
            {
                mod->draw = draw;
                mod->drawUser = user;
            }
        }

        void hostOnFrame(UrbanModFrameFn frame, void *user)
        {
            if (Mod *mod = t_current)
            {
                mod->frame = frame;
                mod->frameUser = user;
            }
        }

        // A crash inside a mod stops that mod, not the game (structured exceptions, Windows).
        template <typename Fn>
        bool guarded(Mod &mod, const char *what, Fn &&call)
        {
#if defined(_WIN32)
            struct Seh
            {
                static bool run(Fn &call)
                {
                    __try
                    {
                        call();
                        return true;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        return false;
                    }
                }
            };
            if (Seh::run(call))
                return true;
            mod.failed = true;
            std::cerr << "[mod " << mod.name << "] crashed in " << what << "; the mod is stopped" << std::endl;
            return false;
#else
            call();
            return true;
#endif
        }

#if defined(URBAN_HAS_GAME_MENU)
        void drawTab(Mod &mod, const Pcsx2GsUi *ui)
        {
            const bool pt = mod.language == "pt";
            if (!mod.description.empty())
                ui->text_wrapped(mod.description.c_str());
            if (mod.failed)
            {
                ui->text_colored(0xFF4A5AFFu, pt ? "Este mod travou e foi parado (veja o log)." : "This mod crashed and was stopped (see the log).");
                return;
            }
            if (!mod.shown.empty())
            {
                ui->spacing();
                int index = 0;
                for (const PatchGroup &group : mod.shown)
                {
                    ui->push_id(index++);
                    int on = ps2x::pnach::groupEnabled(group.id) ? 1 : 0;
                    if (ui->checkbox(group.name.c_str(), &on))
                    {
                        CurrentMod current(&mod);
                        hostSetPatchesEnabled(group.id, on);
                    }
                    if (!group.description.empty())
                        ui->text_disabled(group.description.c_str());
                    ui->pop_id();
                }
            }
            if (mod.draw)
            {
                ui->separator();
                CurrentMod current(&mod);
                const auto *modUi = reinterpret_cast<const UrbanModUi *>(ui);
                guarded(mod, "its tab", [&] { mod.draw(modUi, mod.drawUser); });
            }
        }
#endif
    }

    size_t loadAll(const std::filesystem::path &modsDir, const std::string &dlls, const std::string &language, uint8_t *rdram)
    {
#if defined(_WIN32)
        g_rdram = rdram;
        std::istringstream list(dlls);
        std::string name;
        while (std::getline(list, name, ';'))
        {
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if (name.empty())
                continue;
            const std::filesystem::path folder = modsDir / name;
            const std::filesystem::path dllPath = folder / (name + ".dll");
            auto mod = std::make_unique<Mod>();
            mod->name = name;
            mod->language = language;
            mod->settingsPath = folder / "settings.ini";
            {
                std::ifstream json(folder / "mod.json");
                std::stringstream text;
                text << json.rdbuf();
                mod->description = jsonString(text.str(), language == "pt" ? "description_pt" : "description");
                if (mod->description.empty())
                    mod->description = jsonString(text.str(), "description");
            }
            loadSettings(*mod);
            mod->dll = LoadLibraryW(dllPath.wstring().c_str());
            if (!mod->dll)
            {
                std::cerr << "[mods] " << dllPath.string() << " did not load (error " << GetLastError() << ")" << std::endl;
                continue;
            }
            const auto init = reinterpret_cast<int (*)(const UrbanModHost *)>(GetProcAddress(mod->dll, "urban_mod_init"));
            if (!init)
            {
                std::cerr << "[mods] " << name << ": no urban_mod_init export" << std::endl;
                FreeLibrary(mod->dll);
                continue;
            }
            mod->host = UrbanModHost{URBAN_MOD_API_VERSION, mod->name.c_str(), mod->language.c_str(), hostLog, hostRead, hostWrite,
                                     hostAddPatches, hostSetPatchesEnabled, hostPatchesEnabled, hostSetTab, hostOnFrame,
                                     hostGetSetting, hostSetSetting, hostStateLoads};
            int kept = 0;
            {
                CurrentMod current(mod.get());
                guarded(*mod, "urban_mod_init", [&] { kept = init(&mod->host); });
            }
            if (!kept)
            {
                std::cerr << "[mods] " << name << " declined to load" << std::endl;
                FreeLibrary(mod->dll);
                continue;
            }
            std::cerr << "[mods] " << name << " loaded (" << mod->shown.size() << " patch option(s)"
                      << (mod->draw ? ", own tab" : "") << (mod->frame ? ", frame code" : "") << ")" << std::endl;
            Mod *raw = mod.get();
#if defined(URBAN_HAS_GAME_MENU)
            ps2x::menu::addPanel(name, [raw](const Pcsx2GsUi *ui) { drawTab(*raw, ui); });
#endif
            if (raw->frame)
                ps2x::pnach::addFrameHook([raw]
                {
                    if (raw->failed)
                        return;
                    CurrentMod current(raw);
                    guarded(*raw, "its frame code", [raw] { raw->frame(raw->frameUser); });
                });
            g_mods.push_back(std::move(mod));
        }
        return g_mods.size();
#else
        (void)modsDir;
        (void)dlls;
        (void)language;
        (void)rdram;
        return 0u;
#endif
    }
}
