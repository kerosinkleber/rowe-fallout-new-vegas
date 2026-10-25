// Mod Organizer 2 setup, run as "rowe_launcher.exe --mo2-setup <ModOrganizer.ini> <ModOrganizer.exe>
// [start] [shortcut]" (started by the plugin when the installer's options asked for it; see
// src/mo2setup.cpp). No window. It waits until that Mod Organizer has closed (it keeps its
// executables and the selected one in memory and writes the selection back when it closes, so a
// change made while it runs would be lost), then:
// - start: makes the ROWE launcher MO2's start option: an entry under [customExecutables] (the one
//   whose binary is this mod's rowe_launcher.exe if there is one, else a new "ROWE Launcher") and
//   [Widgets] MainWindow_executablesListBox_index pointing at it (the list shows "<Edit...>" first,
//   then the entries that are not hidden);
// - shortcut: a desktop shortcut that starts that entry through Mod Organizer
//   (ModOrganizer.exe "moshortcut://<instance>:<title>", the instance empty for a portable one).
// ModOrganizer.ini is copied to ModOrganizer.ini.rowe-backup first. The steps are logged to
// rowe_mo2setup.log in the instance's logs folder.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>

static FILE* s_log;

static void Say(const char* fmt, ...)
{
    if (!s_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(s_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfprintf(s_log, fmt, a);
    va_end(a);
    fprintf(s_log, "\n");
    fflush(s_log);
}

static bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string Dir(const std::string& p)
{
    size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string() : p.substr(0, i + 1);
}
static std::string Norm(std::string p)  // for comparing paths
{
    for (char& c : p) c = c == '/' ? '\\' : (char)tolower((unsigned char)c);
    std::string out;
    for (size_t i = 0; i < p.size(); ++i)
        if (!(p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\')) out += p[i];
    return out;
}
static std::string Forward(std::string p)
{
    for (char& c : p)
        if (c == '\\') c = '/';
    return p;
}

// Is a ModOrganizer.exe with this image path running?
static bool Running(const std::string& exe)
{
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = {sizeof(pe)};
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, "ModOrganizer.exe")) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        char path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(h, 0, path, &size) && Norm(path) == Norm(exe)) found = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

// The ini as lines; QSettings values: @ByteArray(...) and doubled backslashes.
struct Ini {
    std::vector<std::string> lines;
    std::string eol = "\r\n";
};

static bool ReadIni(const std::string& path, Ini* ini)
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

static bool WriteIni(const std::string& path, const Ini& ini)
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

// Section [name]: first line index and the index after its last line; -1 if missing.
static int Section(const Ini& ini, const char* name, int* end)
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

static std::string Get(const Ini& ini, const char* section, const std::string& key)
{
    int end, s = Section(ini, section, &end);
    if (s < 0) return "";
    for (int i = s + 1; i < end; ++i)
        if (ini.lines[i].compare(0, key.size() + 1, key + "=") == 0) return Unquote(ini.lines[i].substr(key.size() + 1));
    return "";
}

static void Set(Ini* ini, const char* section, const std::string& key, const std::string& value)
{
    int end, s = Section(*ini, section, &end);
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

// This mod's rowe_launcher.exe in Mod Organizer's mods folder (the enabled one when several).
static std::string FindLauncher(const Ini& ini, const std::string& instanceDir)
{
    std::string base = Get(ini, "Settings", "base_directory");
    if (base.empty()) base = instanceDir;
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
    std::string mods = resolve(Get(ini, "Settings", "mod_directory"), "mods");
    std::string profiles = resolve(Get(ini, "Settings", "profiles_directory"), "profiles");
    std::string profile = Get(ini, "General", "selected_profile");
    std::string enabled;
    if (FILE* f = fopen((profiles + profile + "\\modlist.txt").c_str(), "rb")) {
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
        std::string exe = mods + fd.cFileName + "\\NVSE\\Plugins\\rowe_launcher.exe";
        if (!Exists(exe)) continue;
        if (first.empty()) first = exe;
        if (chosen.empty() && enabled.find(std::string("+") + fd.cFileName + "\n") != std::string::npos) chosen = exe;
        if (chosen.empty() && enabled.find(std::string("+") + fd.cFileName + "\r") != std::string::npos) chosen = exe;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return chosen.empty() ? first : chosen;
}

static bool CreateShortcut(const std::string& lnk, const std::string& target, const std::string& args, const std::string& workDir,
                           const std::string& icon, const std::string& description)
{
    IShellLinkA* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkA, (void**)&link))) return false;
    link->SetPath(target.c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    link->SetDescription(description.c_str());
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file))) {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, lnk.c_str(), -1, w, MAX_PATH);
        ok = SUCCEEDED(file->Save(w, TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

int Mo2SetupMain(int argc, char** argv)
{
    if (argc < 4) return 2;
    std::string iniPath = argv[2], moExe = argv[3];
    bool start = false, shortcut = false;
    for (int i = 4; i < argc; ++i) {
        if (!_stricmp(argv[i], "start")) start = true;
        if (!_stricmp(argv[i], "shortcut")) shortcut = true;
    }
    std::string instanceDir = Dir(iniPath);
    std::string logDir = Exists(instanceDir + "logs") ? instanceDir + "logs\\" : instanceDir;
    s_log = fopen((logDir + "rowe_mo2setup.log").c_str(), "a");
    Say("ROWE Mod Organizer 2 setup: ini %s, Mod Organizer %s, start option %d, desktop shortcut %d", iniPath.c_str(), moExe.c_str(),
        start, shortcut);
    HANDLE once = CreateMutexA(nullptr, TRUE, "ROWE_MO2_SETUP");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Say("another setup is already waiting; this one ends");
        return 0;
    }
    if (Running(moExe)) {
        Say("waiting for Mod Organizer 2 to close (it would overwrite the change)");
        while (Running(moExe)) Sleep(2000);
        Sleep(1500);  // let it finish writing its ini
    }
    Ini ini;
    if (!ReadIni(iniPath, &ini)) {
        Say("cannot read %s", iniPath.c_str());
        return 1;
    }
    std::string launcher = FindLauncher(ini, instanceDir);
    if (launcher.empty()) {
        Say("rowe_launcher.exe not found in Mod Organizer's mods folder; nothing changed");
        return 1;
    }
    Say("the launcher: %s", launcher.c_str());

    // The executable entry: this launcher's, or a new one.
    int end, s = Section(ini, "customExecutables", &end);
    int count = atoi(Get(ini, "customExecutables", "size").c_str());
    int mine = 0;
    std::string title;
    for (int i = 1; i <= count; ++i)
        if (Norm(Get(ini, "customExecutables", std::to_string(i) + "\\binary")) == Norm(launcher)) {
            mine = i;
            title = Get(ini, "customExecutables", std::to_string(i) + "\\title");
        }
    bool changed = false;
    if (start || shortcut) {
        if (!mine) {
            mine = count + 1;
            title = "ROWE Launcher";
            for (int i = 1; i <= count; ++i)
                if (Get(ini, "customExecutables", std::to_string(i) + "\\title") == title) title = "ROWE Launcher (ROWE)";
            std::string n = std::to_string(mine) + "\\";
            Set(&ini, "customExecutables", "size", std::to_string(mine));
            Set(&ini, "customExecutables", n + "arguments", "");
            Set(&ini, "customExecutables", n + "binary", Forward(launcher));
            Set(&ini, "customExecutables", n + "hide", "false");
            Set(&ini, "customExecutables", n + "ownicon", "false");
            Set(&ini, "customExecutables", n + "steamAppID", "");
            Set(&ini, "customExecutables", n + "title", title);
            Set(&ini, "customExecutables", n + "toolbar", "false");
            Set(&ini, "customExecutables", n + "workingDirectory", "");
            Say("added executable %d \"%s\"", mine, title.c_str());
            changed = true;
        } else {
            Say("executable %d \"%s\" already starts it", mine, title.c_str());
            if (Get(ini, "customExecutables", std::to_string(mine) + "\\hide") == "true") {
                Set(&ini, "customExecutables", std::to_string(mine) + "\\hide", "false");
                changed = true;
            }
        }
    }
    (void)s;
    (void)end;
    if (start) {
        int index = 1;  // "<Edit...>" comes first
        for (int i = 1; i < mine; ++i)
            if (Get(ini, "customExecutables", std::to_string(i) + "\\hide") != "true") ++index;
        Set(&ini, "Widgets", "MainWindow_executablesListBox_index", std::to_string(index));
        Say("start option: list entry %d", index);
        changed = true;
    }
    if (changed) {
        std::string backup = iniPath + ".rowe-backup";
        if (!CopyFileA(iniPath.c_str(), backup.c_str(), FALSE)) Say("could not back up the ini; nothing changed");
        else if (!WriteIni(iniPath, ini)) Say("could not write %s (backup: %s)", iniPath.c_str(), backup.c_str());
        else Say("ModOrganizer.ini updated (backup: %s)", backup.c_str());
    }
    if (shortcut) {
        CoInitialize(nullptr);
        char desktop[MAX_PATH];
        std::string instance = Norm(Dir(iniPath)) == Norm(Dir(moExe)) ? "" : instanceDir.substr(0, instanceDir.size() - 1);
        if (!instance.empty()) instance = instance.substr(instance.find_last_of('\\') + 1);
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
            std::string lnk = std::string(desktop) + "\\Fallout New Vegas (ROWE).lnk";
            std::string game = Get(ini, "General", "gamePath");
            std::string icon = game.empty() ? "" : game + "\\FalloutNV.exe";
            bool ok = CreateShortcut(lnk, moExe, "\"moshortcut://" + instance + ":" + title + "\"", Dir(moExe), icon,
                                     "Fallout: New Vegas through Mod Organizer 2, with the ROWE launcher");
            Say(ok ? "desktop shortcut %s" : "could not create the desktop shortcut %s", lnk.c_str());
        }
        CoUninitialize();
    }
    Say("done");
    if (s_log) fclose(s_log);
    return 0;
}
