// Mod Organizer 2 helpers shared by the plugin and rowe_mo2setup.exe (see mo2common.h).
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include "mo2common.h"

bool Mo2Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Mo2Dir(const std::string& p)
{
    size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string() : p.substr(0, i + 1);
}

std::string Mo2Norm(std::string p)
{
    for (char& c : p) c = c == '/' ? '\\' : (char)tolower((unsigned char)c);
    std::string out;
    for (size_t i = 0; i < p.size(); ++i)
        if (!(p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\')) out += p[i];
    return out;
}

// Portable: next to ModOrganizer.exe; otherwise the current instance under %LOCALAPPDATA%\ModOrganizer.
std::string Mo2IniFor(const std::string& moExe)
{
    std::string ini = Mo2Dir(moExe) + "ModOrganizer.ini";
    if (Mo2Exists(ini)) return ini;
    char inst[MAX_PATH] = "", local[MAX_PATH];
    DWORD size = sizeof(inst);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Mod Organizer Team\\Mod Organizer", "CurrentInstance", RRF_RT_REG_SZ, nullptr, inst,
                     &size) == ERROR_SUCCESS &&
        inst[0] && SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        ini = std::string(local) + "\\ModOrganizer\\" + inst + "\\ModOrganizer.ini";
        if (Mo2Exists(ini)) return ini;
    }
    return "";
}

bool Mo2ReadIni(const std::string& path, Mo2IniText* ini)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    if (text.find("\r\n") == std::string::npos) ini->eol = "\n";
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ini->lines.push_back(line);
        start = end + 1;
    }
    return true;
}

bool Mo2WriteIni(const std::string& path, const Mo2IniText& ini)
{
    std::string tmp = path + ".rowe-tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    for (const std::string& l : ini.lines) fprintf(f, "%s%s", l.c_str(), ini.eol.c_str());
    fclose(f);
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

static std::string Unquote(std::string v)
{
    if (v.rfind("@ByteArray(", 0) == 0 && v.size() > 12 && v.back() == ')') v = v.substr(11, v.size() - 12);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i;
        out += v[i];
    }
    return out;
}

int Mo2Section(const Mo2IniText& ini, const char* name, int* end)
{
    std::string head = std::string("[") + name + "]";
    for (int i = 0; i < (int)ini.lines.size(); ++i)
        if (ini.lines[i] == head) {
            int j = i + 1;
            while (j < (int)ini.lines.size() && !(ini.lines[j].size() > 1 && ini.lines[j][0] == '[')) ++j;
            while (j > i + 1 && ini.lines[j - 1].empty()) --j;  // keep the blank line before the next section
            *end = j;
            return i;
        }
    return -1;
}

std::string Mo2Get(const Mo2IniText& ini, const char* section, const std::string& key)
{
    int end, s = Mo2Section(ini, section, &end);
    if (s < 0) return "";
    for (int i = s + 1; i < end; ++i)
        if (ini.lines[i].compare(0, key.size() + 1, key + "=") == 0) return Unquote(ini.lines[i].substr(key.size() + 1));
    return "";
}

void Mo2Set(Mo2IniText* ini, const char* section, const std::string& key, const std::string& value)
{
    int end, s = Mo2Section(*ini, section, &end);
    if (s < 0) {
        if (!ini->lines.empty() && !ini->lines.back().empty()) ini->lines.push_back("");
        ini->lines.push_back(std::string("[") + section + "]");
        ini->lines.push_back(key + "=" + value);
        return;
    }
    for (int i = s + 1; i < end; ++i)
        if (ini->lines[i].compare(0, key.size() + 1, key + "=") == 0) {
            ini->lines[i] = key + "=" + value;
            return;
        }
    ini->lines.insert(ini->lines.begin() + end, key + "=" + value);
}

std::string Mo2FindInMods(const Mo2IniText& ini, const std::string& iniPath, const char* relative)
{
    std::string base = Mo2Get(ini, "Settings", "base_directory");
    if (base.empty()) base = Mo2Dir(iniPath);
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    auto resolve = [&](std::string v, const char* def) {
        if (v.empty()) v = std::string("%BASE_DIR%/") + def;
        size_t at = v.find("%BASE_DIR%");
        if (at != std::string::npos) v.replace(at, 10, base);
        for (char& c : v)
            if (c == '/') c = '\\';
        if (!v.empty() && v.back() != '\\') v += '\\';
        return v;
    };
    std::string mods = resolve(Mo2Get(ini, "Settings", "mod_directory"), "mods");
    std::string profiles = resolve(Mo2Get(ini, "Settings", "profiles_directory"), "profiles");
    std::string enabled;
    if (FILE* f = fopen((profiles + Mo2Get(ini, "General", "selected_profile") + "\\modlist.txt").c_str(), "rb")) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) enabled += line;
        fclose(f);
    }
    std::string first, chosen;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((mods + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        std::string path = mods + fd.cFileName + "\\" + relative;
        if (!Mo2Exists(path)) continue;
        if (first.empty()) first = path;
        std::string plus = std::string("+") + fd.cFileName;
        if (chosen.empty() && (enabled.find(plus + "\n") != std::string::npos || enabled.find(plus + "\r") != std::string::npos))
            chosen = path;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return chosen.empty() ? first : chosen;
}
