// CoopTool: multiplayer with up to 8 fighters (up to 4 humans + bots), every one for himself or custom teams.
// The memory map comes from thiagordsfx's Urban Reign Coop Tool (pnach generator): every fighter has a 0x9750-byte
// block; the fields below are offsets from its current health (P1 at 0x5A5FC8). The settings are written at every
// frame while the mod is switched on in its tab.
#include "urban_mod_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kP1 = 0x005A5FC8u;
    constexpr uint32_t kStride = 0x9750u;
    constexpr int kTeam = -0xB2;        // 16 bits: 0..3 = teams A..D, 4 = no team
    constexpr int kAi1 = -0xB0;         // 16 bits: 1 = AI, 0 = controller
    constexpr int kAi2 = -0x44;
    constexpr int kAiProfile = -0x34;   // 32 bits
    constexpr int kOn = -0x544;         // 16 bits, P5..P8: 1 = in the fight
    constexpr int kSkin = -0x8E24;      // 32 bits: character (looks; moves for P1..P4)
    constexpr int kHp = 0x00, kHpMax = 0x02, kSpa = 0x2A;
    constexpr int kLimbs[] = {0x04, 0x06, 0x08};                    // head, torso, legs (health of each)
    constexpr int kStats[] = {0x50, 0x52, 0x54, 0x56, 0x58, 0x5A}; // STK GRP RGA SPA WPA TGH
    constexpr int kEndurance[] = {0x0A, 0x0C, 0x0E};                // HDE UBE LBE (balance at +6)
    constexpr uint32_t kMovesTable = 0x0094A634u;                   // P1..P4 pick on the select screen (32 bits each)
    constexpr uint32_t kSameFighter = 0x00ABFA7Cu;                  // 8 bits: 1 = a fighter can be picked twice
    constexpr uint32_t kRegionalKo = 0x00ABF670u;                   // 16 bits: 0 = off (avoids bugs with 5+)
    constexpr uint32_t kTeamAttackKo = 0x00ABF674u;                 // 16 bits: 0 = off
    constexpr uint32_t kMission = 0x0094D16Cu;                      // 16 bits (trace only)
    constexpr uint32_t kScene = 0x009449F8u;                        // 32 bits: 4 = select/setup menus, 5 = fight
    constexpr uint32_t kSceneFight = 5u;
    constexpr uint32_t kBotAi = 0x002F002Fu;                        // the AI the extra bots use by default
    constexpr uint16_t kStartHealth = 1500;                         // health the game gives everyone at the start
    constexpr uint16_t kFullSpa = 1000;
    constexpr int kBrad = 0x00;

    struct Character
    {
        int id;
        const char *name;
    };
    constexpr Character kCharacters[] = {
        {0x00, "Brad Hawk"}, {0x02, "Brad Hawk (glasses)"}, {0x03, "Glen"}, {0x04, "Torque"}, {0x05, "Rod"}, {0x06, "Seth"},
        {0x07, "Nas-Tiii"}, {0x08, "Em Cee"}, {0x09, "Real Deal"}, {0x0A, "Ty"}, {0x0B, "Miguel"}, {0x0C, "Ramon"},
        {0x0D, "Jose"}, {0x0E, "Emilio"}, {0x0F, "Kadonashi"}, {0x10, "Reggie"}, {0x11, "Zack"}, {0x12, "Colin"},
        {0x13, "Jake"}, {0x14, "Tong Yoon"}, {0x15, "Grimm"}, {0x16, "BK"}, {0x17, "Grave Digga'"}, {0x18, "Bones"},
        {0x19, "Booma"}, {0x1A, "Busta"}, {0x1B, "Spider"}, {0x1C, "Pain Killah"}, {0x1D, "Dwayne"}, {0x1E, "Dwayne (Mask)"},
        {0x1F, "Shun Ying Lee"}, {0x20, "GD 05"}, {0x21, "DR 88"}, {0x22, "FK 71"}, {0x23, "PT 22"}, {0x24, "Bain"},
        {0x25, "Cooper"}, {0x26, "Anderson"}, {0x27, "Taylor"}, {0x28, "Chris"}, {0x29, "Park"}, {0x2A, "Alex"},
        {0x2B, "McKinzie"}, {0x2C, "Napalm 99"}, {0x2D, "Golem"}, {0x2E, "Riki"}, {0x2F, "Masa"}, {0x30, "Hiro"},
        {0x31, "Ryuji"}, {0x32, "Ye Wei"}, {0x33, "Sha Ying"}, {0x34, "Yan Jun"}, {0x35, "Shinkai"}, {0x36, "Lin Fong Lee"},
        {0x37, "Bordin"}, {0x38, "Lilian"}, {0x39, "Kelly"}, {0x3A, "Vera"}, {0x3B, "Paul"}, {0x3C, "Law"},
        {0x44, "GD 04"}, {0x45, "KG"}, {0x46, "Bain (Mask)"}, {0x47, "Cooper (Mask)"}};
    constexpr int kCharacterCount = static_cast<int>(sizeof(kCharacters) / sizeof(kCharacters[0]));

    // Strength presets of the Coop Tool: base value of the 6 stats and the 3 endurances.
    struct Strength
    {
        int stat;
        int endurance;
    };
    constexpr Strength kStrengths[] = {{0, 0}, {250, 150}, {500, 250}, {1000, 500}, {2000, 1000}}; // 0 = original
    constexpr float kHealth[] = {1.0f, 1.5f, 2.0f, 3.0f};

    // AI of a bot (-0x34, 32 bits). The menu levels are the raw values; the story ones are a profile id in both halves.
    struct Ai
    {
        uint32_t value; // 0 = default (the select screen's level for P2..P4, kBotAi for the extra bots)
        const char *pt;
        const char *en;
    };
    constexpr Ai kAis[] = {
        {0, "Padrão", "Default"},
        {0x00000014u, "Nível 1", "Level 1"},
        {0x00010016u, "Nível 2", "Level 2"},
        {0x00020019u, "Nível 3", "Level 3"},
        {0x0003001Au, "Nível 4", "Level 4"},
        {0x0004001Bu, "Nível 5 (máximo do menu)", "Level 5 (menu max)"},
        {0x003A003Au, "Nível 6 (chefe)", "Level 6 (boss)"},
        {0x00040004u, "História: atirador (usa arma)", "Story: shooter (uses a gun)"},
        {0x000C000Cu, "História: Golem", "Story: Golem"},
        {0x00370037u, "História: Golem (2)", "Story: Golem (2)"},
        {0x00300030u, "História: McKinzie", "Story: McKinzie"},
        {0x002F002Fu, "História: McKinzie 2", "Story: McKinzie 2"},
        {0x00350035u, "História: Napalm 99", "Story: Napalm 99"},
        {0x00360036u, "História: Napalm 99 (2)", "Story: Napalm 99 (2)"},
        {0x00340034u, "História: Shinkai", "Story: Shinkai"},
        {0x003D003Du, "História: Kadonashi", "Story: Kadonashi"},
        {0x004D004Du, "História: Ryuji", "Story: Ryuji"},
        {0x00380038u, "História: Jake", "Story: Jake"},
        {0x00330033u, "História: Jake / Grimm / Tong Yoon / Zack", "Story: Jake / Grimm / Tong Yoon / Zack"},
        {0x00320032u, "História: Glen / Park / Reggie", "Story: Glen / Park / Reggie"},
        {0x00310031u, "História: Park", "Story: Park"},
        {0x00390039u, "História: Alex / Lin Fong Lee (parceiro)", "Story: Alex / Lin Fong Lee (partner)"},
        {0x002A002Au, "História: Yakuza (Hiro / Ryuji / Riki)", "Story: Yakuza (Hiro / Ryuji / Riki)"},
        {0x002B002Bu, "História: Masa / Sha Ying", "Story: Masa / Sha Ying"},
        {0x00280028u, "História: capanga (GD 05 / Bain / Taylor)", "Story: thug (GD 05 / Bain / Taylor)"},
        {0x00290029u, "História: capanga (Cooper / FK 71 / Anderson)", "Story: thug (Cooper / FK 71 / Anderson)"},
        {0x00440044u, "História: BK / Grave Digga'", "Story: BK / Grave Digga'"},
        {0x00080008u, "História: rua (Busta / Em Cee / Torque)", "Story: street (Busta / Em Cee / Torque)"},
        {0x00090009u, "História: rua (Spider / Real Deal / DR 88)", "Story: street (Spider / Real Deal / DR 88)"}};
    constexpr int kAiCount = static_cast<int>(sizeof(kAis) / sizeof(kAis[0]));

    // Free Mode stages in multiplayer. The multiplayer loads a stage's package as afs/safe/ppk/stage%03d.ppk (id + 1),
    // which only exists for its own stages; the Free Mode ones only come in mission packages (mission%03d.ppk). So the
    // stage id is set in the setup (0x94A671, read when the fight loads) and the package name is replaced by the one of
    // a mission on that stage. Those packages have no multiplayer spawn points (everyone starts at the same place), so
    // the fighters are spread over the mission's spawn points (partner + enemies, mm), from the Coop Tool's mission data.
    struct FreeStage
    {
        int id;
        const char *name;
        int mission; // its package: mission%03d.ppk
        int count;
        int points[6][2];
    };
    constexpr FreeStage kFreeStages[] = {
        {0, "Chinatown", 2, 3, {{15586, -4150}, {16950, -2180}, {14825, -1637}}}, // 25 hangs at loading
        {1, "Restaurant", 71, 4, {{-7780, 8120}, {1200, 6200}, {-260, 7720}, {1800, 8800}}},
        {2, "Scrapyard", 35, 4, {{2236, -880}, {4280, 2160}, {2020, 4900}, {2950, 4400}}},
        {3, "Warehouse", 9, 4, {{6804, -200}, {3795, -4840}, {-800, -6450}, {-2550, -5130}}},
        {4, "Hideout", 13, 5, {{2320, -4500}, {-820, 2470}, {-2550, 3300}, {900, 4800}, {2580, 3250}}},
        {5, "Bar", 14, 3, {{-2414, 3280}, {4500, 800}, {1360, 2400}}},
        {6, "Southend", 17, 6, {{10350, -4080}, {6900, -2500}, {6500, 0}, {7000, 1000}, {5200, -3100}, {4000, 0}}},
        {7, "Subway Station", 41, 5, {{1980, -15720}, {-1680, -1870}, {-2610, -8260}, {1340, -4850}, {-50, -1450}}},
        {8, "Motel", 29, 4, {{-16286, -6030}, {-11500, -1680}, {-7500, 800}, {-7700, -2550}}},
        {9, "Motel (2)", 10, 5, {{-1264, -1750}, {-11400, -850}, {-9900, 1850}, {-8980, -2650}, {-13610, 1500}}},
        {10, "Back Alley", 12, 5, {{-3736, -6750}, {-3130, -13850}, {-2800, 4500}, {-1370, 3670}, {-4150, 2975}}},
        {11, "Slum", 37, 5, {{-2830, 4460}, {500, -4000}, {-1000, -5800}, {-2600, -1650}, {1700, 600}}},
        {12, "Abandoned Basement", 65, 5, {{-2350, 9780}, {-230, 4510}, {1650, 3810}, {2000, 6530}, {-816, 3693}}},
        {13, "Central Square", 33, 5, {{-6531, -23292}, {970, -12160}, {-2900, -11500}, {-6120, -14020}, {-700, -8600}}},
        {14, "Southend (2)", 56, 4, {{27280, 3830}, {25120, -9710}, {26580, -11090}, {28180, -9545}}},
        {15, "Motel (3)", 74, 5, {{-5620, 3670}, {-8000, -7200}, {-8250, -2630}, {-5210, -4770}, {-10150, -3900}}},
        {16, "Apartment Rooftop", 92, 5, {{1544, 4360}, {7820, -2050}, {5320, -280}, {6160, -8800}, {3560, -9230}}},
        {17, "Downtown", 40, 5, {{20952, -2720}, {-24000, -9650}, {-20720, -7880}, {-20400, -10180}, {-22150, -11880}}},
        {18, "Green Hill", 72, 4, {{8720, 9550}, {1520, 10000}, {-950, 8415}, {-1600, 12000}}},
        {19, "Bordin Bldg. Rooftop", 99, 2, {{-1608, 3650}, {1850, -2280}}},
        {20, "Underground Parking", 80, 4, {{-5550, -6620}, {-2100, -10400}, {18000, -4500}, {-1750, -13500}}},
        {21, "Bordin Bldg. Top Floor", 100, 2, {{2354, -700}, {-4930, -2670}}},
        {22, "Bordin Bldg. Entrance", 97, 5, {{-5595, -2250}, {-300, -1500}, {-130, -55}, {-850, 2550}, {-1610, 350}}},
        {23, "Bordin Bldg. Main Gate", 76, 4, {{49684, 19840}, {26100, 21750}, {25250, 8100}, {28750, 29400}}},
        {24, "Abandoned Theater", 87, 2, {{1635, 1175}, {-3490, 5210}}}, // 75 crashes a few seconds in
        {25, "Restaurant (2)", 93, 3, {{2080, 610}, {-1300, -2450}, {-200, -3400}}}};
    constexpr int kFreeStageCount = static_cast<int>(sizeof(kFreeStages) / sizeof(kFreeStages[0]));
    constexpr uint32_t kSetupStage = 0x0094A671u;  // 8 bits: the stage the next fight loads (0xFF = random)
    constexpr uint32_t kPackageName = 0x006B9D78u; // the multiplayer's "stage%03d.ppk"
    constexpr char kStagePackage[] = "stage%03d.ppk";
    constexpr int kPosition = -0x8E18; // fighter's position (x, y, z floats, metres)
    constexpr int kFacing = -0x8E08;   // float, turns (0.25 = facing +x)

    // One fighter's options. Humans use team, strength, health and the two switches; bots also the rest.
    struct Fighter
    {
        int character = 0x26; // P5/P6 only
        int team = 1;          // A..D
        int strength = 0;      // kStrengths
        int health = 0;        // kHealth
        int ai = 0;            // kAis
        bool infiniteSpa = false;
        bool unbreakable = false;
    };

    struct Config
    {
        bool on = false;
        int humans = 2;          // 1..4
        int mode = 0;            // 0 = every one for himself, 1 = custom teams
        std::array<Fighter, 4> human{};
        std::vector<Fighter> bots{Fighter{}, Fighter{}};
        bool forceP4Brad = false;
        bool sameFighter = false;
        int stage = -1; // kFreeStages index, -1 = the one of the game's menu
    };

    const UrbanModHost *g_host = nullptr;
    std::mutex g_mutex; // the config: edited on the GS thread (menu), read on the game thread (every frame)
    Config g_config;

    bool pt() { return g_host->language[0] == 'p'; }
    const char *tr(const char *ptText, const char *enText) { return pt() ? ptText : enText; }

    uint32_t fighter(int player) { return kP1 + static_cast<uint32_t>(player - 1) * kStride; }
    uint32_t field(int player, int offset) { return static_cast<uint32_t>(static_cast<int64_t>(fighter(player)) + offset); }

    uint16_t read16(uint32_t address)
    {
        uint16_t value = 0;
        g_host->read(address, &value, sizeof(value));
        return value;
    }
    uint32_t read32(uint32_t address)
    {
        uint32_t value = 0;
        g_host->read(address, &value, sizeof(value));
        return value;
    }
    void write8(uint32_t address, uint8_t value) { g_host->write(address, &value, sizeof(value)); }
    void write16(uint32_t address, uint16_t value) { g_host->write(address, &value, sizeof(value)); }
    void write32(uint32_t address, uint32_t value) { g_host->write(address, &value, sizeof(value)); }

    // --- settings ---
    int settingInt(const char *key, int fallback)
    {
        const char *value = g_host->get_setting(key, nullptr);
        return value && *value ? std::atoi(value) : fallback;
    }
    // "character,team,strength,health,ai,infiniteSpa,unbreakable"
    std::string fighterText(const Fighter &f)
    {
        char value[96];
        std::snprintf(value, sizeof(value), "%d,%d,%d,%d,%d,%d,%d", f.character, f.team, f.strength, f.health, f.ai,
                      f.infiniteSpa ? 1 : 0, f.unbreakable ? 1 : 0);
        return value;
    }
    void readFighter(const char *key, Fighter &f)
    {
        const char *value = g_host->get_setting(key, nullptr);
        if (!value)
            return;
        int spa = f.infiniteSpa, limbs = f.unbreakable;
        std::sscanf(value, "%d,%d,%d,%d,%d,%d,%d", &f.character, &f.team, &f.strength, &f.health, &f.ai, &spa, &limbs);
        f.team = std::clamp(f.team, 0, 3);
        f.strength = std::clamp(f.strength, 0, 4);
        f.health = std::clamp(f.health, 0, 3);
        f.ai = std::clamp(f.ai, 0, kAiCount - 1);
        f.infiniteSpa = spa != 0;
        f.unbreakable = limbs != 0;
    }
    void saveConfig(const Config &config)
    {
        char key[32];
        g_host->set_setting("on", config.on ? "1" : "0");
        g_host->set_setting("humans", std::to_string(config.humans).c_str());
        g_host->set_setting("mode", std::to_string(config.mode).c_str());
        for (int i = 0; i < 4; ++i)
        {
            std::snprintf(key, sizeof(key), "human%d", i + 1);
            g_host->set_setting(key, fighterText(config.human[static_cast<size_t>(i)]).c_str());
        }
        g_host->set_setting("bots", std::to_string(config.bots.size()).c_str());
        for (size_t i = 0; i < config.bots.size(); ++i)
        {
            std::snprintf(key, sizeof(key), "bot%zu", i + 1);
            g_host->set_setting(key, fighterText(config.bots[i]).c_str());
        }
        g_host->set_setting("forceP4Brad", config.forceP4Brad ? "1" : "0");
        g_host->set_setting("sameFighter", config.sameFighter ? "1" : "0");
        g_host->set_setting("stage", std::to_string(config.stage).c_str());
    }
    Config loadConfig()
    {
        Config config;
        config.on = settingInt("on", 0) != 0;
        config.humans = std::clamp(settingInt("humans", config.humans), 1, 4);
        config.mode = std::clamp(settingInt("mode", config.mode), 0, 1);
        char key[32];
        for (int i = 0; i < 4; ++i)
        {
            Fighter &f = config.human[static_cast<size_t>(i)];
            f.team = i % 2;
            std::snprintf(key, sizeof(key), "human%d", i + 1);
            readFighter(key, f);
        }
        const int bots = std::clamp(settingInt("bots", static_cast<int>(config.bots.size())), 0, 8 - config.humans);
        config.bots.assign(static_cast<size_t>(bots), Fighter{});
        for (int i = 0; i < bots; ++i)
        {
            Fighter &bot = config.bots[static_cast<size_t>(i)];
            bot.strength = 2; // Normal
            std::snprintf(key, sizeof(key), "bot%d", i + 1);
            readFighter(key, bot);
        }
        config.forceP4Brad = settingInt("forceP4Brad", 0) != 0;
        config.sameFighter = settingInt("sameFighter", 0) != 0;
        config.stage = std::clamp(settingInt("stage", -1), -1, kFreeStageCount - 1);
        return config;
    }

    // --- every frame (game thread) ---
    bool inFight() { return read32(kScene) == kSceneFight; }

    // 0x59D0C0 (32 bits): 1 = story / Free Mode, 2..4 = Multiplayer (the modes that load stage%03d.ppk).
    constexpr uint32_t kGameMode = 0x0059D0C0u;
    bool multiplayer()
    {
        const uint32_t mode = read32(kGameMode);
        return mode >= 2u && mode <= 4u;
    }

    void trace()
    {
        static const bool on = std::getenv("COOPTOOL_TRACE") != nullptr;
        if (!on)
            return;
        static std::string last;
        static unsigned frame = 0;
        ++frame;
        char line[200];
        std::snprintf(line, sizeof(line), "scene=%u mission=%04x | P1 on=%u | P5 on=%u ai1=%u | stage=%u menu=%u 4453E4=%u 652974=%u",
                      read32(kScene), read16(kMission), read16(field(1, kOn)), read16(field(5, kOn)),
                      read16(field(5, kAi1)), read32(0x005E8C88u), read32(0x00943D98u), read32(0x004453E4u), read32(0x00652974u));
        if (last != line)
        {
            last = line;
            char stamped[220];
            std::snprintf(stamped, sizeof(stamped), "f%u %s", frame, line);
            g_host->log(stamped);
        }
    }

    void applyStrength(int player, int strength)
    {
        if (strength <= 0)
            return;
        const Strength &s = kStrengths[strength];
        for (int offset : kStats)
            write16(field(player, offset), static_cast<uint16_t>(s.stat));
        for (int offset : kEndurance)
        {
            write16(field(player, offset), static_cast<uint16_t>(s.endurance));
            write16(field(player, offset + 6), static_cast<uint16_t>(s.endurance / 2));
        }
    }

    void applyFighter(int player, const Fighter &f, bool human)
    {
        applyStrength(player, f.strength);
        // At the start of a fight everyone has 1500 / 1500: raise it once.
        const float scale = kHealth[f.health];
        if (scale != 1.0f && read16(field(player, kHpMax)) == kStartHealth)
        {
            const uint16_t value = static_cast<uint16_t>(std::min(65535.0f, kStartHealth * scale));
            write16(field(player, kHp), value);
            write16(field(player, kHpMax), value);
            write16(field(player, kSpa), kFullSpa);
        }
        if (human && f.infiniteSpa)
            write16(field(player, kSpa), kFullSpa);
        // A limb's health is full at its endurance (head HDE, torso UBE, legs LBE), not at a fixed 0xFF.
        if (f.unbreakable)
            for (size_t i = 0; i < 3; ++i)
                write16(field(player, kLimbs[i]), read16(field(player, kEndurance[i])));
    }

    // P5..P8 join a fight only when their "on" is set before the fight loads: the game loads the models then, and at
    // the load it clears the slot and reads "on" right after (an extra that was on is set again in that frame). One
    // switched on once the fight runs (or at a restart, which keeps the loaded models) has no model and no place in
    // the scene: black scene, the camera flies off looking for it. So during a fight only the extras that were on
    // before it are set; one switched on mid-fight joins the next fight. COOPTOOL_LIVE=1 sets them at any time.
    std::array<bool, 9> g_extraOn{};

    void resetExtra(int player) { g_extraOn[static_cast<size_t>(player)] = false; }

    bool armExtra(int player)
    {
        static const bool live = std::getenv("COOPTOOL_LIVE") != nullptr;
        bool &was = g_extraOn[static_cast<size_t>(player)];
        if (!(live || was || !inFight() || read16(field(player, kOn)) != 0))
            return false;
        write16(field(player, kOn), 1);
        was = true;
        return true;
    }

    // --- stage (Free Mode stages in multiplayer) ---
    constexpr uint32_t kStageId = 0x005E8C88u; // 32 bits: the stage of the fight that is loaded
    bool g_stageOverridden = false;
    uint8_t g_menuStage = 0xFF; // the setup's own stage, put back when the option is switched off

    void writePackageName(const char *name)
    {
        char text[24]{};
        std::memcpy(text, name, std::min(std::strlen(name), sizeof(text) - 1));
        g_host->write(kPackageName, text, sizeof(text));
    }

    void applyStage(bool on, int stage)
    {
        if (!on || stage < 0)
        {
            if (g_stageOverridden)
            {
                writePackageName(kStagePackage);
                write8(kSetupStage, g_menuStage);
                g_stageOverridden = false;
            }
            return;
        }
        if (!g_stageOverridden)
        {
            g_host->read(kSetupStage, &g_menuStage, 1);
            g_stageOverridden = true;
        }
        const FreeStage &s = kFreeStages[stage];
        // COOPTOOL_MISSION=<n>: another mission's package (testing which missions load in multiplayer).
        static const char *mission = std::getenv("COOPTOOL_MISSION");
        char name[24];
        std::snprintf(name, sizeof(name), "mission%03d.ppk", mission && *mission ? std::atoi(mission) : s.mission);
        writePackageName(name);
        write8(kSetupStage, static_cast<uint8_t>(s.id));
    }

    struct Point
    {
        float x, z;
    };

    float distance(Point a, Point b) { return std::hypot(a.x - b.x, a.z - b.z); }

    // Spawn points for `needed` fighters: the mission's own within 6 m of the middle one (some start in another part of
    // the stage), then points on the lines between them (on the floor the mission uses), each as far as possible from
    // the others; around the middle point only when it is alone.
    std::vector<Point> spawnPoints(const FreeStage &s, size_t needed)
    {
        std::vector<Point> base;
        for (int i = 0; i < s.count; ++i)
            base.push_back({s.points[i][0] / 1000.0f, s.points[i][1] / 1000.0f});
        size_t middle = 0;
        float best = 1e30f;
        for (size_t i = 0; i < base.size(); ++i)
        {
            float sum = 0.0f;
            for (const Point &p : base)
                sum += distance(base[i], p);
            if (sum < best)
            {
                best = sum;
                middle = i;
            }
        }
        const Point center = base[middle];
        std::vector<Point> points;
        for (const Point &p : base)
            if (distance(p, center) <= 6.0f)
                points.push_back(p);
        std::sort(points.begin(), points.end(), [&](Point a, Point b) { return distance(a, center) < distance(b, center); });
        std::vector<Point> candidates;
        for (size_t i = 0; i < points.size(); ++i)
            for (size_t j = i + 1; j < points.size(); ++j)
                for (float t : {0.25f, 0.5f, 0.75f})
                    candidates.push_back({points[i].x + t * (points[j].x - points[i].x), points[i].z + t * (points[j].z - points[i].z)});
        for (int k = 0; k < 8 && points.size() < 2; ++k) // around the middle point when it is alone
        {
            const float angle = static_cast<float>(k) * 0.785398f;
            candidates.push_back({center.x + 1.5f * std::sin(angle), center.z + 1.5f * std::cos(angle)});
        }
        while (points.size() < needed && !candidates.empty())
        {
            size_t pick = 0;
            float farthest = -1.0f;
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                float nearest = 1e30f;
                for (const Point &p : points)
                    nearest = std::min(nearest, distance(candidates[i], p));
                if (nearest > farthest)
                {
                    farthest = nearest;
                    pick = i;
                }
            }
            points.push_back(candidates[pick]);
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(pick));
        }
        points.resize(std::min(points.size(), needed));
        return points;
    }

    // In the mission package everyone starts on the same spot. When P1..P4 stand exactly on one point in a fight on the
    // chosen stage (the start of a round), the fighters are spread over the spawn points, facing the middle.
    // Only at the start of a round: a grab also puts two fighters on the same point, and moving them there broke the
    // grab (the one grabbed was left floating). The window opens when the fight loads and when everyone's health is back
    // to full after someone was hurt (next round, restart), and closes after placing or after 5 s.
    int g_placeWindow = 0;
    bool g_wasInFight = false;
    bool g_someoneHurt = false;

    void placeFighters(int stage)
    {
        const bool fight = inFight();
        if (fight && !g_wasInFight)
        {
            g_placeWindow = 300;
            g_someoneHurt = false;
        }
        g_wasInFight = fight;
        if (!fight)
            return;
        std::vector<int> players;
        for (int p = 1; p <= 8; ++p)
            if (read16(field(p, kOn)) != 0)
                players.push_back(p);
        bool allFull = !players.empty();
        for (int p : players)
            if (static_cast<int16_t>(read16(field(p, kHp))) < static_cast<int16_t>(read16(field(p, kHpMax))))
                allFull = false; // signed: a knocked-out fighter's health goes below 0
        if (!allFull)
            g_someoneHurt = true;
        else if (g_someoneHurt)
        {
            g_someoneHurt = false;
            g_placeWindow = 300;
        }
        if (g_placeWindow <= 0)
            return;
        --g_placeWindow;
        if (stage < 0 || read32(kStageId) != static_cast<uint32_t>(kFreeStages[stage].id))
            return;
        float first[3]{};
        int together = 0;
        for (int p : players)
        {
            if (p > 4)
                continue;
            float at[3];
            g_host->read(field(p, kPosition), at, sizeof(at));
            if (together == 0)
                std::memcpy(first, at, sizeof(first));
            else if (std::fabs(at[0] - first[0]) > 0.01f || std::fabs(at[2] - first[2]) > 0.01f)
                return;
            ++together;
        }
        if (together < 2)
            return;
        const std::vector<Point> points = spawnPoints(kFreeStages[stage], players.size());
        Point middle{0.0f, 0.0f};
        for (const Point &p : points)
        {
            middle.x += p.x / static_cast<float>(points.size());
            middle.z += p.z / static_cast<float>(points.size());
        }
        for (size_t i = 0; i < players.size() && i < points.size(); ++i)
        {
            float at[3];
            g_host->read(field(players[i], kPosition), at, sizeof(at));
            at[0] = points[i].x;
            at[2] = points[i].z;
            g_host->write(field(players[i], kPosition), at, sizeof(at));
            const float facing = std::atan2(middle.x - points[i].x, middle.z - points[i].z) / 6.2831853f;
            g_host->write(field(players[i], kFacing), &facing, sizeof(facing));
        }
        g_placeWindow = 0;
        char line[96];
        std::snprintf(line, sizeof(line), "%zu fighters spread over %s", players.size(), kFreeStages[stage].name);
        g_host->log(line);
    }

    void onFrame(void *)
    {
        trace();
        // A save state puts back another moment of the game: what was seen of the slots no longer holds.
        static uint32_t loads = 0;
        if (g_host->version >= 2u && g_host->state_loads && g_host->state_loads() != loads)
        {
            loads = g_host->state_loads();
            for (int player = 5; player <= 8; ++player)
                resetExtra(player);
        }
        Config config;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            config = g_config;
        }
        // The stage and the global options work on their own (without the players / bots part), in Multiplayer only:
        // in story and Free Mode P4 is a real enemy and the stage comes from the mission.
        const bool versus = multiplayer();
        applyStage(versus, config.stage);
        if (versus)
        {
            placeFighters(config.stage);
            if (config.sameFighter)
                write8(kSameFighter, 1);
            if (config.forceP4Brad)
            {
                write32(field(4, kSkin), kBrad);
                write32(kMovesTable + 12u, kBrad);
            }
        }
        if (!config.on)
        {
            for (int player = 5; player <= 8; ++player)
                resetExtra(player);
            return;
        }
        // The KO rules are switched off as the Coop Tool did: they cause bugs with 5+ fighters.
        write16(kRegionalKo, 0);
        write16(kTeamAttackKo, 0);
        for (int player = 1; player <= config.humans; ++player)
        {
            const Fighter &f = config.human[static_cast<size_t>(player - 1)];
            if (config.mode == 1)
                write16(field(player, kTeam), static_cast<uint16_t>(f.team));
            applyFighter(player, f, true);
        }
        for (int player = std::max(5, config.humans + 1 + static_cast<int>(config.bots.size())); player <= 8; ++player)
            resetExtra(player);
        for (size_t i = 0; i < config.bots.size(); ++i)
        {
            const Fighter &bot = config.bots[i];
            const int player = config.humans + 1 + static_cast<int>(i);
            if (player > 8)
                break;
            const uint32_t ai = kAis[bot.ai].value;
            if (player <= 4)
            {
                // A CPU of the select screen: the game loads it; team, AI and strength are set here.
                if (config.mode == 1)
                    write16(field(player, kTeam), static_cast<uint16_t>(bot.team));
                if (ai)
                    write32(field(player, kAiProfile), ai);
            }
            else
            {
                if (!armExtra(player))
                    continue;
                write16(field(player, kAi1), 1);
                write16(field(player, kAi2), 1);
                write32(field(player, kAiProfile), ai ? ai : kBotAi);
                write16(field(player, kTeam), static_cast<uint16_t>(config.mode == 1 ? bot.team : 4));
                // P7 and P8 share their model with P2 and P3: they take that character, whatever was picked.
                const uint32_t character = player == 7 ? read32(field(2, kSkin)) : player == 8 ? read32(field(3, kSkin))
                                                                                                : static_cast<uint32_t>(bot.character);
                write32(field(player, kSkin), character);
            }
            applyFighter(player, bot, false);
        }
    }

    // --- the tab (GS thread) ---
    int characterIndex(int id)
    {
        for (int i = 0; i < kCharacterCount; ++i)
            if (kCharacters[i].id == id)
                return i;
        return 0;
    }

    const char *teamName(int team)
    {
        static const char *const names[] = {"A", "B", "C", "D"};
        return names[std::clamp(team, 0, 3)];
    }

    // Strength, health and the limbs switch; infinite SPA only for humans.
    void drawFighterOptions(const UrbanModUi *ui, Fighter &f, bool &changed, bool human)
    {
        static const char *const strength[] = {"Original", "Fraco", "Normal", "Forte", "Chefe"};
        static const char *const strengthEn[] = {"Original", "Weak", "Normal", "Strong", "Boss"};
        static const char *const health[] = {"x1", "x1.5", "x2", "x3"};
        ui->set_next_width(140.0f);
        if (ui->combo(tr("Força", "Strength"), &f.strength, pt() ? strength : strengthEn, 5))
            changed = true;
        ui->same_line();
        ui->set_next_width(90.0f);
        if (ui->combo(tr("Vida", "Health"), &f.health, health, 4))
            changed = true;
        if (human)
        {
            int spa = f.infiniteSpa ? 1 : 0;
            if (ui->checkbox(tr("SPA infinito", "Infinite SPA"), &spa))
            {
                f.infiniteSpa = spa != 0;
                changed = true;
            }
            ui->same_line();
        }
        int limbs = f.unbreakable ? 1 : 0;
        if (ui->checkbox(tr("Membros não quebram", "Limbs do not break"), &limbs))
        {
            f.unbreakable = limbs != 0;
            changed = true;
        }
    }

    void drawSettings(const UrbanModUi *ui, Config &c, bool &changed)
    {
        int on = c.on ? 1 : 0;
        if (ui->checkbox(tr("Jogadores e bots ligados (use só no Multiplayer)", "Players and bots on (use it in Multiplayer only)"), &on))
        {
            c.on = on != 0;
            changed = true;
        }
        ui->text_disabled(tr("Bots extras, personagens e golpes entram na próxima luta; força, vida e o resto valem na hora.",
                             "Extra bots, characters and moves join the next fight; strength, health and the rest apply at once."));
        ui->begin_disabled(!c.on);

        ui->separator_text(tr("1. Jogadores humanos", "1. Human players"));
        for (int n = 1; n <= 4; ++n)
        {
            char label[16];
            std::snprintf(label, sizeof(label), "%d##humans", n);
            if (ui->radio(label, c.humans == n))
            {
                c.humans = n;
                if (static_cast<int>(c.bots.size()) > 8 - n)
                    c.bots.resize(static_cast<size_t>(8 - n));
                changed = true;
            }
            if (n < 4)
                ui->same_line();
        }

        ui->separator_text(tr("2. Times", "2. Teams"));
        if (ui->radio(tr("Cada um por si", "Every one for himself"), c.mode == 0))
        {
            c.mode = 0;
            changed = true;
        }
        ui->same_line();
        if (ui->radio(tr("Times personalizados", "Custom teams"), c.mode == 1))
        {
            c.mode = 1;
            changed = true;
        }

        static const char *const teams[] = {"Time A", "Time B", "Time C", "Time D"};
        static const char *const teamsEn[] = {"Team A", "Team B", "Team C", "Team D"};
        const char *const *teamItems = pt() ? teams : teamsEn;

        ui->separator_text(tr("3. Jogadores", "3. Players"));
        for (int p = 1; p <= c.humans; ++p)
        {
            Fighter &f = c.human[static_cast<size_t>(p - 1)];
            ui->push_id(100 + p);
            char line[64];
            std::snprintf(line, sizeof(line), tr("P%d: humano (controle %d)", "P%d: human (controller %d)"), p, p);
            ui->text(line);
            ui->indent();
            if (c.mode == 1)
            {
                ui->set_next_width(140.0f);
                if (ui->combo(tr("Time", "Team"), &f.team, teamItems, 4))
                    changed = true;
            }
            drawFighterOptions(ui, f, changed, true);
            ui->unindent();
            ui->pop_id();
        }

        ui->separator_text(tr("4. Bots", "4. Bots"));
        static std::array<const char *, kCharacterCount> names = []
        {
            std::array<const char *, kCharacterCount> list{};
            for (int i = 0; i < kCharacterCount; ++i)
                list[static_cast<size_t>(i)] = kCharacters[i].name;
            return list;
        }();
        static std::array<const char *, kAiCount> aiPt = []
        {
            std::array<const char *, kAiCount> list{};
            for (int i = 0; i < kAiCount; ++i)
                list[static_cast<size_t>(i)] = kAis[i].pt;
            return list;
        }();
        static std::array<const char *, kAiCount> aiEn = []
        {
            std::array<const char *, kAiCount> list{};
            for (int i = 0; i < kAiCount; ++i)
                list[static_cast<size_t>(i)] = kAis[i].en;
            return list;
        }();
        for (size_t i = 0; i < c.bots.size(); ++i)
        {
            Fighter &bot = c.bots[i];
            const int player = c.humans + 1 + static_cast<int>(i);
            ui->push_id(200 + static_cast<int>(i));
            char line[96];
            if (player <= 4)
                std::snprintf(line, sizeof(line), tr("P%d: CPU da seleção (escolha o lutador no jogo)", "P%d: select-screen CPU (pick the fighter in the game)"), player);
            else if (player >= 7)
                std::snprintf(line, sizeof(line), tr("P%d: bot extra, com o lutador do P%d", "P%d: extra bot, with P%d's fighter"), player, player - 5);
            else
                std::snprintf(line, sizeof(line), tr("P%d: bot extra", "P%d: extra bot"), player);
            ui->text(line);
            ui->indent();
            if (player == 5 || player == 6)
            {
                int index = characterIndex(bot.character);
                ui->set_next_width(200.0f);
                if (ui->combo(tr("Lutador", "Fighter"), &index, names.data(), kCharacterCount))
                {
                    bot.character = kCharacters[index].id;
                    changed = true;
                }
            }
            if (c.mode == 1)
            {
                ui->set_next_width(140.0f);
                if (ui->combo(tr("Time", "Team"), &bot.team, teamItems, 4))
                    changed = true;
                ui->same_line();
            }
            ui->set_next_width(240.0f);
            if (ui->combo(tr("IA", "AI"), &bot.ai, pt() ? aiPt.data() : aiEn.data(), kAiCount))
                changed = true;
            drawFighterOptions(ui, bot, changed, false);
            ui->unindent();
            ui->pop_id();
        }
        if (ui->button(tr("+ Adicionar bot", "+ Add bot")) && static_cast<int>(c.bots.size()) < 8 - c.humans)
        {
            Fighter bot;
            bot.strength = 2;
            c.bots.push_back(bot);
            changed = true;
        }
        ui->same_line();
        if (ui->button(tr("- Remover bot", "- Remove bot")) && !c.bots.empty())
        {
            c.bots.pop_back();
            changed = true;
        }
        char count[64];
        std::snprintf(count, sizeof(count), tr("%d lutadores no total (máximo 8)", "%d fighters in total (8 at most)"),
                      c.humans + static_cast<int>(c.bots.size()));
        ui->same_line();
        ui->text_disabled(count);
        ui->end_disabled();

        // The stage and the global options do not depend on "On": they can be used alone (Multiplayer only).
        ui->separator_text(tr("5. Fase (EXPERIMENTAL)", "5. Stage (EXPERIMENTAL)"));
        ui->text_colored(0xFF2ABFE8u, tr("Experimental: fases do Free Mode no Multiplayer podem travar no carregamento, fechar o jogo ou ter bugs. Use por sua conta e risco (faça save state antes).",
                                         "Experimental: Free Mode stages in Multiplayer may hang while loading, close the game or have bugs. Use at your own risk (save a state first)."));
        static std::array<const char *, kFreeStageCount + 1> stagesPt = []
        {
            std::array<const char *, kFreeStageCount + 1> list{"A do menu do jogo"};
            for (int i = 0; i < kFreeStageCount; ++i)
                list[static_cast<size_t>(i + 1)] = kFreeStages[i].name;
            return list;
        }();
        static std::array<const char *, kFreeStageCount + 1> stagesEn = []
        {
            std::array<const char *, kFreeStageCount + 1> list{"The one of the game's menu"};
            for (int i = 0; i < kFreeStageCount; ++i)
                list[static_cast<size_t>(i + 1)] = kFreeStages[i].name;
            return list;
        }();
        int stage = c.stage + 1;
        ui->set_next_width(260.0f);
        if (ui->combo(tr("Fase do Free Mode", "Free Mode stage"), &stage, pt() ? stagesPt.data() : stagesEn.data(), kFreeStageCount + 1))
        {
            c.stage = stage - 1;
            changed = true;
        }
        ui->text_disabled(tr("Vale na próxima luta (escolha a fase do menu do jogo qualquer). Os lutadores nascem nos pontos de uma missão daquela fase.",
                             "Applies at the next fight (any stage in the game's menu). The fighters start on the spawn points of a mission on that stage."));

        ui->separator_text(tr("6. Opções globais", "6. Global options"));
        int force = c.forceP4Brad ? 1 : 0;
        if (ui->checkbox(tr("Forçar P4 como Brad (garante os golpes do Brad para os bots)", "Force P4 as Brad (makes sure the bots have Brad's moves)"), &force))
        {
            c.forceP4Brad = force != 0;
            changed = true;
        }
        int same = c.sameFighter ? 1 : 0;
        if (ui->checkbox(tr("Permitir lutador repetido na seleção", "Allow the same fighter twice on the select screen"), &same))
        {
            c.sameFighter = same != 0;
            changed = true;
        }
        ui->text_disabled(tr("Fase e opções globais valem mesmo com os jogadores e bots desligados (só no Multiplayer).",
                             "Stage and global options work even with players and bots off (Multiplayer only)."));
    }

    void drawHowTo(const UrbanModUi *ui, const Config &c)
    {
        const int menu = std::min(4, c.humans + static_cast<int>(c.bots.size()));
        char line[160];
        ui->separator_text(tr("Como usar", "How to use"));
        ui->text_wrapped(tr("1. Ligue o CoopTool na aba Multiplayer e monte a luta.", "1. Switch CoopTool on in the Multiplayer tab and set up the fight."));
        std::snprintf(line, sizeof(line), tr("2. No jogo: Multiplayer > %s.", "2. In the game: Multiplayer > %s."),
                      c.mode == 1 ? "Team Battle" : tr("batalha sem times", "battle without teams"));
        ui->text_wrapped(line);
        std::snprintf(line, sizeof(line), tr("3. Conecte %d controle(s) e coloque %d lutador(es) na seleção: %d humano(s)%s.",
                                             "3. Plug in %d controller(s) and put %d fighter(s) on the select screen: %d human(s)%s."),
                      c.humans, menu, c.humans, menu > c.humans ? " + CPU" : "");
        ui->text_wrapped(line);
        for (int p = 1; p <= menu; ++p)
        {
            const bool human = p <= c.humans;
            const int team = human ? c.human[static_cast<size_t>(p - 1)].team : c.bots[static_cast<size_t>(p - c.humans - 1)].team;
            std::snprintf(line, sizeof(line), "   P%d: %s%s%s", p, human ? tr("humano", "human") : "CPU",
                          c.mode == 1 ? tr(" - Time ", " - Team ") : "", c.mode == 1 ? teamName(team) : "");
            ui->text(line);
        }
        if (c.mode == 1)
            ui->text_wrapped(tr("   Se o menu não deixar montar esses times, monte qualquer divisão com 2+ times: o CoopTool corrige quando a luta começa.",
                                "   If the menu does not let you set up these teams, use any split with 2+ teams: CoopTool fixes it when the fight starts."));
        if (c.humans + static_cast<int>(c.bots.size()) > 4)
            ui->text_wrapped(tr("4. Os bots extras (P5-P8) entram sozinhos quando a luta carrega, em todas as lutas.",
                                "4. The extra bots (P5-P8) join on their own when the fight loads, in every fight."));

        ui->separator_text(tr("Limitações", "Limitations"));
        ui->text_wrapped(tr("- Use só no Multiplayer e desligue no modo história e no Free Mode (lá o P5-P8 são inimigos de verdade).",
                            "- Use it in Multiplayer only; switch it off in story mode and Free Mode (there P5-P8 are real enemies)."));
        ui->text_wrapped(tr("- Bots extras ligados no meio de uma luta só entram na próxima: o jogo só monta os lutadores quando a luta carrega.",
                            "- Extra bots switched on during a fight only join the next one: the game only sets up the fighters when the fight loads."));
        ui->text_wrapped(tr("- Os bots extras usam os golpes do Brad: alguém em P1-P4 precisa estar com o Brad (ou ligue \"Forçar P4 como Brad\"). O lutador escolhido muda só a aparência.",
                            "- The extra bots use Brad's moves: someone in P1-P4 has to be Brad (or switch \"Force P4 as Brad\" on). The fighter you pick only changes the looks."));
        ui->text_wrapped(tr("- O P7 usa o lutador do P2 e o P8 o do P3: eles dividem o modelo, e o CoopTool copia sozinho.",
                            "- P7 uses P2's fighter and P8 uses P3's: they share the model, and CoopTool copies it on its own."));
        ui->text_wrapped(tr("- O fim da luta só olha P1-P4: os bots extras não contam para vencer.",
                            "- The end of the fight only looks at P1-P4: the extra bots do not count for the win."));
        ui->text_wrapped(tr("- CPUs da seleção precisam de lutadores liberados no seu save. A IA \"Padrão\" deles é o nível escolhido no menu do jogo.",
                            "- Select-screen CPUs need fighters unlocked in your save. Their \"Default\" AI is the level picked in the game's menu."));
        ui->text_wrapped(tr("- Com o CoopTool ligado, o KO regional e o KO por ataque do próprio time ficam desligados (evitam travamentos com 5+ lutadores).",
                            "- With CoopTool on, regional KO and KO by your own team's attacks are off (they freeze the game with 5+ fighters)."));
        ui->text_wrapped(tr("- Fase do Free Mode (experimental): o jogo carrega o pacote de uma missão daquela fase; alguns têm objetos de missão que travam o carregamento ou fecham o jogo. Testados: Back Alley, Chinatown, Bordin Bldg. Top Floor.",
                            "- Free Mode stage (experimental): the game loads the package of a mission on that stage; some have mission objects that hang the loading or close the game. Tested: Back Alley, Chinatown, Bordin Bldg. Top Floor."));
    }

    void drawTab(const UrbanModUi *ui, void *)
    {
        Config config;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            config = g_config;
        }
        bool changed = false;
        if (ui->begin_tab_bar("##cooptool"))
        {
            if (ui->begin_tab("Multiplayer"))
            {
                drawSettings(ui, config, changed);
                ui->end_tab();
            }
            if (ui->begin_tab(tr("Como usar", "How to use")))
            {
                drawHowTo(ui, config);
                ui->end_tab();
            }
            ui->end_tab_bar();
        }
        if (changed)
        {
            saveConfig(config);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_config = config;
        }
    }
}

URBAN_MOD_EXPORT int urban_mod_init(const UrbanModHost *host)
{
    if (host->version < 1u)
        return 0;
    g_host = host;
    g_config = loadConfig();
    host->set_tab(drawTab, nullptr);
    host->on_frame(onFrame, nullptr);
    host->log(g_config.on ? "CoopTool on" : "CoopTool loaded (off until switched on in its tab)");
    return 1;
}
