#pragma once
#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

namespace SFF_Settings {

    constexpr std::size_t kMaxPerkSpecs = 8;
    constexpr const char* kIniPath = R"(Data\SKSE\Plugins\SimpleFollowerFramework.ini)";

    struct PerkSpec {
        bool has = false;
        std::string file;
        std::uint32_t localID = 0;
    };

    inline std::int32_t MaxExtraFollowers = 3;
    inline std::int32_t FollowerPerkOption = 0;
    inline std::int32_t SpeechLevelsPerSlot = 10;
    inline std::array<PerkSpec, kMaxPerkSpecs> PerkSpecs{};
    inline std::size_t PerkSpecCount = 0;
    inline bool FollowerEssential = false;
    inline bool FriendlyFire = false;
    inline bool FollowerSandbox = false;
    inline char PerkListBuffer[2048]{};
    inline bool Loaded = false;
    inline void (*ApplyGateCallback)() = nullptr;
    inline void (*FriendlyFireCallback)() = nullptr;
    inline void (*SandboxCallback)() = nullptr;

    void Load(bool force = false);
    void Save();
    void ParsePerkListIntoSpecs(const std::string& raw);
    void BuildPerkListBuffer();
}
