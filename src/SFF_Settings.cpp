#include "SFF_Settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace SFF_Settings {

    static std::string TrimCopy(std::string s) {
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    }

    static std::string StripInlineComment(std::string s) {
        for (const char* token : {";", "#", "//"}) {
            auto p = s.find(token);
            if (p != std::string::npos) s = s.substr(0, p);
        }
        return TrimCopy(s);
    }

    static std::string StripQuotes(std::string s) {
        s = TrimCopy(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);

        std::string out;

        out.reserve(s.size());

        for (char c : s)
            if (c != '"') out.push_back(c);
        return TrimCopy(out);
    }

    static bool ParsePluginFormPair(const std::string& input, std::string& outFile, std::uint32_t& outLocalFormID) {
        auto s = StripQuotes(input);
        if (s.empty()) return false;

        auto sep = s.find('|');
        if (sep == std::string::npos) sep = s.find(':');
        if (sep == std::string::npos) return false;

        auto file = StripQuotes(s.substr(0, sep));
        auto idStr = StripQuotes(s.substr(sep + 1));
        if (file.empty() || idStr.empty()) return false;

        if (idStr.rfind("0x", 0) == 0 || idStr.rfind("0X", 0) == 0) idStr = idStr.substr(2);

        try {
            outLocalFormID = static_cast<std::uint32_t>(std::stoul(idStr, nullptr, 16));
        } catch (...) {
            return false;
        }

        outFile = file;
        return true;
    }

    void ParsePerkListIntoSpecs(const std::string& raw) {
        ++PerkListGeneration;
        PerkSpecCount = 0;
        for (auto& p : PerkSpecs) {
            p.has = false;
            p.file.clear();
            p.localID = 0;
        }

        std::string s = StripQuotes(TrimCopy(raw));
        if (s.empty()) return;

        std::size_t start = 0;
        while (start < s.size() && PerkSpecCount < kMaxPerkSpecs) {
            std::size_t comma = s.find(',', start);
            if (comma == std::string::npos) comma = s.size();

            std::string token = StripQuotes(TrimCopy(s.substr(start, comma - start)));
            if (!token.empty()) {
                std::string file;
                std::uint32_t localID = 0;
                if (ParsePluginFormPair(token, file, localID)) {
                    auto& p = PerkSpecs[PerkSpecCount];
                    p.has = true;
                    p.file = file;
                    p.localID = localID;
                    ++PerkSpecCount;
                }
            }
            start = comma + 1;
        }
    }

    void BuildPerkListBuffer() {
        std::string result;
        for (std::size_t i = 0; i < PerkSpecCount; ++i) {
            const auto& p = PerkSpecs[i];
            if (!p.has) continue;
            if (!result.empty()) result += ",";
            char hex[16];
            std::snprintf(hex, sizeof(hex), "%08X", p.localID);
            result += p.file + "|" + hex;
        }
        const auto n = std::min(result.size(), sizeof(PerkListBuffer) - 1);
        std::memcpy(PerkListBuffer, result.data(), n);
        PerkListBuffer[n] = '\0';
    }

    void Load(bool force) {
        if (Loaded && !force) return;
        Loaded = true;

        MaxExtraFollowers = 3;
        FollowerPerkOption = 0;
        SpeechLevelsPerSlot = 10;
        PerkSpecCount = 0;
        for (auto& p : PerkSpecs) {
            p.has = false;
            p.file.clear();
            p.localID = 0;
        }
        FollowerEssential = false;
        FriendlyFire = false;
        FollowerSandbox = false;
        FollowerCrossfire = false;
        PerkListBuffer[0] = '\0';

        DWORD attrs = GetFileAttributesA(kIniPath);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            BuildPerkListBuffer();
            return;
        }

        int maxVal = GetPrivateProfileIntA("General", "iMaxFollowers", 4, kIniPath);
        maxVal = std::clamp(maxVal, 1, 8);
        MaxExtraFollowers = maxVal - 1;

        int opt = GetPrivateProfileIntA("General", "iFollowerPerkOption", -1, kIniPath);
        if (opt < 0) opt = GetPrivateProfileIntA("General", "bFollowerOptionSelector", 0, kIniPath);
        FollowerPerkOption = std::clamp(opt, 0, 2);

        int sl = GetPrivateProfileIntA("General", "iSpeechLevelsPerSlot", 10, kIniPath);
        SpeechLevelsPerSlot = std::max(sl, 1);

        char buf[2048]{};
        GetPrivateProfileStringA("General", "sPerkForms", "", buf, static_cast<DWORD>(sizeof(buf)), kIniPath);

        std::string perkList = StripInlineComment(buf);

        if (perkList.empty()) {
            char buf2[2048]{};

            GetPrivateProfileStringA("General", "sPerkForm", "", buf2, static_cast<DWORD>(sizeof(buf2)), kIniPath);

            perkList = StripInlineComment(buf2);
        }
        ParsePerkListIntoSpecs(perkList);
        BuildPerkListBuffer();

        FollowerEssential = GetPrivateProfileIntA("General", "bFollowerEssential", 0, kIniPath) != 0;

        FriendlyFire = GetPrivateProfileIntA("General", "bFriendlyFireProtection", 0, kIniPath) != 0;

        FollowerSandbox = GetPrivateProfileIntA("General", "bFollowerSandbox", 0, kIniPath) != 0;

        FollowerCrossfire = GetPrivateProfileIntA("General", "bFollowerCrossfireProtection", 0, kIniPath) != 0;
    }

    void Save() {
        auto writeInt = [](const char* key, int val) {
            WritePrivateProfileStringA("General", key, std::to_string(val).c_str(), kIniPath);
        };

        auto writeStr = [](const char* key, const char* val) {
            WritePrivateProfileStringA("General", key, val, kIniPath);
        };

        writeInt("iMaxFollowers", MaxExtraFollowers + 1);
        writeInt("bFollowerOptionSelector", FollowerPerkOption);
        writeInt("iSpeechLevelsPerSlot", SpeechLevelsPerSlot);
        writeInt("bFollowerEssential", FollowerEssential ? 1 : 0);
        writeInt("bFriendlyFireProtection", FriendlyFire ? 1 : 0);
        writeInt("bFollowerSandbox", FollowerSandbox ? 1 : 0);
        writeInt("bFollowerCrossfireProtection", FollowerCrossfire ? 1 : 0);
        ParsePerkListIntoSpecs(PerkListBuffer);
        BuildPerkListBuffer();
        writeStr("sPerkForms", PerkListBuffer);
        WritePrivateProfileStringA("General", "sPerkForm", nullptr, kIniPath);
    }
}
