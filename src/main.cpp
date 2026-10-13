#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_save_state.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_input_options.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "urban/anim_decoder.h"
#include "urban/call_profile.h"
#include "urban/call_trace.h"
#include "urban/function_hooks.h"
#include "urban/game_disc.h"
#include "urban/hang_dump.h"
#include "urban/native_profile.h"
#include "urban/thread_policy.h"
#include "urban/hle_bindings.h"
#include "urban/log.h"
#include "urban/mods.h"
#include "urban/mod_loader.h"
#include "runtime/ps2_game_menu.h"

#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <xmmintrin.h>
#include <pmmintrin.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <cwchar>
#include <csignal>
#include <cstdio>
#include <cstring>
#endif

// ps2_memory.cpp
void ps2xGsSetHandlerBusy(GSRegisters &gs, bool busy);

#if URBAN_HAS_VU1_PROGRAMS
void registerGeneratedVu1Programs(); // generated/vu1/vu1_programs.cpp
#endif

#if !defined(URBAN_ARCH_NAME)
#define URBAN_ARCH_NAME "SSE41"
#endif

namespace
{
#if defined(_WIN32)
    // The exe needs SSE4.1 (the PCSX2 minimum; the runtime uses SSE4.1 intrinsics), and AVX2 when built with
    // URBAN_ARCH=AVX2. Checked first thing in main, before any of that code runs, so an old CPU gets a message instead
    // of an illegal-instruction crash. The default build's own compiler output is SSE2 only.
    bool checkCpu()
    {
        int regs[4] = {};
        __cpuid(regs, 0);
        const int maxLeaf = regs[0];
        __cpuid(regs, 1);
        const bool sse41 = (regs[2] & (1 << 19)) != 0;
        const bool osAvx = (regs[2] & (1 << 27)) != 0 && (regs[2] & (1 << 28)) != 0 && (_xgetbv(0) & 6u) == 6u;
        bool avx2 = false;
        if (maxLeaf >= 7)
        {
            __cpuidex(regs, 7, 0);
            avx2 = osAvx && (regs[1] & (1 << 5)) != 0;
        }
        const bool needAvx2 = std::string_view(URBAN_ARCH_NAME) == "AVX2";
        std::fprintf(stderr, "[cpu] SSE4.1 %s, AVX2 %s; build %s\n", sse41 ? "yes" : "NO", avx2 ? "yes" : "no", URBAN_ARCH_NAME);
        const char *missing = !sse41 ? "SSE4.1" : (needAvx2 && !avx2 ? "AVX2" : nullptr);
        if (!missing)
            return true;
        wchar_t text[256];
        std::swprintf(text, 256,
                      L"This processor does not support %hs, which this build of Urban Reign needs.\n"
                      L"Este processador não tem %hs, que esta versão de Urban Reign precisa.", missing, missing);
        MessageBoxW(nullptr, text, L"Urban Reign", MB_OK | MB_ICONERROR);
        return false;
    }
#endif

    std::filesystem::path executableDirectory()
    {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            return std::filesystem::path(buffer).parent_path();
        }
#endif
        return std::filesystem::current_path();
    }

#if defined(_WIN32)
    void printStackTrace()
    {
        void *frames[48];
        const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
        for (USHORT i = 0; i < count; ++i)
        {
            std::memset(storage, 0, sizeof(storage));
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            const bool haveSymbol = SymFromAddr(process, address, &displacement, symbol) != FALSE;
            const bool haveLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != FALSE;
            std::fprintf(stderr, "[crash]   #%u %s+0x%llx %s:%lu\n", static_cast<unsigned>(i), haveSymbol ? symbol->Name : "?",
                         static_cast<unsigned long long>(displacement), haveLine ? line.FileName : "", haveLine ? line.LineNumber : 0ul);
        }
        std::fflush(stderr);
    }

    void installCrashReporter()
    {
        std::set_terminate([]()
                           {
                               std::fprintf(stderr, "[crash] std::terminate on thread %lu\n", GetCurrentThreadId());
                               if (const std::exception_ptr ep = std::current_exception())
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::fprintf(stderr, "[crash] exception: %s\n", e.what());
                                   }
                                   catch (...)
                                   {
                                       std::fprintf(stderr, "[crash] non-std exception\n");
                                   }
                               }
                               printStackTrace();
                               std::_Exit(3); });
        std::signal(SIGABRT, [](int)
                    {
                        std::fprintf(stderr, "[crash] abort on thread %lu\n", GetCurrentThreadId());
                        printStackTrace();
                        std::_Exit(3); });
    }
#endif

    // The EE FPU truncates and flushes denormals; PCSX2's default EE rounding mode is also Chop.
    void applyEeFloatingPointMode()
    {
        _MM_SET_ROUNDING_MODE(_MM_ROUND_TOWARD_ZERO);
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    }

    // Settings files next to urban.exe (what a launcher edits): controls.ini (the controllers, in PCSX2's format) and
    // config.ini ("[Video]" settings with friendly names, "[Runtime]" switches as environment variable names). Their
    // lines become the environment variables the runtime reads; a variable already set in the environment wins. Both
    // are written with their defaults when missing; config.ini also gets settings added in later versions appended.
    // Files from older versions (controls.ini/controles.ini with PS2X_BIND_ lines; config.ini keys like
    // ResolucaoInterna) are converted once with the same values, and the old file is kept as <name>.old.
    std::string trimText(const std::string &text)
    {
        const size_t begin = text.find_first_not_of(" \t\r");
        const size_t end = text.find_last_not_of(" \t\r");
        return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
    }

    std::string lowerText(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    struct KeyLine
    {
        std::string key;
        std::string value;
        bool active = true; // false: "#KEY=value" (an option shown but switched off)
    };

    // "KEY=value" or "#KEY=value" with KEY made of letters, digits and '_'; false for anything else.
    bool parseKeyLine(const std::string &line, KeyLine &out)
    {
        std::string text = trimText(line);
        out.active = true;
        if (!text.empty() && text[0] == '#')
        {
            out.active = false;
            text = trimText(text.substr(1));
        }
        const size_t equals = text.find('=');
        if (equals == std::string::npos || equals == 0)
            return false;
        const std::string key = trimText(text.substr(0, equals));
        // A name starts with a letter or '_' (a comment like "# 0 = off" is not a switched-off key).
        if (!std::isalpha(static_cast<unsigned char>(key[0])) && key[0] != '_')
            return false;
        for (const char c : key)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return false;
        out.key = key;
        out.value = trimText(text.substr(equals + 1));
        return true;
    }

    // The template with the values of `values` (lower-case key -> setting) in its key lines; the settings the template
    // has no line for are appended under `extraHeading`.
    std::string fillTemplate(const char *templateText, std::map<std::string, KeyLine> values, const char *extraHeading)
    {
        std::string result;
        std::istringstream lines(templateText);
        std::string line;
        while (std::getline(lines, line))
        {
            KeyLine templateKey;
            if (parseKeyLine(line, templateKey))
            {
                const auto it = values.find(lowerText(templateKey.key));
                if (it != values.end())
                {
                    result += (it->second.active ? "" : "#") + templateKey.key + "=" + it->second.value + "\n";
                    values.erase(it);
                    continue;
                }
            }
            result += line + "\n";
        }
        if (!values.empty())
        {
            result += std::string("\n") + extraHeading + "\n";
            for (const auto &[key, setting] : values)
                result += (setting.active ? "" : "#") + setting.key + "=" + setting.value + "\n";
        }
        return result;
    }

    // Keeps the old file as <name>.old and writes the converted one.
    void replaceSettingsFile(const std::filesystem::path &oldPath, const std::filesystem::path &newPath, const std::string &text)
    {
        std::error_code error;
        std::filesystem::path backup = oldPath;
        backup += ".old";
        std::filesystem::copy_file(oldPath, backup, std::filesystem::copy_options::overwrite_existing, error);
        std::ofstream(newPath, std::ios::binary | std::ios::trunc) << text;
        if (oldPath != newPath)
            std::filesystem::remove(oldPath, error);
        std::cerr << "[settings] " << oldPath.filename().string() << " converted to " << newPath.filename().string()
                  << " (old file: " << backup.filename().string() << ")" << std::endl;
    }

    constexpr const char *kDefaultConfig = R"(# Urban Reign settings (read when urban.exe starts; a launcher edits this file).
# An environment variable with the same effect (e.g. PS2X_GS_SCALE) wins over this file.

[Game]
# The disc image (.iso) of Urban Reign (SLUS-21209). Empty = look for one in the "game" folder.
Iso=

[Mods]
# PCSX2 patches (.pnach) from the "mods" folder next to urban.exe that are on, separated by ";".
Enabled=
# DLL mods that are on (folders of the "mods" folder: mods\<Name>\<Name>.dll), separated by ";". Their options are
# in the in-game menu (F2).
Dlls=

[Video]
# Internal resolution: 1 = native PS2 (640x448), 2 = 1280x896, 3 = 1920x1344, 4 = 2560x1792 (up to 8).
# Each step multiplies the graphics card's work (2 = 4 times, 3 = 9 times, 4 = 16 times).
InternalResolution=1
# Window maximized at start: 1 = yes, 0 = no.
StartMaximized=0
# Fullscreen (borderless) at start, like F11: 1 = yes, 0 = no.
StartFullscreen=0
# Renderer: pcsx2 = PCSX2's GS (pcsx2gs.dll, DX11; default); native = the port's own GS (the options below only apply
# to pcsx2).
Renderer=pcsx2
# FXAA anti-aliasing (light): 1 = on, 0 = off.
FXAA=1
# Texture filtering: 0 = nearest (pixels), 1 = forced bilinear, 2 = as the PS2 (default), 3 = forced except sprites.
TextureFiltering=2
# Trilinear filtering: -1 = automatic (default), 0 = off, 1 = as the PS2, 2 = forced.
TrilinearFiltering=-1
# Anisotropic filtering: 0 = off, 2, 4, 8 or 16.
AnisotropicFiltering=0
# Sharpening (AMD CAS): 0 = off, 1 = sharpen only, 2 = sharpen and resize; SharpeningStrength = 0 to 100.
Sharpening=0
SharpeningStrength=50
# Color correction: 1 = applies Brightness/Contrast/Saturation/Gamma (1 to 100; 50 = unchanged).
ColorCorrection=0
Brightness=50
Contrast=50
Saturation=50
Gamma=50
# TV shader: 0 = none, 1 = scanlines, 2 = diagonal, 3 = triangular, 4 = wave, 5 = CRT (Lottes), 6 = 4xRGSS, 7 = NxAGSS.
TVShader=0
# Screen filter (the image scaled to the window): 0 = none (sharp pixels), 1 = smooth (default), 2 = sharp.
ScreenFilter=1
# Dithering: 0 = off, 1 = scaled, 2 = unscaled (default), 3 = force 32 bit.
Dithering=2
# Blending accuracy: 0 = minimum ... 1 = basic (default) ... 5 = maximum (heavier).
BlendingAccuracy=1
# Stretch the image to the window: 1 = yes, 0 = keep 4:3 with black bars.
Stretch=0
# HD textures (PCSX2 texture packs, PNG/DDS): 1 = use them, 0 = no.
HDTextures=1
# Pack folder (the one with the "replacements" subfolder); empty = textures\SLUS-21209 next to urban.exe or in the repository.
HDTexturesFolder=
# Load every HD texture at start (1; uses a lot of memory) or as the game uses them (0).
PreloadHDTextures=0

[Runtime]
# FPS counter (F3 toggles it in game). 1 = on at start.
PS2X_SHOW_FPS=0

# Progressive scan (480p, up to 60 FPS), Michael's "Force Progressive Scan" patch. On by default.
# 0 = back to the interlaced mode (30 FPS).
URBAN_PROGRESSIVE=1

# IOP on its own thread (on by default). Remove the # below to go back to the old mode and compare.
#PS2X_IOP_THREAD=0

# VU1 and GS on their own threads (like PCSX2's "MTVU"): on by default. Remove the # below to go back to the old mode
# (everything on the game thread) and compare.
#PS2X_MTVU=0

# EE cycles per loop iteration (default 16). With 32 (the old value) frames alternated around the vblank: the counter
# said ~60 but only ~37-40 different images per second in the heliport. Remove the # to compare.
#PS2X_EE_LOOP_CYCLES=32

# PCSX2 patches (.pnach): files separated by ";". patch=1 lines are applied at every vblank (also after loading a save
# state). Remove the # and set the path to turn them on.
#PS2X_PNACH=
)";

    struct ConfigSetting
    {
        const char *section;
        const char *key;      // lower case
        const char *legacy;   // lower case, the Portuguese name of older versions
        const char *variable;
    };
    constexpr ConfigSetting kConfigSettings[] = {
        {"game", "iso", "iso", "URBAN_ISO"},
        {"mods", "enabled", "enabled", "URBAN_MODS"},
        {"mods", "dlls", "dlls", "URBAN_MOD_DLLS"},
        {"launcher", "language", "language", "URBAN_LANGUAGE"},
        {"video", "internalresolution", "resolucaointerna", "PS2X_GS_SCALE"},
        {"video", "startmaximized", "maximizada", "URBAN_START_MAXIMIZED"},
        {"video", "startfullscreen", "telacheia", "URBAN_START_FULLSCREEN"},
        {"video", "renderer", "renderizador", "PS2X_GS_RENDERER"},
        {"video", "fxaa", "fxaa", "PS2X_PCSX2_FXAA"},
        {"video", "texturefiltering", "filtrotexturas", "PS2X_PCSX2_TEXTURE_FILTERING"},
        {"video", "trilinearfiltering", "filtrotrilinear", "PS2X_PCSX2_TRILINEAR"},
        {"video", "anisotropicfiltering", "anisotropico", "PS2X_PCSX2_ANISOTROPY"},
        {"video", "sharpening", "nitidez", "PS2X_PCSX2_CAS"},
        {"video", "sharpeningstrength", "nitidezforca", "PS2X_PCSX2_CAS_SHARPNESS"},
        {"video", "colorcorrection", "ajustecores", "PS2X_PCSX2_SHADEBOOST"},
        {"video", "brightness", "brilho", "PS2X_PCSX2_BRIGHTNESS"},
        {"video", "contrast", "contraste", "PS2X_PCSX2_CONTRAST"},
        {"video", "saturation", "saturacao", "PS2X_PCSX2_SATURATION"},
        {"video", "gamma", "gama", "PS2X_PCSX2_GAMMA"},
        {"video", "tvshader", "shadertv", "PS2X_PCSX2_TV_SHADER"},
        {"video", "screenfilter", "filtrotela", "PS2X_PCSX2_PRESENT_FILTER"},
        {"video", "dithering", "pontilhado", "PS2X_PCSX2_DITHERING"},
        {"video", "blendingaccuracy", "precisaomistura", "PS2X_PCSX2_BLENDING"},
        {"video", "stretch", "esticar", "PS2X_PCSX2_STRETCH"},
        {"video", "hdtextures", "texturashd", "PS2X_PCSX2_TEXTURES"},
        {"video", "hdtexturesfolder", "pastatexturas", "PS2X_PCSX2_TEXTURE_DIR"},
        {"video", "preloadhdtextures", "precarregartexturas", "PS2X_PCSX2_TEXTURE_PRECACHE"},
    };

    // A config.ini with the Portuguese names of older versions: rewritten from the template with its values.
    void convertLegacyConfig(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        if (!file)
            return;
        std::map<std::string, KeyLine> values;
        bool legacy = false;
        std::string line;
        while (std::getline(file, line))
        {
            KeyLine setting;
            if (!parseKeyLine(line, setting) || !setting.active)
                continue;
            const std::string key = lowerText(setting.key);
            for (const ConfigSetting &known : kConfigSettings)
            {
                if (key == known.legacy && key != known.key)
                {
                    legacy = true;
                    setting.key = known.key;
                    if (key == "renderizador" && lowerText(setting.value) == "nativo")
                        setting.value = "native";
                }
            }
            values[lowerText(setting.key)] = setting;
        }
        file.close();
        if (legacy)
            replaceSettingsFile(path, path, fillTemplate(kDefaultConfig, values, "# Other settings (from the old config.ini)"));
    }

    // True when the .pnach list comes from config.ini [Mods] Enabled (not from a PS2X_PNACH set by hand).
    bool g_pnachFromMods = false;

    void loadConfigFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::exists(path))
            std::ofstream(path, std::ios::binary) << kDefaultConfig;
        else
            convertLegacyConfig(path);
        std::ifstream file(path);
        if (!file)
            return;
        std::string section;
        std::string line;
        std::set<std::string> present; // "section/key" found in the file
        while (std::getline(file, line))
        {
            line = trimText(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = lowerText(trimText(line.substr(1, line.size() - 2)));
                continue;
            }
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = trimText(line.substr(0, equals));
            const std::string value = trimText(line.substr(equals + 1));
            present.insert(section + "/" + lowerText(key));
            std::string variable;
            for (const ConfigSetting &setting : kConfigSettings)
                if (section == setting.section && (lowerText(key) == setting.key || lowerText(key) == setting.legacy))
                    variable = setting.variable;
            if (variable.empty() && (key.rfind("PS2X_", 0) == 0 || key.rfind("URBAN_", 0) == 0))
                variable = key;
            if (variable.empty() || value.empty() || std::getenv(variable.c_str()))
                continue;
#if defined(_WIN32)
            _putenv_s(variable.c_str(), value.c_str());
#else
            setenv(variable.c_str(), value.c_str(), 0);
#endif
        }
        file.close();

        // Settings added in later versions: appended to an older file with their default and comments (the values
        // already there are left alone), so the user and the launcher see them.
        std::string missing;
        std::string comments;
        std::string templateSection;
        std::string lastWrittenSection;
        std::istringstream defaults(kDefaultConfig);
        while (std::getline(defaults, line))
        {
            const std::string text = trimText(line);
            if (text.empty())
            {
                comments.clear();
                continue;
            }
            if (text[0] == '#' || text[0] == ';')
            {
                comments += line + "\n";
                continue;
            }
            if (text.front() == '[' && text.back() == ']')
            {
                templateSection = lowerText(trimText(text.substr(1, text.size() - 2)));
                comments.clear();
                continue;
            }
            const size_t equals = text.find('=');
            if (equals != std::string::npos && !present.count(templateSection + "/" + lowerText(trimText(text.substr(0, equals)))))
            {
                if (templateSection != lastWrittenSection)
                {
                    missing += "\n[" + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(templateSection[0])))) +
                               templateSection.substr(1) + "]\n";
                    lastWrittenSection = templateSection;
                }
                missing += comments + text + "\n";
                // The default also applies to this run (the file was read before it was appended).
                const std::string key = lowerText(trimText(text.substr(0, equals)));
                const std::string value = trimText(text.substr(equals + 1));
                for (const ConfigSetting &setting : kConfigSettings)
                {
                    if (templateSection == setting.section && key == setting.key && !value.empty() && !std::getenv(setting.variable))
                    {
#if defined(_WIN32)
                        _putenv_s(setting.variable, value.c_str());
#else
                        setenv(setting.variable, value.c_str(), 0);
#endif
                    }
                }
            }
            comments.clear();
        }
        if (!missing.empty())
        {
            std::ofstream out(path, std::ios::app);
            out << "\n# New settings (added by urban.exe with their default value)" << missing;
        }

        // [Mods] Enabled: the .pnach files of the "mods" folder, as PS2X_PNACH (unless that is set itself). Then the
        // list is watched while the game runs (startPnachWatch).
        if (!std::getenv("PS2X_PNACH"))
        {
            g_pnachFromMods = true;
            const char *mods = std::getenv("URBAN_MODS");
            std::string files;
            for (const std::string &file : urban::mods::pnachFiles(path, mods ? mods : "", true))
                files += (files.empty() ? "" : ";") + file;
            if (!files.empty())
            {
#if defined(_WIN32)
                _putenv_s("PS2X_PNACH", files.c_str());
#else
                setenv("PS2X_PNACH", files.c_str(), 0);
#endif
            }
        }
    }

    // While the game runs: the enabled .pnach files are reloaded when config.ini [Mods] Enabled (written by a
    // launcher) or one of the files changes, or on F4 (ps2_pnach.cpp). A list set by hand in PS2X_PNACH stays fixed;
    // only its files are watched.
    void startPnachWatch(const std::filesystem::path &configPath)
    {
        if (g_pnachFromMods)
        {
            ps2x::pnach::watch([configPath] { return urban::mods::pnachFiles(configPath, urban::mods::readEnabled(configPath), false); });
            return;
        }
        std::vector<std::string> fixed;
        std::istringstream list(std::getenv("PS2X_PNACH") ? std::getenv("PS2X_PNACH") : "");
        std::string file;
        while (std::getline(list, file, ';'))
            if (!(file = trimText(file)).empty())
                fixed.push_back(file);
        ps2x::pnach::watch([fixed] { return fixed; });
    }

    // controls.ini: PCSX2's controller sections ([InputSources], [Pad], [Pad1]-[Pad8], as in PCSX2.ini, so they can be
    // copied from there). They become environment variables: [InputSources] SDL -> PS2X_SDL_INPUT, [Pad]
    // MultitapPort1/2 -> PS2X_MULTITAP, [PadN] -> PS2X_PADN=1 and PS2X_PADN_<KEY>=<binding>;<binding>... (a key
    // repeated on several lines, like PCSX2 writes extra bindings).
    constexpr const char *kControlsHeader = R"(# Urban Reign controllers (read when urban.exe starts; a launcher edits this file).
# Same format as PCSX2's controller settings: the [Pad] and [Pad1]...[Pad8] sections of PCSX2.ini can be copied here.
#   <Button> = <Source>/<Element>; several lines with the same button = any of them presses it; "a & b" = both held.
#   Keyboard/<Key>: A-Z, 0-9, F1-F12, Up, Down, Left, Right, Space, Return, Tab, Backspace, Escape, Shift, Control,
#                   Alt, LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt, Insert, Delete, Home, End,
#                   PageUp, PageDown, Comma, Period, Minus, Equal, Slash, Semicolon, Keypad+0 ... Keypad+9
#   Pointer-0/LeftButton, RightButton, MiddleButton: mouse buttons.
#   SDL-<n>/<Element>: controller n. Like PCSX2, an Xbox controller keeps its XInput number and the others take the
#                   lowest free one; urban.exe prints "SDL-<n>: <name>" in the console when a controller connects.
#     Buttons: FaceSouth (Cross / A), FaceEast (Circle / B), FaceWest (Square / X), FaceNorth (Triangle / Y), DPadUp,
#              DPadDown, DPadLeft, DPadRight, Back, Start, Guide, LeftShoulder, RightShoulder, LeftStick, RightStick,
#              Misc1, Paddle1-4, Touchpad
#     Axes: +LeftTrigger, +RightTrigger, -LeftX, +LeftX, -LeftY (up), +LeftY (down), -RightX, +RightX, -RightY, +RightY
#   LargeMotor / SmallMotor = SDL-<n>/LargeMotor / SmallMotor: the controller that vibrates (remove the line = no
#   vibration); LargeMotorScale / SmallMotorScale: strength (1 = normal, 0 = off).
#   Deadzone (0-0.95) and AxisScale (1.33 = PCSX2's default) for the sticks; InvertL / InvertR: 1 = X, 2 = Y, 3 = both.
#   Type = None: no controller in that port/slot. Without a multitap only Pad1 and Pad2 are used.

[InputSources]
# Controllers through SDL (vibration; PlayStation controllers without Steam/DS4Windows): true. false = the old input
# (GLFW, no vibration; SDL-<n> is then GLFW's gamepad n).
SDL = true

[Pad]
# Multitap (four controllers on one port), like PCSX2's. Urban Reign reads the extra players on port 2.
# Players in PCSX2's order: Pad1 = port 1, Pad2 = port 2, then the multitap slots (port 1 B-D, then port 2 B-D).
MultitapPort1 = false
MultitapPort2 = true
)";

    // The [Pad1]-[Pad8] sections with the runtime's defaults; `pad1Keys` replaces Pad1's keyboard and mouse bindings
    // (key -> binding, from an older controls.ini).
    std::string defaultPadSections(const std::multimap<std::string, std::string> *pad1Keys)
    {
        std::string text;
        for (uint32_t pad = 0; pad < 8u; ++pad)
        {
            text += "\n[Pad" + std::to_string(pad + 1u) + "]\n";
            std::set<std::string> replaced;
            for (const auto &[key, value] : ps2_host_input::defaultPadSettings(pad))
            {
                const bool hostBinding = value.rfind("Keyboard/", 0) == 0 || value.rfind("Pointer-", 0) == 0;
                if (pad == 0u && pad1Keys && hostBinding && pad1Keys->count(key))
                {
                    if (replaced.insert(key).second)
                    {
                        const auto range = pad1Keys->equal_range(key);
                        for (auto it = range.first; it != range.second; ++it)
                            text += key + " = " + it->second + "\n";
                    }
                    continue;
                }
                text += key + " = " + value + "\n";
            }
        }
        return text;
    }

    // A key name of the old PS2X_BIND_<BUTTON>=<KEY> lines as a binding of this file ("" for NONE or unknown).
    std::string legacyKeyBinding(std::string name)
    {
        for (char &c : name)
            c = static_cast<char>(c == '-' || c == ' ' ? '_' : std::toupper(static_cast<unsigned char>(c)));
        if (name.size() == 1u && std::isalnum(static_cast<unsigned char>(name[0])))
            return "Keyboard/" + name;
        if (name.size() >= 2u && name[0] == 'F' && std::isdigit(static_cast<unsigned char>(name[1])))
            return "Keyboard/" + name;
        static const std::map<std::string, std::string> kNames{
            {"UP", "Keyboard/Up"}, {"DOWN", "Keyboard/Down"}, {"LEFT", "Keyboard/Left"}, {"RIGHT", "Keyboard/Right"},
            {"SPACE", "Keyboard/Space"}, {"BACKSPACE", "Keyboard/Backspace"}, {"ENTER", "Keyboard/Return"},
            {"TAB", "Keyboard/Tab"}, {"ESCAPE", "Keyboard/Escape"}, {"LEFT_SHIFT", "Keyboard/LeftShift"},
            {"RIGHT_SHIFT", "Keyboard/RightShift"}, {"SHIFT", "Keyboard/Shift"}, {"LEFT_CTRL", "Keyboard/LeftControl"},
            {"RIGHT_CTRL", "Keyboard/RightControl"}, {"LEFT_CONTROL", "Keyboard/LeftControl"},
            {"RIGHT_CONTROL", "Keyboard/RightControl"}, {"CTRL", "Keyboard/Control"}, {"CONTROL", "Keyboard/Control"},
            {"MOUSE_LEFT", "Pointer-0/LeftButton"}, {"LMB", "Pointer-0/LeftButton"},
            {"MOUSE_RIGHT", "Pointer-0/RightButton"}, {"RMB", "Pointer-0/RightButton"},
            {"MOUSE_MIDDLE", "Pointer-0/MiddleButton"}};
        const auto it = kNames.find(name);
        return it != kNames.end() ? it->second : std::string();
    }

    // config.ini without its [Controls] section (Multitap moved to controls.ini [Pad]); returns that Multitap value.
    std::string removeConfigControls(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        if (!file)
            return {};
        std::vector<std::string> kept;
        std::string multitap;
        bool inControls = false;
        bool found = false;
        std::string line;
        while (std::getline(file, line))
        {
            const std::string text = trimText(line);
            if (!text.empty() && text.front() == '[' && text.back() == ']')
            {
                inControls = lowerText(text) == "[controls]";
                if (inControls)
                {
                    found = true;
                    // The heading urban.exe wrote above the section when it appended it goes too.
                    while (!kept.empty() && (trimText(kept.back()).empty() || kept.back().rfind("# New settings (added by urban.exe", 0) == 0))
                        kept.pop_back();
                    continue;
                }
            }
            if (inControls)
            {
                KeyLine setting;
                if (parseKeyLine(line, setting) && setting.active && lowerText(setting.key) == "multitap")
                    multitap = setting.value;
                continue;
            }
            kept.push_back(line);
        }
        file.close();
        if (found)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            for (const std::string &keptLine : kept)
                out << keptLine << "\n";
        }
        return multitap;
    }

    // controls.ini (or controles.ini) of older versions: "PS2X_BIND_<BUTTON>=<KEY>" lines plus runtime switches. The
    // bindings become Pad1's keyboard bindings, the switches move to config.ini [Runtime], the old file is kept as
    // <name>.old.
    void convertLegacyControls(const std::filesystem::path &legacy, const std::filesystem::path &path, const std::filesystem::path &configPath)
    {
        std::map<std::string, KeyLine> switches;
        std::multimap<std::string, std::string> pad1Keys;
        static const std::map<std::string, std::string> kButtons{
            {"TRIANGLE", "Triangle"}, {"SQUARE", "Square"}, {"CIRCLE", "Circle"}, {"CROSS", "Cross"}, {"UP", "Up"},
            {"DOWN", "Down"}, {"LEFT", "Left"}, {"RIGHT", "Right"}, {"L1", "L1"}, {"R1", "R1"}, {"L2", "L2"},
            {"R2", "R2"}, {"L3", "L3"}, {"R3", "R3"}, {"START", "Start"}, {"SELECT", "Select"}};
        std::set<std::string> boundButtons;
        std::ifstream old(legacy);
        std::string line;
        while (std::getline(old, line))
        {
            KeyLine setting;
            if (!parseKeyLine(line, setting))
                continue;
            std::string upper = setting.key;
            for (char &c : upper)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (upper.rfind("PS2X_BIND_", 0) == 0)
            {
                const auto button = kButtons.find(upper.substr(10));
                if (button == kButtons.end() || !setting.active || !boundButtons.insert(button->second).second)
                    continue;
                // NONE (""): the button keeps only its controller binding; the empty line is removed below.
                pad1Keys.emplace(button->second, legacyKeyBinding(setting.value));
                continue;
            }
            const std::string key = lowerText(setting.key);
            const auto it = switches.find(key);
            if (it == switches.end() || setting.active || !it->second.active)
                switches[key] = setting;
        }
        old.close();

        std::string multitap = removeConfigControls(configPath);
        std::string controls = kControlsHeader;
        if (!multitap.empty())
        {
            const auto set = [&](const char *key, bool on)
            {
                const std::string from = std::string(key) + (on ? " = false" : " = true");
                const size_t at = controls.find(from);
                if (at != std::string::npos)
                    controls.replace(at, from.size(), std::string(key) + (on ? " = true" : " = false"));
            };
            set("MultitapPort1", multitap.find('1') != std::string::npos);
            set("MultitapPort2", multitap.find('2') != std::string::npos);
        }
        std::string pads = defaultPadSections(&pad1Keys);
        // Remove the empty "Key = " lines left by the NONE bindings.
        for (size_t at = pads.find(" = \n"); at != std::string::npos; at = pads.find(" = \n"))
        {
            const size_t begin = pads.rfind('\n', at) + 1u;
            pads.erase(begin, at + 4u - begin);
        }

        std::error_code error;
        std::filesystem::path backup = legacy;
        backup += ".old";
        std::filesystem::copy_file(legacy, backup, std::filesystem::copy_options::overwrite_existing, error);
        std::ofstream(path, std::ios::binary | std::ios::trunc) << controls << pads;
        if (legacy != path)
            std::filesystem::remove(legacy, error);

        // The runtime switches: config.ini [Runtime] with the old values (a new config.ini gets the whole template).
        const char *runtimeStart = std::strstr(kDefaultConfig, "[Runtime]");
        if (!std::filesystem::exists(configPath))
            std::ofstream(configPath, std::ios::binary) << fillTemplate(kDefaultConfig, switches, "# Other settings (from the old controls.ini)");
        else
            std::ofstream(configPath, std::ios::binary | std::ios::app)
                << "\n" << fillTemplate(runtimeStart, switches, "# Other settings (from the old controls.ini)");
        std::cerr << "[settings] " << legacy.filename().string() << " converted to " << path.filename().string()
                  << " (PCSX2 format) and config.ini [Runtime] (old file: " << backup.filename().string() << ")" << std::endl;
    }

    void setDefaultEnv(const std::string &variable, const std::string &value)
    {
        if (value.empty() || std::getenv(variable.c_str()))
            return;
#if defined(_WIN32)
        _putenv_s(variable.c_str(), value.c_str());
#else
        setenv(variable.c_str(), value.c_str(), 0);
#endif
    }

    void loadControlsFile(const std::filesystem::path &exeDir)
    {
        const std::filesystem::path path = exeDir / "controls.ini";
        const std::filesystem::path legacy = exeDir / "controles.ini";
        const std::filesystem::path configPath = exeDir / "config.ini";
        bool hasPads = false;
        if (std::filesystem::exists(path))
        {
            std::ifstream file(path);
            std::string line;
            while (std::getline(file, line) && !hasPads)
                hasPads = lowerText(trimText(line)).rfind("[pad", 0) == 0;
        }
        if (std::filesystem::exists(path) && !hasPads)
            convertLegacyControls(path, path, configPath);
        else if (!std::filesystem::exists(path) && std::filesystem::exists(legacy))
            convertLegacyControls(legacy, path, configPath);
        else if (!std::filesystem::exists(path))
            std::ofstream(path, std::ios::binary) << kControlsHeader << defaultPadSections(nullptr);

        std::ifstream file(path);
        std::string section;
        std::string line;
        std::map<std::string, std::string> values; // variable -> bindings joined with ';'
        while (std::getline(file, line))
        {
            line = trimText(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = lowerText(trimText(line.substr(1, line.size() - 2)));
                if (section.size() == 4u && section.rfind("pad", 0) == 0 && section[3] >= '1' && section[3] <= '8')
                    values["PS2X_PAD" + section.substr(3)] = "1";
                continue;
            }
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = lowerText(trimText(line.substr(0, equals)));
            const std::string value = trimText(line.substr(equals + 1));
            if (section == "inputsources" && key == "sdl")
                values["PS2X_SDL_INPUT"] = lowerText(value) == "false" || value == "0" ? "0" : "1";
            else if (section == "pad" && (key == "multitapport1" || key == "multitapport2"))
            {
                std::string &ports = values["PS2X_MULTITAP"];
                const bool on = lowerText(value) == "true" || value == "1";
                const char port = key.back();
                if (on && ports.find(port) == std::string::npos)
                    ports += port;
            }
            else if (section.size() == 4u && section.rfind("pad", 0) == 0 && section[3] >= '1' && section[3] <= '8')
            {
                std::string variable = "PS2X_PAD" + section.substr(3) + "_";
                for (const char c : key)
                    variable += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                std::string &joined = values[variable];
                joined += (joined.empty() ? "" : ";") + value;
            }
        }
        if (values.count("PS2X_MULTITAP") && values["PS2X_MULTITAP"].empty())
            values["PS2X_MULTITAP"] = "0";
        for (const auto &[variable, value] : values)
            setDefaultEnv(variable, value);
    }

    // Progressive scan, on by default (URBAN_PROGRESSIVE=0 keeps the interlaced 30 FPS mode). Michael's "Force
    // Progressive Scan" patch from PCSX2's patches.zip turns the two video-setup
    // calls at 0x1372e0 (jal 0x315c88) and 0x1372e8 (jal 0x1370c8) into jal 0x1373b0. That is the game's own
    // progressive path: sceGsResetGraph(0, 0, 0x50 = 480p, 1). The code is recompiled, so instead of patching the
    // instructions both callees are wrapped and redirected when called from exactly those return addresses.
    PS2Runtime::RecompiledFunction g_progressiveTarget = nullptr;
    PS2Runtime::RecompiledFunction g_videoSetupA = nullptr; // 0x315c88
    PS2Runtime::RecompiledFunction g_videoSetupB = nullptr; // 0x1370c8

    void enterProgressive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = 0x1373b0u;
        g_progressiveTarget(rdram, ctx, runtime);
    }

    void videoSetupA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x1372e8u)
            return enterProgressive(rdram, ctx, runtime);
        g_videoSetupA(rdram, ctx, runtime);
    }

    void videoSetupB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x1372f0u)
            return enterProgressive(rdram, ctx, runtime);
        g_videoSetupB(rdram, ctx, runtime);
    }

    void installProgressivePatch(PS2Runtime &runtime)
    {
        const char *setting = std::getenv("URBAN_PROGRESSIVE");
        if (setting && *setting == '0')
            return;
        g_progressiveTarget = runtime.lookupFunction(0x1373b0u);
        g_videoSetupA = runtime.lookupFunction(0x315c88u);
        g_videoSetupB = runtime.lookupFunction(0x1370c8u);
        if (!g_progressiveTarget || !g_videoSetupA || !g_videoSetupB)
        {
            URBAN_ERROR(Boot, "progressive scan patch: recompiled functions missing");
            return;
        }
        auto &hooks = urban::FunctionHooks::instance();
        hooks.replace(0x315c88u, "progressive_video_setup_a", &videoSetupA, urban::ReplacementStatus::Native, "Force Progressive Scan");
        hooks.replace(0x1370c8u, "progressive_video_setup_b", &videoSetupB, urban::ReplacementStatus::Native, "Force Progressive Scan");
        URBAN_INFO(Boot, "progressive scan (480p) forced, as PCSX2's Force Progressive Scan patch");
    }

    // MTVU: the GS interrupt handler (0x10bc38) read-modify-writes SIGLBLID, so the GIF thread must not apply a SIGNAL
    // while it runs (ps2xGsSetHandlerBusy in ps2_memory.cpp). With the GS on the game thread nothing can interleave.
    PS2Runtime::RecompiledFunction g_gsHandler = nullptr;

    void gsHandlerExclusive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        GSRegisters &gs = runtime->memory().gs();
        const bool async = ps2x::asyncvif::active(runtime->memory());
        ps2x::asyncvif::traceEvent('H', static_cast<uint32_t>(gs.siglblid));
        if (async)
            ps2xGsSetHandlerBusy(gs, true);
        g_gsHandler(rdram, ctx, runtime);
        if (async)
            ps2xGsSetHandlerBusy(gs, false);
        ps2x::asyncvif::traceEvent('h');
    }

    void installGsHandlerGuard(PS2Runtime &runtime)
    {
        g_gsHandler = runtime.lookupFunction(0x10bc38u);
        if (!g_gsHandler)
            return;
        urban::FunctionHooks::instance().replace(0x10bc38u, "gs_handler_exclusive", &gsHandlerExclusive, urban::ReplacementStatus::Native,
                                                 "MTVU: SIGNAL waits for the GS handler");
    }

    void printUsage()
    {
        std::cout << "usage: urban [--iso <path-to-" << urban::kSerial << ".iso>]\n"
                  << "environment: URBAN_ISO=<path>, URBAN_TRACE=BOOT,LOADER,FILE,EE,IOP,GS,VU,SPU2,INPUT,GAME,HOOK|ALL,\n"
                  << "             URBAN_MISSING_FUNCTION=continue\n";
    }
}

int main(int argc, char *argv[])
{
#if defined(_WIN32)
    if (!checkCpu())
        return 4;
    installCrashReporter();
#endif
    const std::filesystem::path exeDir = executableDirectory();
    loadControlsFile(exeDir); // controls.ini, PCSX2 format (converted from the controls.ini/controles.ini of older versions)
    loadConfigFile(exeDir / "config.ini");
    // --write-settings: only write (or complete/convert) controls.ini and config.ini with their comments and defaults,
    // then exit. The launcher runs it before editing them for the first time.
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--write-settings")
            return 0;
    urban::log::configureFromEnvironment();
    urban::hangdump::startFromEnvironment();
    urban::nativeprofile::startFromEnvironment();
    urban::nativeprofile::startCaptureHotkey(exeDir); // F6: start/stop a profile capture (perfil_<n>.txt)
    urban::threadpolicy::startFromEnvironment();      // PS2X_THREAD_POLICY: cores and priority by CPU topology

    std::filesystem::path iso;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if ((arg == "--iso" || arg == "-iso") && i + 1 < argc)
        {
            iso = argv[++i];
        }
        else if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
        else if (iso.empty() && arg.rfind("-", 0) != 0)
        {
            iso = arg;
        }
    }

    if (iso.empty())
    {
        if (const char *env = std::getenv("URBAN_ISO"))
        {
            iso = env;
        }
    }
    if (iso.empty())
    {
        iso = urban::findDiscImage({exeDir / "game", exeDir.parent_path() / "game", exeDir.parent_path().parent_path() / "game",
                                    exeDir.parent_path().parent_path().parent_path() / "game",
                                    std::filesystem::current_path() / "game", std::filesystem::current_path()});
    }
    if (iso.empty())
    {
        URBAN_ERROR(Boot, "no disc image found. Pass --iso <path> to your own " << urban::kSerial << " disc image.");
        printUsage();
        return 2;
    }

    const std::filesystem::path cacheDir = exeDir / "game_data";
    URBAN_INFO(Boot, "disc image: " << iso.string());
    const urban::DiscCheckResult disc = urban::verifyDiscAndPrepare(iso, cacheDir);
    if (!disc.ok)
    {
        URBAN_ERROR(Boot, disc.error);
        return 3;
    }

    PS2Runtime::IoPaths paths;
    paths.elfPath = disc.bootElfPath;
    paths.elfDirectory = cacheDir;
    paths.hostRoot = cacheDir;
    paths.cdRoot = cacheDir;
    paths.mcRoot = exeDir / "memcards" / "mc0";
    // URBAN_MC_DIR=<folder>: memory cards (Mcd001.ps2) from another folder; the test drivers use a copy of the card.
    if (const char *mcDir = std::getenv("URBAN_MC_DIR"); mcDir && *mcDir)
        paths.mcRoot = std::filesystem::path(mcDir) / "mc0";
#if defined(_WIN32)
    if (!std::getenv("PS2X_GS_SHADER_CACHE_DIR"))
        _putenv_s("PS2X_GS_SHADER_CACHE_DIR", (cacheDir / "gs_shaders").string().c_str());
#endif
    paths.cdImage = std::filesystem::absolute(iso);
    std::error_code error;
    std::filesystem::create_directories(paths.mcRoot, error);
    // URBAN_QUICK_STATE=<file>: another F5/F9 slot (the autotest driver uses its own, not the player's).
    const char *quickState = std::getenv("URBAN_QUICK_STATE");
    ps2x::savestate::setQuickSlotPath(quickState && *quickState ? std::filesystem::path(quickState) : exeDir / "states" / "quick.state");

    GSThreadedBackend::SetEnabledByDefault(true);
    GSThreadedBackend::SetGpuByDefault(true);
    GSGpuBackend::SetAsyncPresentDefault(true);
    // Synchronous VIF1/VU1/GIF by default. Urban Reign reads/writes GS and VIF registers many times per frame (SIGNAL
    // handler 0x10bc38, DMA kicks) and each access waited for the async workers, so the game thread and VU thread ran in
    // series anyway, plus the hand-off cost: in a fight 34 fields/s async vs 46 fields/s sync (2026-10-07).
    // PS2X_MTVU=1 turns the async workers back on.
    // 2026-10-08: on by default again (PS2X_MTVU=0 for the synchronous mode). With the PCSX2-like timing model
    // (asyncvif::vblankWaitEnabled: the vblank waits for the workers, emulated time does not run ahead of real time while
    // they work) the heaviest 4-fighter map runs at ~59 FPS (45.7 synchronous) and the 4-fighter select at 60
    // (docs/PROGRESS.md §50).
    ps2x::asyncvif::setDefaultEnabled(true);
    ps2x::asyncvif::setHostPacingDefault(true);
    EeScheduler::setHostPacingDefault(true);

#if defined(_WIN32)
    // Urban Reign uses libmc 2.x with the old MCSERV; the runtime's MCSERV HLE speaks the newer XMCSERV protocol and
    // reads a folder card. Default to the game's own MCMAN/MCSERV IRX over the emulated SIO2 card (Mcd001.ps2).
    if (!std::getenv("PS2X_IOP_HLE_MCSERV"))
        _putenv_s("PS2X_IOP_HLE_MCSERV", "0");
    // PADMAN reads both pad ports in one SIO2 transfer; with port 2 empty the sticky "no response" RECV1 made it drop
    // port 1 too (stuck re-sending 0x42). Report RECV1 as connected when any command of the transfer was answered.
    if (!std::getenv("PS2X_SIO2_RECV1"))
        _putenv_s("PS2X_SIO2_RECV1", "2");
    // EE cycles charged per loop iteration of the recompiled code (runtime default 32; straight-line code costs nothing).
    // The game starts each frame 15 lines (T3 H-BLANK count 510 of 525) before the vblank, with interrupts disabled while
    // it kicks the frame. At 32 that stretch cost ~300K cycles, longer than the 15 lines: the vblank fell inside it, the
    // vblank handler ran late and stored a late T3 reference, so the next frame started after the vblank instead of
    // before it; frames alternated around the vblank, two in one field and none in the next. Heliport, 4 fighters:
    // ~37 distinct images/s at "60 FPS"; at 16: ~52 at 55 (93% -> 91% speed), the other heavy map 50 -> 59 at 60, the
    // select screen 52 -> 59 (docs/PROGRESS.md §52). PS2X_EE_LOOP_CYCLES=32 restores the old timing.
    if (!std::getenv("PS2X_EE_LOOP_CYCLES"))
        _putenv_s("PS2X_EE_LOOP_CYCLES", "16");
    // The finished image stays on the GPU: the GS thread's context shares textures with the window's, so no copy to RAM
    // and back each frame (the runtime's default is still the copy). Same frame rate with 6 cores, ~20% of a core less
    // CPU, and more distinct images on smaller machines (docs/PROGRESS.md §60). The SotC port ships with it on.
    // PS2X_GS_DIRECT_PRESENT=0 restores the copy path.
    if (!std::getenv("PS2X_GS_DIRECT_PRESENT"))
        _putenv_s("PS2X_GS_DIRECT_PRESENT", "1");
    // Both VRAM downloads of a fight are the light flare's, at the sun's screen position, once a frame: an 8x8 depth
    // block (occlusion) and up to 8x8 of a half-size color copy of the frame (0x3480, CT24; the glare's brightness).
    // The runtime defaults to asynchronous depth only; "all" also gives the color block one frame late instead of making
    // the game thread wait for the GS thread and the GPU: 8 fighters 54.8 -> 58.2 frames/s (docs/PROGRESS.md §63).
    // PS2X_GS_ASYNC_READBACK=1 keeps depth only, =0 reads both synchronously.
    if (!std::getenv("PS2X_GS_ASYNC_READBACK"))
        _putenv_s("PS2X_GS_ASYNC_READBACK", "all");
#endif

    try
    {
        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI)
        // ImGui runtime debugger from PS2Recomp: F1 toggles it; URBAN_DEBUG_UI=1 opens it at start-up.
        static PS2DebugPanel debugPanel;
        {
            const char *openAtStart = std::getenv("URBAN_DEBUG_UI");
            debugPanel.setVisible(openAtStart && *openAtStart == '1');
        }
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->initialize(); },
            [](PS2Runtime &rt, void *userData)
            {
                auto *panel = static_cast<PS2DebugPanel *>(userData);
                panel->draw(rt);
                // With PCSX2's GS its window covers this one: hidden while the debugger is open.
                GSPcsx2Backend::SetHostUiVisible(panel->isVisible());
            },
            [](PS2Runtime &, void *userData) { static_cast<PS2DebugPanel *>(userData)->shutdown(); },
            &debugPanel);
#endif
        {
            const char *maximized = std::getenv("URBAN_START_MAXIMIZED"); // config.ini [Video] Maximizada
            ps2_host_display::setStartMaximized(maximized && *maximized == '1');
            const char *fullscreen = std::getenv("URBAN_START_FULLSCREEN"); // config.ini [Video] StartFullscreen
            ps2_host_display::setStartFullscreen(fullscreen && *fullscreen == '1');
        }
        if (!runtime.initialize("Urban Reign"))
        {
            URBAN_ERROR(Boot, "runtime initialization failed");
            return 1;
        }
        URBAN_INFO(Boot, "runtime initialized; loading " << disc.bootElfPath.string());
        if (!runtime.loadELF(disc.bootElfPath.string()))
        {
            URBAN_ERROR(Boot, "failed to load " << disc.bootElfPath.string());
            return 1;
        }
        URBAN_INFO(Boot, "ELF loaded, entry 0x" << std::hex << runtime.cpu().pc);
        // PS2X_PNACH=<file>[;<file>...] (controls.ini): PCSX2 patches; patch=0 lines now, patch=1 at every vblank.
        if (ps2x::pnach::loadFromEnvironment() != 0u)
            ps2x::pnach::applyOnce(runtime.memory().getRDRAM());
        startPnachWatch(exeDir / "config.ini");
        // F2: the in-game menu (PCSX2's GS only), with the mods tab.
        {
            const char *language = std::getenv("URBAN_LANGUAGE");
            const bool portuguese = language && std::string(language) == "pt";
            ps2x::menu::setTitle("MODS / PATCHES", portuguese ? "F2 fecha o menu. O jogo continua rodando."
                                                              : "F2 closes the menu. The game keeps running.");
            ps2x::menu::setEmptyText(portuguese ? "Nenhum mod ligado. Ligue os mods na aba Mods do launcher (config.ini [Mods] Dlls)."
                                                : "No mod is on. Switch mods on in the launcher's Mods tab (config.ini [Mods] Dlls).");
            const char *dlls = std::getenv("URBAN_MOD_DLLS");
            urban::modloader::loadAll(exeDir / "mods", dlls ? dlls : "", portuguese ? "pt" : "en", runtime.memory().getRDRAM());
        }
        PS2Runtime::setIoPaths(paths);

        const char *missingPolicy = std::getenv("URBAN_MISSING_FUNCTION");
        runtime.setMissingFunctionPolicy(missingPolicy && std::string(missingPolicy) == "continue"
                                             ? PS2Runtime::MissingFunctionPolicy::ContinueToTarget
                                             : PS2Runtime::MissingFunctionPolicy::Stop);
        urban::FunctionHooks::instance().attach(runtime);
        urban::FunctionHooks::instance().observeEntry(runtime.cpu().pc, "boot_entry", [](uint8_t *, R5900Context *, PS2Runtime *) {
            applyEeFloatingPointMode();
            URBAN_INFO(Ee, "EE FPU host mode: round toward zero, denormals flushed");
        });
        urban::installHleBindings(runtime);
#if URBAN_HAS_VU1_PROGRAMS
        registerGeneratedVu1Programs(); // VU1 microcode compiled ahead of time (generated/vu1)
#endif
        installProgressivePatch(runtime);
        installGsHandlerGuard(runtime);
        urban::animdecoder::install(runtime);
        // FPS overlay (F3): 0x128dc0 runs once per iteration of the game's main loop (one game frame).
        urban::FunctionHooks::instance().observeEntry(0x128dc0u, "game_frame", [](uint8_t *, R5900Context *, PS2Runtime *runtime) {
            ps2_host_display::noteGameFrame();
            urban::nativeprofile::noteGameFrame();
            ps2x::asyncvif::traceEvent('F', static_cast<uint32_t>(runtime->eeScheduler().currentCycle() >> 10)); // EE cycle / 1024
        });
        urban::hangdump::setRuntime(&runtime);
        urban::calltrace::installFromEnvironment(runtime);
        urban::FunctionHooks::instance().logBindings();
        urban::callprofile::installFromEnvironment(runtime); // last: wraps whatever the slots hold now

        URBAN_INFO(Boot, "starting guest at 0x" << std::hex << runtime.cpu().pc);
        runtime.run();
        URBAN_INFO(Boot, "runtime exited");
    }
    catch (const std::exception &e)
    {
        URBAN_ERROR(Boot, "fatal: " << e.what());
        std::cout.flush();
        std::_Exit(1);
    }
    std::cout.flush();
    std::_Exit(0);
}
