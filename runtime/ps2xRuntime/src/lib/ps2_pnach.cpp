#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace ps2x::pnach
{
    namespace
    {
        constexpr uint32_t kRamSize = 32u * 1024u * 1024u;
        using List = std::vector<Patch>;

        // The patches in use: replaced as a whole by a reload (watcher thread), read at every vblank (EE thread).
        std::mutex g_mutex;
        std::shared_ptr<const List> g_list = std::make_shared<List>();
        List g_loading; // loadFile() target before the list is published
        bool g_loaded = false;
        std::atomic<bool> g_onceAfterReload{false};
        std::atomic<bool> g_reloadRequested{false};
        std::atomic<uint64_t> g_writes{0u};
        std::vector<std::function<void()>> g_frameHooks; // set before the game starts, run on the EE thread

        std::mutex g_messageMutex;
        std::string g_message;
        std::chrono::steady_clock::time_point g_messageTime;

        std::string trim(std::string text)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool parseHex(const std::string &text, uint64_t &out)
        {
            if (text.empty())
                return false;
            char *end = nullptr;
            out = std::strtoull(text.c_str(), &end, 16);
            return end && *end == '\0';
        }

        std::shared_ptr<const List> current()
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            return g_list;
        }

        // The sources of the list in use: the enabled files' patches, then the enabled groups of the mods.
        struct Group
        {
            std::string name;
            List patches;
            bool enabled = false;
        };
        std::mutex g_sourcesMutex;
        List g_files;
        std::vector<Group> g_groups;

        void rebuild() // g_sourcesMutex held
        {
            List combined = g_files;
            for (const Group &group : g_groups)
                if (group.enabled)
                    combined.insert(combined.end(), group.patches.begin(), group.patches.end());
            auto shared = std::make_shared<const List>(std::move(combined));
            std::lock_guard<std::mutex> lock(g_mutex);
            g_list = std::move(shared);
        }

        void publish(List files)
        {
            std::lock_guard<std::mutex> lock(g_sourcesMutex);
            g_files = std::move(files);
            rebuild();
        }

        // EE RAM (32 MB, little endian like the host); addresses outside it are ignored, as PCSX2 ignores writes to
        // unmapped memory.
        struct Ram
        {
            uint8_t *rdram;
            uint64_t writes = 0u;

            template <typename T>
            T read(uint32_t address) const
            {
                address &= 0x01FFFFFFu;
                T value{};
                if (address + sizeof(T) <= kRamSize)
                    std::memcpy(&value, rdram + address, sizeof(T));
                return value;
            }

            template <typename T>
            void write(uint32_t address, T value)
            {
                address &= 0x01FFFFFFu;
                if (address + sizeof(T) > kRamSize)
                    return;
                std::memcpy(rdram + address, &value, sizeof(T));
                ++writes;
            }
        };

        // PCSX2's handle_extended_t (Patch_Memory.cpp): CodeBreaker codes, one patch line at a time, with the state
        // that multi-line codes and the D/E conditionals carry over to the next lines.
        struct Extended
        {
            uint32_t skip = 0u;
            uint32_t iterations = 0u;
            uint32_t previousType = 0u;
            uint32_t previousAddress = 0u;
            uint32_t lastType = 0u;
            uint32_t iterationIncrement = 0u;
            uint32_t value = 0u;

            void pointerWrite(Ram &ram, uint32_t base)
            {
                if (((base & 0x0FFFFFFFu) & 0x3FFFFFFCu) == 0u)
                    return;
                switch (lastType)
                {
                case 0u: ram.write<uint8_t>(previousAddress, static_cast<uint8_t>(value)); break;
                case 1u: ram.write<uint16_t>(previousAddress, static_cast<uint16_t>(value)); break;
                case 2u: ram.write<uint32_t>(previousAddress, value); break;
                default: break;
                }
            }

            void run(Ram &ram, uint32_t addr, uint32_t data)
            {
                if (skip > 0u)
                {
                    --skip;
                    return;
                }
                switch (previousType)
                {
                case 0x3040u: // vvvvvvvv 00000000: increment the 32-bit value of the previous line
                    ram.write<uint32_t>(previousAddress, ram.read<uint32_t>(previousAddress) + addr);
                    previousType = 0u;
                    return;
                case 0x3050u: // decrement
                    ram.write<uint32_t>(previousAddress, ram.read<uint32_t>(previousAddress) - addr);
                    previousType = 0u;
                    return;
                case 0x4000u: // vvvvvvvv iiiiiiii: serial write
                    for (uint32_t i = 0; i < iterations; ++i)
                        ram.write<uint32_t>(previousAddress + i * iterationIncrement, addr + data * i);
                    previousType = 0u;
                    return;
                case 0x5000u: // dddddddd 00000000: copy bytes
                    for (uint32_t i = 0; i < iterations; ++i)
                        ram.write<uint8_t>((addr + i) & 0x0FFFFFFFu, ram.read<uint8_t>(previousAddress + i));
                    previousType = 0u;
                    return;
                case 0x6000u: // 000Xnnnn iiiiiiii: pointer write, first pointer
                {
                    iterations = (addr & 0xFFFFu) == 0u ? 1u : (addr & 0xFFFFu);
                    lastType = (addr & 0x000F0000u) >> 16u;
                    const uint32_t base = ram.read<uint32_t>(previousAddress);
                    previousAddress = base + data;
                    if (--iterations == 0u)
                    {
                        previousType = 0u;
                        pointerWrite(ram, base);
                    }
                    else
                        previousType = ((base & 0x0FFFFFFFu) & 0x3FFFFFFCu) == 0u ? 0u : 0x6001u;
                    return;
                }
                case 0x6001u: // iiiiiiii iiiiiiii: the next two pointer offsets
                {
                    uint32_t base = ram.read<uint32_t>(previousAddress & 0x0FFFFFFFu);
                    previousAddress = base + addr;
                    if (--iterations == 0u)
                    {
                        previousType = 0u;
                        pointerWrite(ram, base);
                        return;
                    }
                    base = ram.read<uint32_t>(previousAddress);
                    previousAddress = base + data;
                    if (--iterations == 0u)
                    {
                        previousType = 0u;
                        pointerWrite(ram, base);
                    }
                    return;
                }
                default:
                    break;
                }

                previousType = 0u;
                const uint32_t type = addr & 0xF0000000u;
                const uint32_t target = addr & 0x0FFFFFFFu;
                if (type == 0x00000000u) // 0aaaaaaa 000000vv
                    ram.write<uint8_t>(target, static_cast<uint8_t>(data));
                else if (type == 0x10000000u) // 1aaaaaaa 0000vvvv
                    ram.write<uint16_t>(target, static_cast<uint16_t>(data));
                else if (type == 0x20000000u) // 2aaaaaaa vvvvvvvv
                    ram.write<uint32_t>(target, data);
                else if (type == 0x30000000u)
                {
                    const uint32_t op = addr & 0xFFFF0000u;
                    if (op == 0x30000000u) // 300000vv 0aaaaaaa
                        ram.write<uint8_t>(data, static_cast<uint8_t>(ram.read<uint8_t>(data) + (addr & 0xFFu)));
                    else if (op == 0x30100000u) // 301000vv 0aaaaaaa
                        ram.write<uint8_t>(data, static_cast<uint8_t>(ram.read<uint8_t>(data) - (addr & 0xFFu)));
                    else if (op == 0x30200000u) // 3020vvvv 0aaaaaaa
                        ram.write<uint16_t>(data, static_cast<uint16_t>(ram.read<uint16_t>(data) + (addr & 0xFFFFu)));
                    else if (op == 0x30300000u) // 3030vvvv 0aaaaaaa
                        ram.write<uint16_t>(data, static_cast<uint16_t>(ram.read<uint16_t>(data) - (addr & 0xFFFFu)));
                    else if (op == 0x30400000u || op == 0x30500000u) // 30400000 0aaaaaaa + the value on the next line
                    {
                        previousType = op == 0x30400000u ? 0x3040u : 0x3050u;
                        previousAddress = data;
                    }
                }
                else if (type == 0x40000000u) // 4aaaaaaa nnnnssss + the value line
                {
                    iterations = data >> 16u;
                    iterationIncrement = (data & 0xFFFFu) * 4u;
                    previousAddress = target;
                    previousType = 0x4000u;
                }
                else if (type == 0x50000000u) // 5sssssss nnnnnnnn + the destination line
                {
                    previousAddress = target;
                    iterations = data;
                    previousType = 0x5000u;
                }
                else if (type == 0x60000000u) // 6aaaaaaa vvvvvvvv + the pointer line(s)
                {
                    previousAddress = target;
                    value = data;
                    iterations = 0u;
                    previousType = 0x6000u;
                }
                else if (type == 0x70000000u) // 7aaaaaaa 00Y0vvvv: OR/AND/XOR, 8 or 16 bits
                {
                    switch (data & 0x00F00000u)
                    {
                    case 0x00000000u: ram.write<uint8_t>(target, static_cast<uint8_t>(ram.read<uint8_t>(target) | (data & 0xFFu))); break;
                    case 0x00100000u: ram.write<uint16_t>(target, static_cast<uint16_t>(ram.read<uint16_t>(target) | (data & 0xFFFFu))); break;
                    case 0x00200000u: ram.write<uint8_t>(target, static_cast<uint8_t>(ram.read<uint8_t>(target) & (data & 0xFFu))); break;
                    case 0x00300000u: ram.write<uint16_t>(target, static_cast<uint16_t>(ram.read<uint16_t>(target) & (data & 0xFFFFu))); break;
                    case 0x00400000u: ram.write<uint8_t>(target, static_cast<uint8_t>(ram.read<uint8_t>(target) ^ (data & 0xFFu))); break;
                    case 0x00500000u: ram.write<uint16_t>(target, static_cast<uint16_t>(ram.read<uint16_t>(target) ^ (data & 0xFFFFu))); break;
                    default: break;
                    }
                }
                else if (type == 0xD0000000u) // Daaaaaaa 00t0vvvv: run the next line only if the 16-bit test holds
                {
                    const uint16_t mem = ram.read<uint16_t>(target);
                    const uint16_t test = static_cast<uint16_t>(data);
                    const uint32_t how = data & 0x00F00000u;
                    if ((how == 0x00000000u && mem != test) || (how == 0x00100000u && mem == test) ||
                        (how == 0x00200000u && mem >= test) || (how == 0x00300000u && mem <= test))
                        skip = 1u;
                }
                else if (type == 0xE0000000u) // Ennnvvvv taaaaaaa: run the next nnn lines only if the test holds
                {
                    const uint16_t mem = ram.read<uint16_t>(data & 0x0FFFFFFFu);
                    const uint16_t test = static_cast<uint16_t>(addr);
                    const uint32_t how = data & 0xF0000000u;
                    if ((how == 0x00000000u && mem != test) || (how == 0x10000000u && mem == test) ||
                        (how == 0x20000000u && mem >= test) || (how == 0x30000000u && mem <= test))
                        skip = (addr & 0x0FFF0000u) >> 16u;
                }
            }
        };

        // Applies the lines of one place set in file order; extended lines share PCSX2's state.
        uint64_t applyList(uint8_t *rdram, const List &list, bool once)
        {
            Ram ram{rdram};
            Extended extended;
            for (const Patch &patch : list)
            {
                const bool wanted = once ? (patch.place == 0u || patch.place == 2u) : patch.place != 0u;
                if (!wanted)
                    continue;
                switch (patch.bytes)
                {
                case 0u: extended.run(ram, patch.address, static_cast<uint32_t>(patch.value)); break;
                case 1u: ram.write<uint8_t>(patch.address, static_cast<uint8_t>(patch.value)); break;
                case 2u: ram.write<uint16_t>(patch.address, static_cast<uint16_t>(patch.value)); break;
                case 4u: ram.write<uint32_t>(patch.address, static_cast<uint32_t>(patch.value)); break;
                case 8u: ram.write<uint64_t>(patch.address, patch.value); break;
                default: break;
                }
            }
            return ram.writes;
        }

        // The patch lines of a .pnach text; returns how many were added.
        size_t parse(List &list, std::istream &file, size_t &unsupported)
        {
            size_t added = 0u;
            std::string line;
            while (std::getline(file, line))
            {
                if (const size_t comment = line.find("//"); comment != std::string::npos)
                    line.resize(comment);
                line = trim(line);
                if (line.rfind("patch", 0) != 0u)
                    continue; // gametitle=, comment=, author=, [groups], blank lines
                const size_t equals = line.find('=');
                if (equals == std::string::npos)
                    continue;
                std::vector<std::string> fields;
                std::stringstream rest(line.substr(equals + 1u));
                std::string field;
                while (std::getline(rest, field, ','))
                    fields.push_back(trim(field));
                uint64_t place = 0u, address = 0u, value = 0u;
                if (fields.size() < 5u || lower(fields[1]) != "ee" || !parseHex(fields[0], place) || place > 2u ||
                    !parseHex(fields[2], address) || !parseHex(fields[4], value))
                {
                    ++unsupported;
                    continue;
                }
                const std::string type = lower(fields[3]);
                Patch patch;
                patch.place = static_cast<uint8_t>(place);
                patch.address = static_cast<uint32_t>(address);
                patch.value = value;
                if (type == "byte")
                    patch.bytes = 1u;
                else if (type == "short")
                    patch.bytes = 2u;
                else if (type == "word")
                    patch.bytes = 4u;
                else if (type == "double")
                    patch.bytes = 8u;
                else if (type == "extended")
                    patch.bytes = 0u;
                else
                {
                    ++unsupported;
                    continue;
                }
                list.push_back(patch);
                ++added;
            }
            return added;
        }

        size_t loadInto(List &list, const std::string &path)
        {
            std::ifstream file(path);
            if (!file)
            {
                std::fprintf(stderr, "[pnach] cannot open %s\n", path.c_str());
                return 0u;
            }
            size_t unsupported = 0u;
            const size_t added = parse(list, file, unsupported);
            std::fprintf(stderr, "[pnach] %s: %zu patches loaded, %zu lines skipped\n", path.c_str(), added, unsupported);
            return added;
        }

        List loadAll(const std::vector<std::string> &files)
        {
            List list;
            for (const std::string &path : files)
                (void)loadInto(list, path);
            return list;
        }

        std::vector<std::string> environmentFiles()
        {
            std::vector<std::string> files;
            const char *value = std::getenv("PS2X_PNACH");
            if (!value || !*value || std::strcmp(value, "none") == 0 || std::strcmp(value, "0") == 0)
                return files;
            std::stringstream list(value);
            std::string path;
            while (std::getline(list, path, ';'))
            {
                path = trim(path);
                if (path.size() >= 2u && path.front() == '"' && path.back() == '"')
                    path = path.substr(1u, path.size() - 2u);
                if (!path.empty())
                    files.push_back(path);
            }
            return files;
        }

        // The file list plus each file's time stamp: a reload is due when this changes.
        std::string signature(const std::vector<std::string> &files)
        {
            std::string text;
            for (const std::string &path : files)
            {
                std::error_code error;
                const auto time = std::filesystem::last_write_time(path, error);
                text += path + '|' + (error ? std::string("-") : std::to_string(time.time_since_epoch().count())) + '\n';
            }
            return text;
        }

        void showMessage(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_messageMutex);
            g_message = std::move(text);
            g_messageTime = std::chrono::steady_clock::now();
        }
    }

    size_t loadFile(const std::string &path)
    {
        const size_t added = loadInto(g_loading, path);
        publish(g_loading);
        return added;
    }

    size_t loadFromEnvironment()
    {
        if (g_loaded)
            return current()->size();
        g_loaded = true;
        for (const std::string &path : environmentFiles())
            (void)loadInto(g_loading, path);
        publish(g_loading);
        return g_loading.size();
    }

    void clear()
    {
        g_loading.clear();
        publish({});
    }

    bool active() noexcept
    {
        return !current()->empty();
    }

    void applyOnce(uint8_t *rdram)
    {
        if (!rdram)
            return;
        const uint64_t count = applyList(rdram, *current(), true);
        g_writes.fetch_add(count, std::memory_order_relaxed);
        if (count != 0u)
            std::fprintf(stderr, "[pnach] %llu boot patches applied\n", static_cast<unsigned long long>(count));
    }

    void applyContinuous(uint8_t *rdram)
    {
        if (!rdram)
            return;
        const std::shared_ptr<const List> list = current();
        if (g_onceAfterReload.exchange(false, std::memory_order_acq_rel))
            g_writes.fetch_add(applyList(rdram, *list, true), std::memory_order_relaxed);
        const uint64_t count = list->empty() ? 0u : applyList(rdram, *list, false);
        for (const auto &hook : g_frameHooks)
            hook();
        if (list->empty())
            return;
        const uint64_t total = g_writes.fetch_add(count, std::memory_order_relaxed) + count;
        // Every ~10 s: proof that the patches keep being applied (benchmarks check this line).
        static auto lastLog = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (count != 0u && now - lastLog >= std::chrono::seconds(10))
        {
            lastLog = now;
            std::fprintf(stderr, "[pnach] %llu writes applied so far (%llu per vblank)\n", static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(count));
        }
    }

    uint64_t writesApplied() noexcept
    {
        return g_writes.load(std::memory_order_relaxed);
    }

    void watch(std::function<std::vector<std::string>()> files)
    {
        const char *reload = std::getenv("PS2X_PNACH_RELOAD");
        if ((reload && *reload == '0') || !files)
            return;
        const char *fixed = std::getenv("PS2X_PNACH");
        if (fixed && (std::strcmp(fixed, "none") == 0 || std::strcmp(fixed, "0") == 0))
            return; // benchmarks of unpatched scenes
        std::thread([files = std::move(files)]
        {
            std::string last = signature(files());
            for (;;)
            {
                for (int i = 0; i < 10 && !g_reloadRequested.load(std::memory_order_relaxed); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const bool forced = g_reloadRequested.exchange(false);
                const std::vector<std::string> list = files();
                std::string now = signature(list);
                if (!forced && now == last)
                    continue;
                last = std::move(now);
                List patches = loadAll(list);
                const size_t count = patches.size();
                publish(std::move(patches));
                g_onceAfterReload.store(true, std::memory_order_release);
                std::fprintf(stderr, "[pnach] reloaded: %zu file(s), %zu patches\n", list.size(), count);
                showMessage(list.empty() ? std::string("Patches off")
                                         : "Patches reloaded: " + std::to_string(list.size()) + " file(s), " + std::to_string(count) + " lines");
            }
        }).detach();
    }

    void addFrameHook(std::function<void()> hook)
    {
        if (hook)
            g_frameHooks.push_back(std::move(hook));
    }

    int addGroup(const std::string &name, const std::string &pnachText, bool enabled)
    {
        Group group;
        group.name = name;
        group.enabled = enabled;
        std::istringstream text(pnachText);
        size_t unsupported = 0u;
        if (parse(group.patches, text, unsupported) == 0u)
        {
            std::fprintf(stderr, "[pnach] group '%s' has no patch line\n", name.c_str());
            return -1;
        }
        std::fprintf(stderr, "[pnach] group '%s': %zu patches (%s)%s\n", name.c_str(), group.patches.size(), enabled ? "on" : "off",
                     unsupported ? ", some lines skipped" : "");
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        g_groups.push_back(std::move(group));
        rebuild();
        if (enabled)
            g_onceAfterReload.store(true, std::memory_order_release);
        return static_cast<int>(g_groups.size() - 1u);
    }

    void setGroupEnabled(int id, bool enabled)
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        if (id < 0 || id >= static_cast<int>(g_groups.size()) || g_groups[static_cast<size_t>(id)].enabled == enabled)
            return;
        g_groups[static_cast<size_t>(id)].enabled = enabled;
        rebuild();
        if (enabled)
            g_onceAfterReload.store(true, std::memory_order_release);
        std::fprintf(stderr, "[pnach] group '%s' %s\n", g_groups[static_cast<size_t>(id)].name.c_str(), enabled ? "on" : "off");
    }

    bool groupEnabled(int id)
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        return id >= 0 && id < static_cast<int>(g_groups.size()) && g_groups[static_cast<size_t>(id)].enabled;
    }

    void requestReload()
    {
        g_reloadRequested.store(true, std::memory_order_relaxed);
    }

    bool overlayMessage(std::string &text, float &alpha)
    {
        std::lock_guard<std::mutex> lock(g_messageMutex);
        if (g_message.empty())
            return false;
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_messageTime).count();
        if (seconds >= 3.0f)
        {
            g_message.clear();
            return false;
        }
        text = g_message;
        alpha = seconds < 2.5f ? 1.0f : (3.0f - seconds) / 0.5f;
        return true;
    }
}
