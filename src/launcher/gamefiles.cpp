// The launcher's view of the game: where it is, its INI files, saves and archives (see gamefiles.h).
#include "gamefiles.h"
#include "oldnames.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "hudstyle.h"

static bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string Slash(std::string s)
{
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s;
}
static std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ---- finding the game ------------------------------------------------------------------------

// The process that started this one.
static bool ParentExe(char* out, DWORD size)
{
    DWORD self = GetCurrentProcessId(), parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = {sizeof(pe)};
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (pe.th32ProcessID == self) parent = pe.th32ParentProcessID;
    CloseHandle(snap);
    if (!parent) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
    if (!h) return false;
    bool ok = QueryFullProcessImageNameA(h, 0, out, &size) != 0;
    CloseHandle(h);
    return ok;
}

// A value of Mod Organizer 2's ini: key=@ByteArray(value) or key=value, backslashes doubled.
static std::string Mo2Value(const std::string& ini, const char* key)
{
    char raw[1024];
    GetPrivateProfileStringA("General", key, "", raw, sizeof(raw), ini.c_str());
    std::string v = raw;
    if (v.rfind("@ByteArray(", 0) == 0 && v.size() > 12 && v.back() == ')') v = v.substr(11, v.size() - 12);
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i;
        out += v[i] == '/' ? '\\' : v[i];
    }
    return out;
}

// Started by Mod Organizer 2: its instance's ini (portable: next to ModOrganizer.exe; otherwise
// %LOCALAPPDATA%\ModOrganizer\<current instance>).
static std::string Mo2Ini()
{
    char exe[MAX_PATH];
    if (!ParentExe(exe, MAX_PATH)) return "";
    char* name = strrchr(exe, '\\');
    if (!name || _stricmp(name + 1, "ModOrganizer.exe")) return "";
    name[1] = 0;
    std::string ini = std::string(exe) + "ModOrganizer.ini";
    if (Exists(ini)) return ini;
    char inst[MAX_PATH] = "";
    DWORD size = sizeof(inst);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Mod Organizer Team\\Mod Organizer", "CurrentInstance", RRF_RT_REG_SZ, nullptr, inst, &size) ==
        ERROR_SUCCESS && inst[0]) {
        char local[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
            ini = std::string(local) + "\\ModOrganizer\\" + inst + "\\ModOrganizer.ini";
            if (Exists(ini)) return ini;
        }
    }
    return "";
}

bool FindGame(GamePaths* p)
{
    std::string mo2 = Mo2Ini();
    if (!mo2.empty()) {
        p->mo2 = mo2;
        p->mo2Profile = Mo2Value(mo2, "selected_profile");
        std::string g = Mo2Value(mo2, "gamePath");
        if (!g.empty() && Exists(Slash(g) + "FalloutNV.exe")) {
            p->game = Slash(g);
            p->how = "Mod Organizer 2";
        }
    }
    // Next to the launcher or above it (Data\NVSE\Plugins), then the working folder.
    if (p->game.empty()) {
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string dir = self;
        for (int up = 0; up < 5 && p->game.empty(); ++up) {
            size_t s = dir.find_last_of('\\');
            if (s == std::string::npos) break;
            dir = dir.substr(0, s);
            if (Exists(dir + "\\FalloutNV.exe")) { p->game = dir + "\\"; p->how = "launcher folder"; }
        }
    }
    if (p->game.empty()) {
        char cwd[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, cwd);
        if (Exists(Slash(cwd) + "FalloutNV.exe")) { p->game = Slash(cwd); p->how = "working folder"; }
    }
    if (p->game.empty()) {
        char path[MAX_PATH] = "";
        DWORD size = sizeof(path);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "Software\\Bethesda Softworks\\FalloutNV", "Installed Path", RRF_RT_REG_SZ, nullptr, path, &size) ==
                ERROR_SUCCESS && Exists(Slash(path) + "FalloutNV.exe")) {
            p->game = Slash(path);
            p->how = "registry";
        }
    }
    char docs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) p->myGames = std::string(docs) + "\\My Games\\FalloutNV\\";
    if (!p->game.empty()) {
        TakeOverOldIni((p->game + "Data\\NVSE\\Plugins\\").c_str());  // the settings of fnvfov, the mod's former name
        p->modIni = p->game + "Data\\NVSE\\Plugins\\rowe.ini";
    }
    return !p->game.empty();
}

bool GameRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = {sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
        found = !_stricmp(pe.szExeFile, "FalloutNV.exe");
    CloseHandle(snap);
    return found;
}

// ---- INI files -------------------------------------------------------------------------------

static const char kMissing[] = "\x01";

bool Ini::Has(const std::string& file, const char* section, const char* key) const
{
    if (file.empty()) return false;
    char v[8];
    GetPrivateProfileStringA(section, key, kMissing, v, sizeof(v), file.c_str());
    return strcmp(v, kMissing) != 0;
}

IniFile Ini::Where(const char* section, const char* key) const
{
    if (Has(custom, section, key)) return kIniCustom;
    if (Has(prefs, section, key)) return kIniPrefs;
    if (Has(fallout, section, key)) return kIniDefault;
    return kIniNone;
}

std::string Ini::Get(const char* section, const char* key, const char* def) const
{
    const std::string* file = nullptr;
    switch (Where(section, key)) {
    case kIniCustom: file = &custom; break;
    case kIniPrefs: file = &prefs; break;
    case kIniDefault: file = &fallout; break;
    default: return def;
    }
    char v[1024];
    GetPrivateProfileStringA(section, key, def, v, sizeof(v), file->c_str());
    // Values may carry a comment (FalloutCustom.ini: "1400 ; ...").
    std::string s = v;
    size_t c = s.find(';');
    if (c != std::string::npos) s = s.substr(0, c);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

int Ini::GetInt(const char* section, const char* key, int def) const
{
    std::string v = Get(section, key, "");
    if (v.empty()) return def;
    return (int)strtoll(v.c_str(), nullptr, 0);
}

float Ini::GetFloat(const char* section, const char* key, float def) const
{
    std::string v = Get(section, key, "");
    return v.empty() ? def : (float)atof(v.c_str());
}

void Ini::BackupOnce()
{
    if (backedUp) return;
    backedUp = true;
    for (const std::string* f : {&prefs, &custom}) {
        if (f->empty() || !Exists(*f)) continue;
        std::string bak = *f + ".rowe-launcher.bak";
        if (!Exists(bak)) CopyFileA(f->c_str(), bak.c_str(), TRUE);
    }
}

bool Ini::Set(const char* section, const char* key, const char* value)
{
    BackupOnce();
    const std::string& file = Has(custom, section, key) ? custom : prefs;
    return WritePrivateProfileStringA(section, key, value, file.c_str()) != 0;
}

bool Ini::SetInt(const char* section, const char* key, int value)
{
    char v[32];
    wsprintfA(v, "%d", value);
    return Set(section, key, v);
}

bool Ini::SetFloat(const char* section, const char* key, float value)
{
    char v[32];
    _snprintf(v, sizeof(v) - 1, "%.4f", value);
    v[sizeof(v) - 1] = 0;
    return Set(section, key, v);
}

// ---- saves -----------------------------------------------------------------------------------

// Header: "FO3SAVEGAME", header size, version, then fields separated by '|': language (64 bytes),
// screenshot width and height, save number, player name, karma title, level, location, play time
// (strings: u16 length, '|', text), then the screenshot (RGB).
static bool ParseSave(const std::string& path, SaveInfo* s)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char h[1024];
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 100 || memcmp(h, "FO3SAVEGAME", 11)) return false;
    size_t pos = 11 + 8;
    auto bar = [&]() { return pos < n && h[pos++] == '|'; };
    auto u32 = [&](int* v) {
        if (pos + 4 > n) return false;
        *v = h[pos] | h[pos + 1] << 8 | h[pos + 2] << 16 | h[pos + 3] << 24;
        pos += 4;
        return bar();
    };
    auto str = [&](std::string* v) {
        if (pos + 3 > n) return false;
        int len = h[pos] | h[pos + 1] << 8;
        pos += 2;
        if (!bar() || pos + len > n) return false;
        v->assign((const char*)h + pos, len);
        pos += len;
        return bar();
    };
    if (!bar()) return false;
    // The language field: up to the next '|'.
    while (pos < n && h[pos] != '|') ++pos;
    if (!bar()) return false;
    if (!u32(&s->shotW) || !u32(&s->shotH) || !u32(&s->number) || !str(&s->player) || !str(&s->karma) || !u32(&s->level) ||
        !str(&s->location) || !str(&s->playTime))
        return false;
    s->shotOffset = (long long)pos;
    if (s->shotW <= 0 || s->shotH <= 0 || s->shotW > 4096 || s->shotH > 4096) s->shotW = s->shotH = 0;
    return true;
}

std::vector<SaveInfo> ListSaves(const std::string& dir)
{
    std::vector<SaveInfo> list;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.fos").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return list;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SaveInfo s;
        s.file = fd.cFileName;
        s.file.resize(s.file.size() - 4);
        s.written = fd.ftLastWriteTime;
        if (!ParseSave(dir + fd.cFileName, &s)) s.location = "(unreadable save)";
        list.push_back(s);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(list.begin(), list.end(), [](const SaveInfo& a, const SaveInfo& b) { return CompareFileTime(&a.written, &b.written) > 0; });
    return list;
}

bool LoadSaveShot(const std::string& dir, const SaveInfo& s, std::vector<unsigned char>* bgra)
{
    if (s.shotW <= 0) return false;
    FILE* f = fopen((dir + s.file + ".fos").c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> rgb((size_t)s.shotW * s.shotH * 3);
    bool ok = _fseeki64(f, s.shotOffset, SEEK_SET) == 0 && fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    fclose(f);
    if (!ok) return false;
    bgra->resize((size_t)s.shotW * s.shotH * 4);
    for (size_t i = 0, n = (size_t)s.shotW * s.shotH; i < n; ++i) {
        (*bgra)[i * 4 + 0] = rgb[i * 3 + 2];
        (*bgra)[i * 4 + 1] = rgb[i * 3 + 1];
        (*bgra)[i * 4 + 2] = rgb[i * 3 + 0];
        (*bgra)[i * 4 + 3] = 255;
    }
    return true;
}

// ---- zlib (archived files) -------------------------------------------------------------------
// A small inflate after zlib's puff.c.

struct Inflater {
    const unsigned char* in;
    size_t inLen, inPos;
    unsigned bitbuf;
    int bitcnt;
    unsigned char* out;
    size_t outLen, outPos;
    bool err;
};

static int Bits(Inflater* s, int need)
{
    unsigned long long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inPos >= s->inLen) { s->err = true; return 0; }
        val |= (unsigned long long)s->in[s->inPos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (unsigned)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1ull << need) - 1));
}

struct Huff { short count[16]; short symbol[320]; };

static int Decode(Inflater* s, const Huff* h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= Bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int Construct(Huff* h, const short* length, int n)
{
    for (int len = 0; len <= 15; ++len) h->count[len] = 0;
    for (int sym = 0; sym < n; ++sym) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; ++sym)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                  6145, 8193, 12289, 16385, 24577};
static const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool Codes(Inflater* s, const Huff* lencode, const Huff* distcode)
{
    for (;;) {
        int sym = Decode(s, lencode);
        if (sym < 0 || s->err) return false;
        if (sym < 256) {
            if (s->outPos >= s->outLen) return false;
            s->out[s->outPos++] = (unsigned char)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + Bits(s, kLenExtra[sym]);
            int ds = Decode(s, distcode);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = kDistBase[ds] + Bits(s, kDistExtra[ds]);
            if (s->err || dist > s->outPos || s->outPos + len > s->outLen) return false;
            for (; len > 0; --len, ++s->outPos) s->out[s->outPos] = s->out[s->outPos - dist];
        }
    }
}

static bool Inflate(Inflater* s)
{
    static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int last;
    do {
        last = Bits(s, 1);
        int type = Bits(s, 2);
        if (s->err) return false;
        if (type == 0) {  // stored
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->inPos + 4 > s->inLen) return false;
            size_t len = s->in[s->inPos] | s->in[s->inPos + 1] << 8;
            s->inPos += 4;
            if (s->inPos + len > s->inLen || s->outPos + len > s->outLen) return false;
            memcpy(s->out + s->outPos, s->in + s->inPos, len);
            s->inPos += len;
            s->outPos += len;
        } else if (type == 1) {  // fixed codes
            static Huff lencode, distcode;
            static bool built;
            if (!built) {
                short lengths[288];
                int sym = 0;
                for (; sym < 144; ++sym) lengths[sym] = 8;
                for (; sym < 256; ++sym) lengths[sym] = 9;
                for (; sym < 280; ++sym) lengths[sym] = 7;
                for (; sym < 288; ++sym) lengths[sym] = 8;
                Construct(&lencode, lengths, 288);
                for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
                Construct(&distcode, lengths, 30);
                built = true;
            }
            if (!Codes(s, &lencode, &distcode)) return false;
        } else if (type == 2) {  // dynamic codes
            int nlen = Bits(s, 5) + 257, ndist = Bits(s, 5) + 1, ncode = Bits(s, 4) + 4;
            if (s->err || nlen > 286 || ndist > 30) return false;
            short lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = (short)Bits(s, 3);
            Huff lencode, distcode;
            if (Construct(&lencode, lengths, 19) != 0) return false;
            for (int index = 0; index < nlen + ndist;) {
                int sym = Decode(s, &lencode);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[index++] = (short)sym;
                } else {
                    short len = 0;
                    int rep;
                    if (sym == 16) {
                        if (index == 0) return false;
                        len = lengths[index - 1];
                        rep = 3 + Bits(s, 2);
                    } else if (sym == 17) {
                        rep = 3 + Bits(s, 3);
                    } else {
                        rep = 11 + Bits(s, 7);
                    }
                    if (index + rep > nlen + ndist) return false;
                    while (rep--) lengths[index++] = len;
                }
            }
            if (lengths[256] == 0) return false;
            int err = Construct(&lencode, lengths, nlen);
            if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
            err = Construct(&distcode, lengths + nlen, ndist);
            if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
            if (!Codes(s, &lencode, &distcode)) return false;
        } else {
            return false;
        }
    } while (!last);
    return !s->err;
}

static bool ZlibDecompress(const unsigned char* in, size_t inLen, std::vector<unsigned char>* out, size_t outLen)
{
    if (inLen < 2 || (in[0] & 0x0F) != 8) return false;
    out->resize(outLen);
    Inflater s = {in + 2, inLen - 2, 0, 0, 0, out->data(), outLen, 0, false};
    return Inflate(&s) && s.outPos == outLen;
}

// ---- BSA archives (version 104) ----------------------------------------------------------------

// The directory of an archive: each file's folder, name, size and offset.
struct Bsa {
    struct Header { char magic[4]; DWORD version, offset, flags, folders, files, folderNames, fileNames, fileFlags; } hd;
    struct FileRec { std::string folder; const char* name; DWORD size, offset; };
    std::vector<FileRec> recs;
    std::vector<char> names;
    FILE* f = nullptr;
    ~Bsa() { if (f) fclose(f); }

    bool Open(const std::string& archive, DWORD needFileFlags = 0)  // fileFlags: 1 meshes, 2 textures, ...
    {
        f = fopen(archive.c_str(), "rb");
        if (!f) return false;
        if (fread(&hd, sizeof(hd), 1, f) != 1 || memcmp(hd.magic, "BSA\0", 4) || (hd.version != 103 && hd.version != 104) || (hd.flags & 3) != 3 ||
            (hd.fileFlags & needFileFlags) != needFileFlags)
            return false;
        std::vector<DWORD> counts(hd.folders);
        _fseeki64(f, hd.offset, SEEK_SET);
        for (DWORD i = 0; i < hd.folders; ++i) {
            unsigned char rec[16];
            if (fread(rec, 16, 1, f) != 1) return false;
            counts[i] = *(DWORD*)(rec + 8);
        }
        recs.reserve(hd.files);
        for (DWORD i = 0; i < hd.folders; ++i) {
            int len = fgetc(f);
            if (len <= 0) return false;
            std::string folder(len, 0);
            if (fread(&folder[0], 1, len, f) != (size_t)len) return false;
            folder.resize(strlen(folder.c_str()));
            for (DWORD k = 0; k < counts[i]; ++k) {
                unsigned char rec[16];
                if (fread(rec, 16, 1, f) != 1) return false;
                recs.push_back({folder, nullptr, *(DWORD*)(rec + 8), *(DWORD*)(rec + 12)});
            }
        }
        names.resize(hd.fileNames);
        if (recs.size() != hd.files || fread(names.data(), 1, names.size(), f) != names.size()) return false;
        const char* name = names.data();
        for (FileRec& r : recs) {
            if (name >= names.data() + names.size()) return false;
            r.name = name;
            name += strlen(name) + 1;
        }
        return true;
    }

    bool Read(const FileRec& r, std::vector<unsigned char>* out)
    {
        bool compressed = ((hd.flags & 4) != 0) != ((r.size & 0x40000000) != 0);
        DWORD size = r.size & 0x3FFFFFFF;
        std::vector<unsigned char> data(size);
        if (_fseeki64(f, r.offset, SEEK_SET) != 0 || fread(data.data(), 1, size, f) != size) return false;
        size_t skip = 0;
        if (hd.flags & 0x100) skip = 1 + (size ? data[0] : 0);  // the path, embedded before the data
        if (compressed && skip + 4 <= size) {
            DWORD raw = *(DWORD*)(data.data() + skip);
            return ZlibDecompress(data.data() + skip + 4, size - skip - 4, out, raw);
        }
        if (compressed || skip > size) return false;
        out->assign(data.begin() + skip, data.end());
        return true;
    }
};

static bool ReadBsaFile(const std::string& archive, const std::string& wanted, std::vector<unsigned char>* out)
{
    Bsa bsa;
    if (!bsa.Open(archive)) return false;
    for (const Bsa::FileRec& r : bsa.recs)
        if (Lower(r.folder + "\\" + r.name) == wanted) return bsa.Read(r, out);
    return false;
}

// The archives, the ini's list (later ones first) and then the rest of the Data folder's.
static std::vector<std::string> Archives(const std::string& data, const Ini& ini)
{
    std::vector<std::string> archives;
    std::string list = ini.Get("Archive", "SArchiveList", "");
    for (size_t s = 0; s < list.size();) {
        size_t e = list.find(',', s);
        if (e == std::string::npos) e = list.size();
        std::string a = list.substr(s, e - s);
        while (!a.empty() && a[0] == ' ') a.erase(0, 1);
        while (!a.empty() && a.back() == ' ') a.pop_back();
        if (!a.empty()) archives.insert(archives.begin(), a);
        s = e + 1;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((data + "*.bsa").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            bool listed = false;
            for (const std::string& a : archives) listed |= !_stricmp(a.c_str(), fd.cFileName);
            if (!listed) archives.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return archives;
}

static bool ReadLoose(const std::string& path, std::vector<unsigned char>* out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out->resize((size_t)size);
    bool ok = fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

// A file of the game's Data folder: loose, else from the archives.
bool ReadGameFile(const GamePaths& p, const Ini& ini, const char* path, std::vector<unsigned char>* out)
{
    std::string data = p.game + "Data\\";
    if (ReadLoose(data + path, out)) return true;
    std::string wanted = Lower(path);
    for (const std::string& a : Archives(data, ini))
        if (ReadBsaFile(data + a, wanted, out)) return true;
    return false;
}

void ReadGameFolder(const GamePaths& p, const Ini& ini, const char* folder, bool (*want)(const char* name),
                    const std::function<bool(const std::string& name, std::vector<unsigned char>& data)>& got,
                    const std::atomic<bool>* stop)
{
    std::string data = p.game + "Data\\", dir = Lower(folder);
    std::vector<std::string> seen;
    auto fresh = [&](const std::string& name) { return std::find(seen.begin(), seen.end(), name) == seen.end(); };
    std::vector<unsigned char> bytes;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((data + dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = Lower(fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !want(name.c_str()) || !fresh(name)) continue;
            seen.push_back(name);
            if (ReadLoose(data + dir + "\\" + name, &bytes) && !got(name, bytes)) { FindClose(h); return; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    bool textures = dir.compare(0, 9, "textures\\") == 0;
    for (const std::string& a : Archives(data, ini)) {
        if (stop && *stop) return;
        Bsa bsa;
        if (!bsa.Open(data + a, textures ? 2 : 0)) continue;
        for (const Bsa::FileRec& r : bsa.recs) {
            if (r.folder.size() != dir.size() || Lower(r.folder) != dir) continue;
            std::string name = Lower(r.name);
            if (!want(name.c_str()) || !fresh(name)) continue;
            seen.push_back(name);
            if (bsa.Read(r, &bytes) && !got(name, bytes)) return;
        }
    }
}

// ---- the UI font -----------------------------------------------------------------------------

static int Shift(DWORD mask)
{
    int s = 0;
    while (mask && !(mask & 1)) { mask >>= 1; ++s; }
    return s;
}

// The alpha of a DDS texture's first level: DXT1/3/5 or uncompressed with an alpha mask.
static bool DdsAlpha(const std::vector<unsigned char>& d, std::vector<unsigned char>* alpha, int* w, int* h)
{
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4)) return false;
    *h = *(const int*)(d.data() + 12);
    *w = *(const int*)(d.data() + 16);
    DWORD pfFlags = *(const DWORD*)(d.data() + 80), fourcc = *(const DWORD*)(d.data() + 84), bits = *(const DWORD*)(d.data() + 88);
    DWORD aMask = *(const DWORD*)(d.data() + 104);
    if (*w <= 0 || *h <= 0 || *w > 8192 || *h > 8192) return false;
    alpha->assign((size_t)*w * *h, 255);
    const unsigned char* px = d.data() + 128;
    size_t avail = d.size() - 128;
    if (pfFlags & 4) {  // compressed
        int kind = fourcc == MAKEFOURCC('D', 'X', 'T', '1') ? 1 : fourcc == MAKEFOURCC('D', 'X', 'T', '3') ? 3 : fourcc == MAKEFOURCC('D', 'X', 'T', '5') ? 5 : 0;
        if (!kind) return false;
        int bw = (*w + 3) / 4, bh = (*h + 3) / 4, bs = kind == 1 ? 8 : 16;
        if (avail < (size_t)bw * bh * bs) return false;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const unsigned char* b = px + ((size_t)by * bw + bx) * bs;
                unsigned char a[16];
                if (kind == 1) {
                    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
                    DWORD idx = *(const DWORD*)(b + 4);
                    for (int i = 0; i < 16; ++i) a[i] = c0 <= c1 && ((idx >> (2 * i)) & 3) == 3 ? 0 : 255;
                } else if (kind == 3) {
                    for (int i = 0; i < 16; ++i) a[i] = (unsigned char)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                } else {
                    int a0 = b[0], a1 = b[1];
                    int lut[8] = {a0, a1};
                    if (a0 > a1)
                        for (int i = 1; i < 7; ++i) lut[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                    else {
                        for (int i = 1; i < 5; ++i) lut[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                        lut[6] = 0;
                        lut[7] = 255;
                    }
                    unsigned long long idx = 0;
                    for (int i = 0; i < 6; ++i) idx |= (unsigned long long)b[2 + i] << (8 * i);
                    for (int i = 0; i < 16; ++i) a[i] = (unsigned char)lut[(idx >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; ++i) {
                    int x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                    if (x < *w && y < *h) (*alpha)[(size_t)y * *w + x] = a[i];
                }
            }
        return true;
    }
    int bpp = (int)bits / 8;
    if (bpp < 1 || bpp > 4 || !aMask || avail < (size_t)*w * *h * bpp) return false;
    int shift = Shift(aMask);
    DWORD maxv = aMask >> shift;
    for (size_t i = 0, n = (size_t)*w * *h; i < n; ++i) {
        DWORD v = 0;
        memcpy(&v, px + i * bpp, bpp);
        (*alpha)[i] = (unsigned char)(((v & aMask) >> shift) * 255 / maxv);
    }
    return true;
}

bool LoadGameFont(const GamePaths& p, const Ini& ini, std::string* used)
{
    std::string fnt = ini.Get("Fonts", "sFontFile_2", "Textures\\Fonts\\Monofonto_Large.fnt");
    std::vector<unsigned char> fd, dds, alpha;
    if (!ReadGameFile(p, ini, fnt.c_str(), &fd) || fd.size() < 296 + 256 * 56) return false;
    char tex[33] = {};
    memcpy(tex, fd.data() + 8 + 4, 32);  // the first texture's name
    std::string dir = fnt.substr(0, fnt.find_last_of("\\/") + 1);
    int w, h;
    if (!ReadGameFile(p, ini, (dir + tex + ".dds").c_str(), &dds) || !DdsAlpha(dds, &alpha, &w, &h)) return false;
    if (!GameFontSet(fd.data(), (unsigned)fd.size(), alpha.data(), w, h)) return false;
    *used = fnt;
    return true;
}

// ---- launching -------------------------------------------------------------------------------

// What starts the game with xNVSE (as the plugin's restart decides): FalloutNV.exe itself if it
// loads xNVSE (the 4GB patcher's exe names nvse_steam_loader.dll), else nvse_loader.exe.
static std::string Starter(const std::string& game)
{
    std::string exe = game + "FalloutNV.exe";
    FILE* f = fopen(exe.c_str(), "rb");
    if (f) {
        std::vector<char> data;
        _fseeki64(f, 0, SEEK_END);
        data.resize((size_t)_ftelli64(f));
        _fseeki64(f, 0, SEEK_SET);
        size_t got = fread(data.data(), 1, data.size(), f);
        fclose(f);
        static const char kName[] = "nvse_steam_loader.dll";
        for (size_t i = 0; i + sizeof(kName) - 1 <= got; ++i)
            if (data[i] == 'n' && !_strnicmp(&data[i], kName, sizeof(kName) - 1)) return exe;
    }
    if (Exists(game + "nvse_loader.exe")) return game + "nvse_loader.exe";
    return exe;
}

bool LaunchGame(const GamePaths& p, const std::string& save, std::string* error)
{
    // The plugin reads this at its start (restart.cpp) and runs the commands: wait for the main
    // menu, then load the save.
    std::string start = p.game + "Data\\NVSE\\Plugins\\rowe_restart.txt";
    DeleteFileA(start.c_str());
    if (!save.empty()) {
        FILE* f = fopen(start.c_str(), "w");
        if (!f) { *error = "cannot write " + start; return false; }
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        fprintf(f, "time %llu\nwaitmenu 1013 3600\nwait 60\ncon load \"%s\"\n", ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime,
                save.c_str());
        fclose(f);
    }
    std::string exe = Starter(p.game);
    std::string cmd = "\"" + exe + "\"";
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, p.game.c_str(), &si, &pi)) {
        char msg[64];
        wsprintfA(msg, " (error %lu)", GetLastError());
        *error = "cannot start " + exe + msg;
        DeleteFileA(start.c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
