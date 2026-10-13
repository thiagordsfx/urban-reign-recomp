#include "urban/mods.h"


#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace urban::mods
{
    namespace
    {
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

        // "[name]" -> "name" (lower case), else "".
        std::string sectionOf(const std::string &line)
        {
            const std::string text = trim(line);
            return text.size() >= 2u && text.front() == '[' && text.back() == ']' ? lower(trim(text.substr(1, text.size() - 2))) : "";
        }

        // "key=value" with key = Enabled (any case, not commented out).
        bool isEnabledLine(const std::string &line, std::string *value)
        {
            const std::string text = trim(line);
            const size_t equals = text.find('=');
            if (text.empty() || text[0] == '#' || text[0] == ';' || equals == std::string::npos ||
                lower(trim(text.substr(0, equals))) != "enabled")
                return false;
            if (value)
                *value = trim(text.substr(equals + 1));
            return true;
        }

        std::vector<std::string> split(const std::string &list)
        {
            std::vector<std::string> names;
            std::istringstream stream(list);
            std::string name;
            while (std::getline(stream, name, ';'))
                if (!(name = trim(name)).empty())
                    names.push_back(name);
            return names;
        }

    }

    std::string readEnabled(const std::filesystem::path &configPath)
    {
        std::ifstream file(configPath);
        std::string line, section, enabled, value;
        while (std::getline(file, line))
        {
            if (const std::string name = sectionOf(line); !name.empty())
                section = name;
            else if (section == "mods" && isEnabledLine(line, &value))
                enabled = value;
        }
        return enabled;
    }

    std::vector<std::string> pnachFiles(const std::filesystem::path &configPath, const std::string &enabled, bool log)
    {
        std::vector<std::string> files;
        for (const std::string &name : split(enabled))
        {
            const std::filesystem::path file = configPath.parent_path() / "mods" / name;
            if (std::filesystem::exists(file))
                files.push_back(file.string());
            else if (log)
                std::cerr << "[mods] " << name << " is not in the mods folder" << std::endl;
        }
        return files;
    }
}
