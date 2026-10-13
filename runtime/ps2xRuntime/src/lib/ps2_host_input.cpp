#include "ps2_host_input.h"
#include "runtime/ps2_input_options.h"
#include "runtime/ps2_save_state.h"
#include "ps2_host_backend.h"
#include "ps2_host_gamepads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2_host_input
{
    namespace
    {
        using ps2x::iop::PadInputState;
        using ps2x::iop::PadPressure;
        namespace PadButton = ps2x::iop::PadButton;

        // Controller bindings in PCSX2's syntax (controls.ini [Pad1]-[Pad8], the [PadN] sections of PCSX2.ini): each
        // PS2 button or stick direction lists "<source>/<element>" bindings, any of which presses it; "a & b" needs
        // both. urban.exe turns the file into PS2X_PAD<n>=1 plus PS2X_PAD<n>_<KEY>=<binding>;<binding>... variables.
        enum class Source
        {
            Keyboard,
            Pointer,
            PadButton,
            PadAxis
        };

        struct Element
        {
            Source source = Source::Keyboard;
            int device = 0;
            int code = 0;
            float sign = 1.0f; // PadAxis: +1 = the positive half (the whole range of a trigger), -1 = the negative half
        };

        using Chord = std::vector<Element>;    // all held ("&")
        using Bindings = std::vector<Chord>;   // any of them

        // Keyboard codes for "Shift"/"Control"/"Alt" (either side, as PCSX2 names them on Windows).
        constexpr int kAnyShift = -1;
        constexpr int kAnyControl = -2;
        constexpr int kAnyAlt = -3;

        constexpr std::array<std::pair<std::string_view, int>, 43> kKeyNames{{
            {"escape", KEY_ESCAPE}, {"tab", KEY_TAB}, {"backspace", KEY_BACKSPACE}, {"return", KEY_ENTER},
            {"enter", KEY_KP_ENTER}, {"insert", KEY_INSERT}, {"delete", KEY_DELETE}, {"pause", KEY_PAUSE},
            {"home", KEY_HOME}, {"end", KEY_END}, {"left", KEY_LEFT}, {"up", KEY_UP}, {"right", KEY_RIGHT},
            {"down", KEY_DOWN}, {"pageup", KEY_PAGE_UP}, {"pagedown", KEY_PAGE_DOWN}, {"shift", kAnyShift},
            {"control", kAnyControl}, {"alt", kAnyAlt}, {"capslock", KEY_CAPS_LOCK}, {"space", KEY_SPACE},
            {"comma", KEY_COMMA}, {"minus", KEY_MINUS}, {"period", KEY_PERIOD}, {"slash", KEY_SLASH},
            {"semicolon", KEY_SEMICOLON}, {"equal", KEY_EQUAL}, {"bracketleft", KEY_LEFT_BRACKET},
            {"backslash", KEY_BACKSLASH}, {"bracketright", KEY_RIGHT_BRACKET}, {"apostrophe", KEY_APOSTROPHE},
            {"quoteleft", KEY_GRAVE}, {"leftshift", KEY_LEFT_SHIFT}, {"rightshift", KEY_RIGHT_SHIFT},
            {"leftcontrol", KEY_LEFT_CONTROL}, {"rightcontrol", KEY_RIGHT_CONTROL}, {"leftalt", KEY_LEFT_ALT},
            {"rightalt", KEY_RIGHT_ALT}, {"numlock", KEY_NUM_LOCK}, {"scrolllock", KEY_SCROLL_LOCK},
            {"print", KEY_PRINT_SCREEN}, {"menu", KEY_KB_MENU}, {"keypad+enter", KEY_KP_ENTER},
        }};

        // SDL element names as PCSX2 writes them (SDLInputSource), in SDL's button order.
        constexpr std::array<std::string_view, ps2_host_gamepads::ButtonCount> kSdlButtonNames{
            "FaceSouth", "FaceEast", "FaceWest", "FaceNorth", "Back", "Guide", "Start", "LeftStick", "RightStick",
            "LeftShoulder", "RightShoulder", "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Misc1", "Paddle1",
            "Paddle2", "Paddle3", "Paddle4", "Touchpad"};
        constexpr std::array<std::string_view, ps2_host_gamepads::AxisCount> kSdlAxisNames{
            "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};

        std::string lowerName(std::string_view text)
        {
            std::string name(text);
            for (char &ch : name)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return name;
        }

        std::string_view trimView(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1u);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1u);
            return text;
        }

        std::optional<int> keyCode(std::string_view name)
        {
            const std::string key = lowerName(name);
            if (key.size() == 1u && key[0] >= 'a' && key[0] <= 'z')
                return KEY_A + (key[0] - 'a');
            if (key.size() == 1u && key[0] >= '0' && key[0] <= '9')
                return KEY_ZERO + (key[0] - '0');
            if (key.size() >= 2u && key[0] == 'f' && std::isdigit(static_cast<unsigned char>(key[1])))
            {
                const int number = std::atoi(key.c_str() + 1);
                if (number >= 1 && number <= 12)
                    return KEY_F1 + number - 1;
            }
            if (key.size() == 8u && key.rfind("keypad+", 0) == 0 && key[7] >= '0' && key[7] <= '9')
                return KEY_KP_0 + (key[7] - '0');
            for (const auto &[keyName, code] : kKeyNames)
                if (key == keyName)
                    return code;
            return std::nullopt;
        }

        // "Keyboard/W", "Pointer-0/LeftButton", "SDL-0/FaceSouth", "SDL-0/+LeftTrigger", "SDL-1/-LeftY".
        std::optional<Element> parseElement(std::string_view text)
        {
            text = trimView(text);
            const size_t slash = text.find('/');
            if (slash == std::string_view::npos)
                return std::nullopt;
            const std::string source = lowerName(text.substr(0, slash));
            std::string_view name = text.substr(slash + 1u);
            Element element;
            if (source == "keyboard")
            {
                const auto code = keyCode(name);
                if (!code)
                    return std::nullopt;
                element.source = Source::Keyboard;
                element.code = *code;
                return element;
            }
            if (source.rfind("pointer", 0) == 0)
            {
                const std::string button = lowerName(name);
                element.source = Source::Pointer;
                if (button == "leftbutton")
                    element.code = MOUSE_BUTTON_LEFT;
                else if (button == "rightbutton")
                    element.code = MOUSE_BUTTON_RIGHT;
                else if (button == "middlebutton")
                    element.code = MOUSE_BUTTON_MIDDLE;
                else
                    return std::nullopt;
                return element;
            }
            if (source.rfind("sdl-", 0) != 0)
                return std::nullopt;
            element.device = std::atoi(source.c_str() + 4);
            if (element.device < 0 || element.device >= ps2_host_gamepads::kMaxDevices)
                return std::nullopt;
            if (!name.empty() && (name[0] == '+' || name[0] == '-'))
            {
                element.source = Source::PadAxis;
                element.sign = name[0] == '-' ? -1.0f : 1.0f;
                name.remove_prefix(1u);
                for (size_t i = 0; i < kSdlAxisNames.size(); ++i)
                {
                    if (lowerName(name) == lowerName(kSdlAxisNames[i]))
                    {
                        element.code = static_cast<int>(i);
                        return element;
                    }
                }
                return std::nullopt;
            }
            element.source = Source::PadButton;
            for (size_t i = 0; i < kSdlButtonNames.size(); ++i)
            {
                if (lowerName(name) == lowerName(kSdlButtonNames[i]))
                {
                    element.code = static_cast<int>(i);
                    return element;
                }
            }
            return std::nullopt;
        }

        std::vector<std::string_view> splitList(std::string_view text, char separator)
        {
            std::vector<std::string_view> parts;
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = std::min(text.find(separator, begin), text.size());
                if (const std::string_view part = trimView(text.substr(begin, end - begin)); !part.empty())
                    parts.push_back(part);
                begin = end + 1u;
            }
            return parts;
        }

        // PCSX2's [PadN] keys for the 16 digital buttons and the 8 stick directions.
        constexpr std::array<std::pair<std::string_view, uint16_t>, 16> kPadButtonKeys{{
            {"Up", PadButton::Up}, {"Right", PadButton::Right}, {"Down", PadButton::Down}, {"Left", PadButton::Left},
            {"Triangle", PadButton::Triangle}, {"Circle", PadButton::Circle}, {"Cross", PadButton::Cross},
            {"Square", PadButton::Square}, {"Select", PadButton::Select}, {"Start", PadButton::Start},
            {"L1", PadButton::L1}, {"L2", PadButton::L2}, {"R1", PadButton::R1}, {"R2", PadButton::R2},
            {"L3", PadButton::L3}, {"R3", PadButton::R3},
        }};
        enum StickHalf
        {
            LUp,
            LRight,
            LDown,
            LLeft,
            RUp,
            RRight,
            RDown,
            RLeft,
            StickHalfCount
        };
        constexpr std::array<std::string_view, StickHalfCount> kStickKeys{"LUp", "LRight", "LDown", "LLeft",
                                                                          "RUp", "RRight", "RDown", "RLeft"};

        struct PadConfig
        {
            std::array<Bindings, kPadButtonKeys.size()> buttons;
            std::array<Bindings, StickHalfCount> sticks;
            std::vector<int> largeMotor; // devices that play each motor
            std::vector<int> smallMotor;
            bool enabled = true;         // Type = None: no controller on this port/slot
            float deadzone = 0.0f;
            float axisScale = 1.33f;
            float buttonDeadzone = 0.0f;
            float largeMotorScale = 1.0f;
            float smallMotorScale = 1.0f;
            uint32_t invertL = 0u; // 1 = X, 2 = Y
            uint32_t invertR = 0u;
        };

        // Fills `config` from one [PadN] key (PCSX2's name, any case) and its bindings separated by ';'.
        void applyPadSetting(PadConfig &config, uint32_t pad, std::string_view key, std::string_view value)
        {
            const std::string name = lowerName(key);
            auto number = [&](float fallback)
            {
                char *end = nullptr;
                const std::string text(trimView(value));
                const float parsed = std::strtof(text.c_str(), &end);
                return end != text.c_str() && std::isfinite(parsed) ? parsed : fallback;
            };
            auto bindings = [&]()
            {
                Bindings list;
                for (std::string_view entry : splitList(value, ';'))
                {
                    Chord chord;
                    for (std::string_view part : splitList(entry, '&'))
                    {
                        if (const auto element = parseElement(part))
                            chord.push_back(*element);
                        else
                        {
                            std::cerr << "[input] Pad" << pad + 1u << " " << key << ": unknown binding '" << part << "'" << std::endl;
                            chord.clear();
                            break;
                        }
                    }
                    if (!chord.empty())
                        list.push_back(std::move(chord));
                }
                return list;
            };
            for (size_t i = 0; i < kPadButtonKeys.size(); ++i)
                if (name == lowerName(kPadButtonKeys[i].first))
                    return void(config.buttons[i] = bindings());
            for (size_t i = 0; i < kStickKeys.size(); ++i)
                if (name == lowerName(kStickKeys[i]))
                    return void(config.sticks[i] = bindings());
            if (name == "largemotor" || name == "smallmotor")
            {
                std::vector<int> &devices = name == "largemotor" ? config.largeMotor : config.smallMotor;
                devices.clear();
                for (std::string_view entry : splitList(value, ';'))
                {
                    const std::string text = lowerName(entry);
                    if (text.rfind("sdl-", 0) == 0)
                        devices.push_back(std::atoi(text.c_str() + 4));
                }
            }
            else if (name == "type")
                config.enabled = lowerName(trimView(value)) != "none";
            else if (name == "deadzone")
                config.deadzone = std::clamp(number(0.0f), 0.0f, 0.95f);
            else if (name == "axisscale")
                config.axisScale = std::clamp(number(1.33f), 0.01f, 4.0f);
            else if (name == "buttondeadzone")
                config.buttonDeadzone = std::clamp(number(0.0f), 0.0f, 0.95f);
            else if (name == "largemotorscale")
                config.largeMotorScale = std::max(number(1.0f), 0.0f);
            else if (name == "smallmotorscale")
                config.smallMotorScale = std::max(number(1.0f), 0.0f);
            else if (name == "invertl")
                config.invertL = static_cast<uint32_t>(number(0.0f)) & 3u;
            else if (name == "invertr")
                config.invertR = static_cast<uint32_t>(number(0.0f)) & 3u;
            // Other PCSX2 keys (Analog, PressureModifier, ...) are accepted and ignored.
        }

        // [PadN] from the PS2X_PAD<n>_* variables when urban.exe found the section (PS2X_PAD<n>=1), else the defaults.
        PadConfig loadPadConfig(uint32_t pad)
        {
            PadConfig config;
            const std::string prefix = "PS2X_PAD" + std::to_string(pad + 1u);
            if (!std::getenv(prefix.c_str()))
            {
                for (const auto &[key, value] : defaultPadSettings(pad))
                {
                    // Defaults list a key once per binding; join them like the file's repeated lines.
                    std::string joined;
                    for (const auto &[otherKey, otherValue] : defaultPadSettings(pad))
                        if (otherKey == key)
                            joined += (joined.empty() ? "" : ";") + otherValue;
                    applyPadSetting(config, pad, key, joined);
                }
                return config;
            }
            constexpr std::string_view kSettingKeys[] = {"Type", "Deadzone", "AxisScale", "ButtonDeadzone",
                                                         "LargeMotorScale", "SmallMotorScale", "InvertL", "InvertR",
                                                         "LargeMotor", "SmallMotor"};
            auto read = [&](std::string_view key)
            {
                std::string variable = prefix + "_";
                for (char ch : key)
                    variable += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (const char *value = std::getenv(variable.c_str()))
                    applyPadSetting(config, pad, key, value);
            };
            for (std::string_view key : kSettingKeys)
                read(key);
            for (const auto &[key, button] : kPadButtonKeys)
                read(key);
            for (std::string_view key : kStickKeys)
                read(key);
            return config;
        }

        const std::array<PadConfig, kMaxPlayers> &padConfigs()
        {
            static const std::array<PadConfig, kMaxPlayers> configs = []
            {
                std::array<PadConfig, kMaxPlayers> list;
                for (uint32_t pad = 0; pad < kMaxPlayers; ++pad)
                    list[pad] = loadPadConfig(pad);
                return list;
            }();
            return configs;
        }

        constexpr std::array<std::pair<PadPressure, uint16_t>, static_cast<size_t>(PadPressure::Count)> kPressureButtons{{
            {PadPressure::Right, PadButton::Right},
            {PadPressure::Left, PadButton::Left},
            {PadPressure::Up, PadButton::Up},
            {PadPressure::Down, PadButton::Down},
            {PadPressure::Triangle, PadButton::Triangle},
            {PadPressure::Circle, PadButton::Circle},
            {PadPressure::Cross, PadButton::Cross},
            {PadPressure::Square, PadButton::Square},
            {PadPressure::L1, PadButton::L1},
            {PadPressure::R1, PadButton::R1},
            {PadPressure::L2, PadButton::L2},
            {PadPressure::R2, PadButton::R2},
        }};

        struct ScriptStep
        {
            double start = 0.0;
            double end = 0.0;
            bool fields = false;
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
        };

        struct AxisName
        {
            std::string_view name;
            size_t axis;
            uint8_t value;
        };

        constexpr std::array<AxisName, 8> kAxisNames{{
            {"lleft", 0u, 0x00u},
            {"lright", 0u, 0xFFu},
            {"lup", 1u, 0x00u},
            {"ldown", 1u, 0xFFu},
            {"rleft", 2u, 0x00u},
            {"rright", 2u, 0xFFu},
            {"rup", 3u, 0x00u},
            {"rdown", 3u, 0xFFu},
        }};

        constexpr std::array<std::pair<std::string_view, uint16_t>, 16> kButtonNames{{
            {"select", PadButton::Select},
            {"l3", PadButton::L3},
            {"r3", PadButton::R3},
            {"start", PadButton::Start},
            {"up", PadButton::Up},
            {"right", PadButton::Right},
            {"down", PadButton::Down},
            {"left", PadButton::Left},
            {"l2", PadButton::L2},
            {"r2", PadButton::R2},
            {"l1", PadButton::L1},
            {"r1", PadButton::R1},
            {"triangle", PadButton::Triangle},
            {"circle", PadButton::Circle},
            {"cross", PadButton::Cross},
            {"square", PadButton::Square},
        }};

        std::vector<std::string_view> split(std::string_view text, char separator)
        {
            std::vector<std::string_view> parts;
            size_t begin = 0u;
            while (begin <= text.size())
            {
                const size_t end = std::min(text.find(separator, begin), text.size());
                parts.push_back(text.substr(begin, end - begin));
                begin = end + 1u;
            }
            return parts;
        }

        std::optional<ScriptStep> parseStep(std::string_view text)
        {
            constexpr double kDefaultHoldSeconds = 1.0;
            constexpr double kDefaultHoldFields = 10.0;
            const std::vector<std::string_view> fields = split(text, ':');
            if (fields.size() < 2u || fields.size() > 3u)
                return std::nullopt;
            try
            {
                ScriptStep step;
                std::string_view start = fields[0];
                step.fields = !start.empty() && start.back() == 'v';
                if (step.fields)
                    start.remove_suffix(1u);
                step.start = std::stod(std::string(start));
                const double defaultHold = step.fields ? kDefaultHoldFields : kDefaultHoldSeconds;
                step.end = step.start + (fields.size() == 3u ? std::stod(std::string(fields[2])) : defaultHold);
                for (std::string_view name : split(fields[1], '+'))
                {
                    const auto axis = std::find_if(kAxisNames.begin(), kAxisNames.end(), [&](const AxisName &entry)
                                                   { return entry.name == name; });
                    if (axis != kAxisNames.end())
                    {
                        step.axes[axis->axis] = axis->value;
                        continue;
                    }
                    const auto found = std::find_if(kButtonNames.begin(), kButtonNames.end(), [&](const auto &entry)
                                                    { return entry.first == name; });
                    if (found == kButtonNames.end())
                        return std::nullopt;
                    step.buttons |= found->second;
                }
                return step;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::vector<ScriptStep> loadScript()
        {
            std::vector<ScriptStep> script;
            const char *value = std::getenv("PS2X_PAD_SCRIPT");
            if (!value || !*value)
                return script;
            for (std::string_view entry : split(value, ','))
            {
                if (const auto step = parseStep(entry))
                    script.push_back(*step);
                else
                    std::cerr << "[input] ignoring pad script step '" << entry << "'" << std::endl;
            }
            std::cerr << "[input] pad script: " << script.size() << " step(s) on port 0" << std::endl;
            return script;
        }

        struct ScriptedInput
        {
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
            bool active = false;
        };

        // PS2X_PAD_FIFO=<file>: a test driver appends lines while the game runs, each "buttons[:seconds]" with the
        // same names as PS2X_PAD_SCRIPT (default hold 0.3 s). Every new complete line is pressed right away, so the
        // driver can react to what is on screen instead of relying on fixed timings.
        struct FifoPress
        {
            uint16_t buttons = 0u;
            std::array<uint8_t, 4> axes{0x80u, 0x80u, 0x80u, 0x80u};
            std::chrono::steady_clock::time_point end;
        };

        void pollPadFifo(std::vector<FifoPress> &active)
        {
            static const char *path = std::getenv("PS2X_PAD_FIFO");
            if (!path || !*path)
                return;
            static std::streamoff offset = 0;
            static auto lastCheck = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCheck < std::chrono::milliseconds(30))
                return;
            lastCheck = now;
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return;
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < offset)
                offset = 0; // the driver truncated the file
            if (size <= offset)
                return;
            std::string chunk(static_cast<size_t>(size - offset), '\0');
            file.seekg(offset);
            file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const size_t lastNewline = chunk.rfind('\n');
            if (lastNewline == std::string::npos)
                return; // wait for a complete line
            offset += static_cast<std::streamoff>(lastNewline + 1u);
            for (std::string_view line : split(std::string_view(chunk).substr(0, lastNewline), '\n'))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.remove_suffix(1u);
                if (line.empty())
                    continue;
                // "!save" / "!load": quick save state slot, like F5 / F9.
                if (line == "!save" || line == "!load")
                {
                    line == "!save" ? ps2x::savestate::requestQuickSave() : ps2x::savestate::requestQuickLoad();
                    continue;
                }
                const std::string entry = "0:" + std::string(line) + (line.find(':') == std::string_view::npos ? ":0.3" : "");
                if (const auto step = parseStep(entry))
                {
                    FifoPress press;
                    press.buttons = step->buttons;
                    press.axes = step->axes;
                    press.end = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          std::chrono::duration<double>(step->end - step->start));
                    active.push_back(press);
                }
                else
                    std::cerr << "[input] ignoring pad fifo line '" << line << "'" << std::endl;
            }
        }

        ScriptedInput scriptedInput(uint64_t vsyncTick)
        {
            static const std::vector<ScriptStep> script = loadScript();
            static const auto origin = std::chrono::steady_clock::now();
            static std::vector<FifoPress> fifoPresses;
            ScriptedInput input;
            pollPadFifo(fifoPresses);
            if (!fifoPresses.empty())
            {
                const auto nowTime = std::chrono::steady_clock::now();
                std::erase_if(fifoPresses, [&](const FifoPress &press) { return press.end <= nowTime; });
                for (const FifoPress &press : fifoPresses)
                {
                    input.buttons |= press.buttons;
                    for (size_t i = 0; i < input.axes.size(); ++i)
                        if (press.axes[i] != 0x80u)
                            input.axes[i] = press.axes[i];
                }
            }
            if (script.empty())
            {
                input.active = input.buttons != 0u;
                return input;
            }
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
            for (const ScriptStep &step : script)
            {
                const double clock = step.fields ? static_cast<double>(vsyncTick) : now;
                if (clock >= step.start && clock < step.end)
                {
                    input.buttons |= step.buttons;
                    for (size_t i = 0; i < input.axes.size(); ++i)
                    {
                        if (step.axes[i] != 0x80u)
                        {
                            input.axes[i] = step.axes[i];
                            input.active = true;
                        }
                    }
                }
            }
            input.active = input.active || input.buttons != 0u;
            return input;
        }

        std::mutex g_mutex;
        // One state per player (controls.ini [Pad1]-[Pad8]); by default player 1 is the keyboard plus SDL-0, player n
        // SDL-<n-1>.
        std::array<PadInputState, kMaxPlayers> g_state = []
        {
            std::array<PadInputState, kMaxPlayers> state{};
            state[0].connected = true;
            return state;
        }();

        // Multitap ports (bit 0 = controller port 1, bit 1 = port 2), from PS2X_MULTITAP like the IOP's SIO2.
        uint32_t multitapPorts()
        {
            static const uint32_t ports = []
            {
                const char *v = std::getenv("PS2X_MULTITAP");
                const std::string_view text = v ? v : "0";
                return (text.find('1') != std::string_view::npos ? 1u : 0u) | (text.find('2') != std::string_view::npos ? 2u : 0u);
            }();
            return ports;
        }

        // The player behind (port, slot), in PCSX2's order: 1A, 2A, then 1B-1D and 2B-2D for the ports with a multitap
        // (the slots of a port without one are skipped, so the third controller is 2B when only port 2 has a multitap).
        int playerFor(uint32_t port, uint32_t slot)
        {
            if (port >= kPadPorts || slot >= 4u)
                return -1;
            if (slot == 0u)
                return static_cast<int>(port);
            const uint32_t taps = multitapPorts();
            if ((taps & (1u << port)) == 0u)
                return -1;
            int player = 2 + static_cast<int>(slot) - 1;
            if (port == 1u && (taps & 1u) != 0u)
                player += 3;
            return player;
        }

        constexpr uint8_t kAxisNeutral = 0x7Fu;

        uint8_t axisToByte(float value)
        {
            const float clamped = std::clamp(value, -1.0f, 1.0f);
            const float scaled = std::round(static_cast<float>(kAxisNeutral) + clamped * (clamped < 0.0f ? 127.0f : 128.0f));
            return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
        }

        std::atomic<const std::atomic<uint64_t> *> g_fieldCounter{nullptr};
        std::atomic<uint32_t> g_invertRightDefault{0u};
        std::atomic<bool> g_mouseCameraEnabled{false};
        std::atomic<bool> g_mouseStickEnabled{true};
        bool g_mouseLocked = false;
        int g_ignoredMouseButton = -1;
        bool g_skipMouseDelta = false;
        bool g_mouseMoved = false;
        float g_mouseRightX = 0.0f;
        float g_mouseRightY = 0.0f;
        MouseCameraInput g_mouseCameraInput;
        std::chrono::steady_clock::time_point g_mouseInputStart;

        void recordMouseCameraInput(Vector2 delta, bool active)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!active)
            {
                g_mouseCameraInput = {};
                return;
            }
            g_mouseCameraInput.active = true;
            if (delta.x == 0.0f && delta.y == 0.0f)
                return;
            const auto now = std::chrono::steady_clock::now();
            if ((g_mouseCameraInput.x == 0.0f && g_mouseCameraInput.y == 0.0f) ||
                now - g_mouseInputStart > std::chrono::milliseconds(100))
            {
                g_mouseCameraInput.x = 0.0f;
                g_mouseCameraInput.y = 0.0f;
                g_mouseInputStart = now;
            }
            g_mouseCameraInput.x += delta.x;
            g_mouseCameraInput.y += delta.y;
        }

        // 0..1: how far the element is pressed (keys and buttons are 0 or 1).
        float elementValue(const Element &element)
        {
            switch (element.source)
            {
            case Source::Keyboard:
                if (element.code == kAnyShift)
                    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 1.0f : 0.0f;
                if (element.code == kAnyControl)
                    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ? 1.0f : 0.0f;
                if (element.code == kAnyAlt)
                    return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT) ? 1.0f : 0.0f;
                return IsKeyDown(element.code) ? 1.0f : 0.0f;
            case Source::Pointer:
                // With the mouse camera on, only while the mouse is captured (the click that captures it is skipped).
                if (g_mouseCameraEnabled.load(std::memory_order_relaxed) && (!g_mouseLocked || element.code == g_ignoredMouseButton))
                    return 0.0f;
                return IsWindowFocused() && IsMouseButtonDown(element.code) ? 1.0f : 0.0f;
            case Source::PadButton:
                return ps2_host_gamepads::button(element.device, static_cast<ps2_host_gamepads::Button>(element.code)) ? 1.0f : 0.0f;
            case Source::PadAxis:
                return std::max(0.0f, element.sign * ps2_host_gamepads::axis(element.device, static_cast<ps2_host_gamepads::Axis>(element.code)));
            }
            return 0.0f;
        }

        float bindingsValue(const Bindings &bindings)
        {
            float value = 0.0f;
            for (const Chord &chord : bindings)
            {
                float held = 1.0f;
                for (const Element &element : chord)
                    held = std::min(held, elementValue(element));
                value = std::max(value, held);
            }
            return value;
        }

        // A pad counts as plugged in when one of its bindings can be pressed: the keyboard and the mouse always, a
        // controller while it is connected.
        bool padPresent(const PadConfig &config)
        {
            if (!config.enabled)
                return false;
            auto present = [](const Bindings &bindings)
            {
                for (const Chord &chord : bindings)
                    for (const Element &element : chord)
                        if (element.source == Source::Keyboard || element.source == Source::Pointer ||
                            ps2_host_gamepads::connected(element.device))
                            return true;
                return false;
            };
            for (const Bindings &bindings : config.buttons)
                if (present(bindings))
                    return true;
            for (const Bindings &bindings : config.sticks)
                if (present(bindings))
                    return true;
            return false;
        }

        float positiveSetting(const char *name, float fallback)
        {
            if (const char *value = std::getenv(name))
            {
                char *end = nullptr;
                const float parsed = std::strtof(value, &end);
                if (end != value && *end == '\0' && std::isfinite(parsed) && parsed > 0.0f)
                    return parsed;
            }
            return fallback;
        }

        float mouseSensitivity()
        {
            static const float sensitivity = positiveSetting("PS2X_MOUSE_SENSITIVITY", 1.0f);
            return sensitivity;
        }

        float mouseVerticalSensitivity()
        {
            static const float sensitivity = positiveSetting("PS2X_MOUSE_VERTICAL_SENSITIVITY", 1.0f);
            return sensitivity;
        }

        Vector2 mouseStick(Vector2 desired)
        {
            const float distance = std::hypot(desired.x, desired.y);
            if (distance <= 0.0f)
                return {};
            constexpr float deadzone = 0.4f;
            const float magnitude = deadzone + (1.0f - deadzone) * std::min(distance, 1.0f);
            const float scale = magnitude / distance;
            return {desired.x * scale, desired.y * scale};
        }

        void sampleMouse()
        {
            g_mouseMoved = false;
            if (!g_mouseCameraEnabled.load(std::memory_order_relaxed))
            {
                if (g_mouseLocked)
                    EnableCursor();
                g_mouseLocked = false;
                g_ignoredMouseButton = -1;
                recordMouseCameraInput({}, false);
                return;
            }
            if (!IsWindowFocused() || IsKeyPressed(KEY_ESCAPE))
            {
                if (g_mouseLocked)
                    EnableCursor();
                g_mouseLocked = false;
                g_ignoredMouseButton = -1;
                recordMouseCameraInput({}, false);
                return;
            }
            if (!g_mouseLocked)
            {
                int button = -1;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                    button = MOUSE_BUTTON_LEFT;
                else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
                    button = MOUSE_BUTTON_RIGHT;
                else if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE))
                    button = MOUSE_BUTTON_MIDDLE;
                if (button != -1)
                {
                    DisableCursor();
                    g_mouseLocked = true;
                    g_ignoredMouseButton = button;
                    g_skipMouseDelta = true;
                }
                recordMouseCameraInput({}, g_mouseLocked);
                return;
            }
            if (g_ignoredMouseButton != -1 && !IsMouseButtonDown(g_ignoredMouseButton))
                g_ignoredMouseButton = -1;
            if (g_skipMouseDelta)
            {
                g_skipMouseDelta = false;
                recordMouseCameraInput({}, true);
                return;
            }

            const Vector2 delta = GetMouseDelta();
            recordMouseCameraInput(delta, true);
            if (!g_mouseStickEnabled.load(std::memory_order_relaxed))
                return;
            const float distance = std::hypot(delta.x, delta.y);
            if (distance <= 0.0f)
                return;
            const float frameTime = std::max(GetFrameTime(), 1.0f / 240.0f);
            const float scale = mouseSensitivity() / (frameTime * 900.0f);
            const Vector2 stick = mouseStick({delta.x * scale, delta.y * scale * mouseVerticalSensitivity()});
            g_mouseRightX = stick.x;
            g_mouseRightY = stick.y;
            g_mouseMoved = true;
        }

        uint32_t rightStickInvert()
        {
            if (const char *value = std::getenv("PS2X_INVERT_RIGHT_STICK"))
            {
                const std::string_view text(value);
                uint32_t axes = 0u;
                if (text.find_first_of("xX") != std::string_view::npos || text == "1")
                    axes |= kInvertRightX;
                if (text.find_first_of("yY") != std::string_view::npos || text == "1")
                    axes |= kInvertRightY;
                return axes;
            }
            return g_invertRightDefault.load(std::memory_order_relaxed);
        }

        // Full pressure for the pressed buttons (the scripted input has no analog values).
        void applyPressure(PadInputState &state)
        {
            for (const auto &[pressure, button] : kPressureButtons)
                state.pressure[static_cast<size_t>(pressure)] = (state.buttons & button) != 0u ? 0xFFu : 0x00u;
        }

        // A stick from its four directions: radial deadzone, then PCSX2's axis scale (1.33 reaches the corners).
        std::pair<float, float> stick(const PadConfig &config, const std::array<float, StickHalfCount> &halves, size_t up, uint32_t invert)
        {
            float x = halves[up + 1u] - halves[up + 3u]; // right - left
            float y = halves[up + 2u] - halves[up];      // down - up
            const float magnitude = std::hypot(x, y);
            if (magnitude <= config.deadzone || magnitude <= 0.0f)
                return {0.0f, 0.0f};
            if (config.deadzone > 0.0f)
            {
                const float scale = (magnitude - config.deadzone) / (1.0f - config.deadzone) / magnitude;
                x *= scale;
                y *= scale;
            }
            x = std::clamp(x * config.axisScale, -1.0f, 1.0f);
            y = std::clamp(y * config.axisScale, -1.0f, 1.0f);
            return {(invert & 1u) != 0u ? -x : x, (invert & 2u) != 0u ? -y : y};
        }

        // Player n (0-based): controls.ini [Pad<n+1>], plus the mouse as the right stick for player 1 (mouse camera).
        PadInputState readPort(uint32_t player)
        {
            PadInputState state;
            const PadConfig &config = padConfigs()[player];
            // PS2X_FAKE_PADS=<n> (tests): players 1..n count as connected, idle, without a controller behind them.
            static const uint32_t fakePads = []
            { const char *v = std::getenv("PS2X_FAKE_PADS"); return v ? static_cast<uint32_t>(std::max(std::atoi(v), 0)) : 0u; }();
            state.connected = padPresent(config) || player < fakePads;
            if (!state.connected)
                return state;

            std::array<float, kPadButtonKeys.size()> values{};
            for (size_t i = 0; i < kPadButtonKeys.size(); ++i)
            {
                values[i] = bindingsValue(config.buttons[i]);
                if (values[i] > std::max(config.buttonDeadzone, 0.2f))
                    state.buttons |= kPadButtonKeys[i].second;
            }
            for (const auto &[pressure, button] : kPressureButtons)
            {
                for (size_t i = 0; i < kPadButtonKeys.size(); ++i)
                {
                    if (kPadButtonKeys[i].second == button && (state.buttons & button) != 0u)
                        state.pressure[static_cast<size_t>(pressure)] = static_cast<uint8_t>(std::clamp(values[i], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }

            std::array<float, StickHalfCount> halves{};
            for (size_t i = 0; i < halves.size(); ++i)
                halves[i] = bindingsValue(config.sticks[i]);
            const auto [leftX, leftY] = stick(config, halves, LUp, config.invertL);
            const uint32_t invertRight = config.invertR | rightStickInvert();
            auto [rightX, rightY] = stick(config, halves, RUp, invertRight);
            if (player == 0u && g_mouseMoved && g_mouseStickEnabled.load(std::memory_order_relaxed))
            {
                rightX = (invertRight & kInvertRightX) != 0u ? -g_mouseRightX : g_mouseRightX;
                rightY = (invertRight & kInvertRightY) != 0u ? -g_mouseRightY : g_mouseRightY;
            }
            state.leftX = axisToByte(leftX);
            state.leftY = axisToByte(leftY);
            state.rightX = axisToByte(rightX);
            state.rightY = axisToByte(rightY);
            return state;
        }

        // The motors each player's DualShock 2 was told to run (IOP thread), played on the controllers its
        // LargeMotor/SmallMotor bindings name (window thread). small: on/off, as on the pad; large: 0-255.
        std::array<std::atomic<uint16_t>, kMaxPlayers> g_motors{};
        std::atomic<bool> g_blocked{false}; // the in-game menu has the input

        void updateVibration()
        {
            std::array<float, ps2_host_gamepads::kMaxDevices> large{};
            std::array<float, ps2_host_gamepads::kMaxDevices> small{};
            for (uint32_t player = 0; player < kMaxPlayers; ++player)
            {
                const uint16_t motors = g_motors[player].load(std::memory_order_relaxed);
                const PadConfig &config = padConfigs()[player];
                const float largeValue = static_cast<float>(motors >> 8u) / 255.0f * config.largeMotorScale;
                const float smallValue = (motors & 0xFFu) != 0u ? config.smallMotorScale : 0.0f;
                for (int device : config.largeMotor)
                    if (device >= 0 && device < ps2_host_gamepads::kMaxDevices)
                        large[static_cast<size_t>(device)] = std::max(large[static_cast<size_t>(device)], largeValue);
                for (int device : config.smallMotor)
                    if (device >= 0 && device < ps2_host_gamepads::kMaxDevices)
                        small[static_cast<size_t>(device)] = std::max(small[static_cast<size_t>(device)], smallValue);
            }
            for (int device = 0; device < ps2_host_gamepads::kMaxDevices; ++device)
                ps2_host_gamepads::setRumble(device, large[static_cast<size_t>(device)], small[static_cast<size_t>(device)]);
        }
    }

    void setRightStickInvertDefault(uint32_t axes)
    {
        g_invertRightDefault.store(axes, std::memory_order_relaxed);
    }

    void setMouseCameraEnabled(bool enabled)
    {
        g_mouseCameraEnabled.store(enabled, std::memory_order_relaxed);
    }

    void setFieldCounter(const std::atomic<uint64_t> *counter)
    {
        g_fieldCounter.store(counter);
    }

    void sample()
    {
        ps2_host_gamepads::update();
        updateVibration();
        sampleMouse();
        // Only the players that have a (port, slot): 2 without a multitap, 5 with one, 8 with two.
        const uint32_t taps = multitapPorts();
        const uint32_t players = 2u + ((taps & 1u) ? 3u : 0u) + ((taps & 2u) ? 3u : 0u);
        std::array<PadInputState, kMaxPlayers> state{};
        const bool blocked = g_blocked.load(std::memory_order_relaxed);
        for (uint32_t player = 0; player < players; ++player)
        {
            state[player] = readPort(player);
            if (blocked)
            {
                PadInputState idle;
                idle.connected = state[player].connected;
                state[player] = idle;
            }
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = state;
    }

    void setMouseStickEnabled(bool enabled)
    {
        g_mouseStickEnabled.store(enabled, std::memory_order_relaxed);
    }

    MouseCameraInput consumeMouseCameraInput()
    {
        MouseCameraInput input;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            input = g_mouseCameraInput;
            g_mouseCameraInput.x = 0.0f;
            g_mouseCameraInput.y = 0.0f;
            if (std::chrono::steady_clock::now() - g_mouseInputStart > std::chrono::milliseconds(100))
            {
                input.x = 0.0f;
                input.y = 0.0f;
            }
        }
        const uint32_t invert = rightStickInvert();
        input.x *= mouseSensitivity() * ((invert & kInvertRightX) != 0u ? -1.0f : 1.0f);
        input.y *= mouseSensitivity() * mouseVerticalSensitivity() * ((invert & kInvertRightY) != 0u ? -1.0f : 1.0f);
        return input;
    }

    void releaseMouse()
    {
        if (g_mouseLocked)
            EnableCursor();
        g_mouseLocked = false;
        g_ignoredMouseButton = -1;
        g_mouseMoved = false;
        recordMouseCameraInput({}, false);
    }

    void setBlocked(bool blocked)
    {
        g_blocked.store(blocked, std::memory_order_relaxed);
    }

    void setVibration(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor)
    {
        const int player = playerFor(port, slot);
        if (player < 0)
            return;
        const uint16_t motors = static_cast<uint16_t>((largeMotor << 8u) | (smallMotor != 0u ? 1u : 0u));
        const uint16_t previous = g_motors[static_cast<size_t>(player)].exchange(motors, std::memory_order_relaxed);
        static const bool log = [] { const char *v = std::getenv("PS2X_LOG_VIBRATION"); return v && *v && *v != '0'; }();
        if (log && previous != motors)
            std::cerr << "[input] player " << player + 1 << " vibration: small " << unsigned(smallMotor) << " large "
                      << unsigned(largeMotor) << std::endl;
    }

    std::vector<std::pair<std::string, std::string>> defaultPadSettings(uint32_t pad)
    {
        std::vector<std::pair<std::string, std::string>> settings{
            {"Type", "DualShock2"},
            {"InvertL", "0"},
            {"InvertR", "0"},
            {"Deadzone", "0"},
            {"AxisScale", "1.33"},
            {"ButtonDeadzone", "0"},
            {"LargeMotorScale", "1"},
            {"SmallMotorScale", "1"},
        };
        const std::string sdl = "SDL-" + std::to_string(pad) + "/";
        // Player 1 also has the keyboard: face buttons on the arrow keys, left stick on WASD (2026-10-12 defaults).
        const bool keyboard = pad == 0u;
        auto add = [&](const char *key, const char *keyboardBinding, const char *padElement)
        {
            if (keyboard && keyboardBinding)
                settings.emplace_back(key, keyboardBinding);
            settings.emplace_back(key, sdl + padElement);
        };
        add("Up", nullptr, "DPadUp");
        add("Right", nullptr, "DPadRight");
        add("Down", nullptr, "DPadDown");
        add("Left", nullptr, "DPadLeft");
        add("Triangle", "Keyboard/Up", "FaceNorth");
        add("Circle", "Keyboard/Right", "FaceEast");
        add("Cross", "Keyboard/Down", "FaceSouth");
        add("Square", "Keyboard/Left", "FaceWest");
        add("Select", "Keyboard/Tab", "Back");
        add("Start", "Keyboard/Return", "Start");
        add("L1", "Keyboard/Q", "LeftShoulder");
        add("L2", "Keyboard/1", "+LeftTrigger");
        add("R1", "Keyboard/E", "RightShoulder");
        add("R2", "Keyboard/3", "+RightTrigger");
        add("L3", "Keyboard/F", "LeftStick");
        add("R3", "Keyboard/G", "RightStick");
        add("LUp", "Keyboard/W", "-LeftY");
        add("LRight", "Keyboard/D", "+LeftX");
        add("LDown", "Keyboard/S", "+LeftY");
        add("LLeft", "Keyboard/A", "-LeftX");
        add("RUp", nullptr, "-RightY");
        add("RRight", nullptr, "+RightX");
        add("RDown", nullptr, "+RightY");
        add("RLeft", nullptr, "-RightX");
        settings.emplace_back("LargeMotor", sdl + "LargeMotor");
        settings.emplace_back("SmallMotor", sdl + "SmallMotor");
        return settings;
    }

    ps2x::iop::PadInputState padState(uint32_t port, uint32_t slot)
    {
        const int player = playerFor(port, slot);
        if (player < 0)
            return {};
        PadInputState state;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            state = g_state[static_cast<size_t>(player)];
        }
        if (player == 0)
        {
            const std::atomic<uint64_t> *counter = g_fieldCounter.load();
            const ScriptedInput scripted = scriptedInput(counter ? counter->load() : 0u);
            if (scripted.active)
            {
                state.connected = true;
                state.buttons |= scripted.buttons;
                if (scripted.axes[0] != 0x80u)
                    state.leftX = scripted.axes[0];
                if (scripted.axes[1] != 0x80u)
                    state.leftY = scripted.axes[1];
                if (scripted.axes[2] != 0x80u)
                    state.rightX = scripted.axes[2];
                if (scripted.axes[3] != 0x80u)
                    state.rightY = scripted.axes[3];
                applyPressure(state);
            }
        }
        return state;
    }
}
