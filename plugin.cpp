#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {
    void LoadSettings();
    void Install();
    void OnMessage(SKSE::MessagingInterface::Message* msg);
    void ApplyFollowerDialogueGate();

    void LoadEssentialSetting();
    void RefreshEssentialLoadedActors();
    void ScheduleEssentialRecheckBurst();
    void UpdateEssentialForActor(RE::Actor* a);

    RE::TESGlobal* g_playerFollowerCount = nullptr;
    RE::TESGlobal* g_sffCanRecruitMore = nullptr;
    RE::TESGlobal* g_sffCurrentFollowerCount = nullptr;

    std::int32_t g_maxExtraFollowers = 2;

    // Follower Perk Option Selector
    // 0 = Option 1: No perks, use iMaxFollowers
    // 1 = Option 2: +1 follower per owned perk in sPerkForms (iMaxFollowers ignored)
    std::int32_t g_followerPerkOption = 0;

    struct PerkSpec {
        bool has = false;
        std::string file;
        std::uint32_t localID = 0;
    };

    static constexpr std::size_t kMaxPerkSpecs = 8;
    std::array<PerkSpec, kMaxPerkSpecs> g_perkSpecs{};
    std::size_t g_perkSpecCount = 0;

    static constexpr const char* kRequiredPluginName = "Simple Follower Framework.esp";

    bool g_bFollowerEssential = false;
    std::unordered_map<RE::FormID, std::uint8_t> g_essOrig{};

    bool IsValidActor(RE::Actor* a) { return a && a != RE::PlayerCharacter::GetSingleton() && !a->IsDead(); }

    std::string TrimCopy(std::string s) {
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };

        while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) {
            s.erase(s.begin());
        }
        while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) {
            s.pop_back();
        }
        return s;
    }

    std::string StripInlineComment(std::string s) {
        auto pSemi = s.find(';');
        if (pSemi != std::string::npos) {
            s = s.substr(0, pSemi);
        }

        auto pHash = s.find('#');
        if (pHash != std::string::npos) {
            s = s.substr(0, pHash);
        }

        auto pSlash = s.find("//");
        if (pSlash != std::string::npos) {
            s = s.substr(0, pSlash);
        }

        return TrimCopy(s);
    }

    std::string StripQuotes(std::string s) {
        s = TrimCopy(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            s = s.substr(1, s.size() - 2);
        }
        s = TrimCopy(s);

        if (s.find('"') != std::string::npos) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c != '"') {
                    out.push_back(c);
                }
            }
            s = TrimCopy(out);
        }

        return s;
    }

    bool ParsePluginFormPair(const std::string& input, std::string& outFile, std::uint32_t& outLocalFormID) {
        auto s = StripQuotes(input);
        if (s.empty()) {
            return false;
        }

        auto sep = s.find('|');
        if (sep == std::string::npos) {
            sep = s.find(':');
        }
        if (sep == std::string::npos) {
            return false;
        }

        auto file = StripQuotes(s.substr(0, sep));
        auto idStr = StripQuotes(s.substr(sep + 1));

        if (file.empty() || idStr.empty()) {
            return false;
        }

        if (idStr.rfind("0x", 0) == 0 || idStr.rfind("0X", 0) == 0) {
            idStr = idStr.substr(2);
        }

        try {
            outLocalFormID = static_cast<std::uint32_t>(std::stoul(idStr, nullptr, 16));
        } catch (...) {
            return false;
        }

        outFile = file;
        return true;
    }

    bool FileExistsA(const char* path) {
        DWORD attrs = GetFileAttributesA(path);
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    [[noreturn]] void MessageAndExit(const char* msg) {
        MessageBoxA(nullptr, msg, "SimpleFollowerFramework.dll", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        ExitProcess(1);
    }

bool IsRequiredPluginLoaded() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return false;
        }

        if (dh->LookupLoadedModByName("Simple Follower Framework.esp")) {
            return true;
        }

        // check if EditorIDs exist
        if (RE::TESForm::LookupByEditorID("SFF_CurrentFollowerCount")) {
            return true;
        }

        return false;
    }

    bool PluginsTxtExplicitlyDisablesRequiredPlugin() {
        char localAppData[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, static_cast<DWORD>(sizeof(localAppData)));
        if (n == 0 || n >= sizeof(localAppData)) {
            return false;
        }

        const char* dirs[] = {"Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim VR", "Skyrim"};

        for (auto* d : dirs) {
            std::string path = std::string(localAppData) + "\\" + d + "\\plugins.txt";
            if (!FileExistsA(path.c_str())) {
                continue;
            }

            std::ifstream in(path);
            if (!in.is_open()) {
                continue;
            }

            std::string line;
            while (std::getline(in, line)) {
                line = StripInlineComment(line);
                if (line.empty()) {
                    continue;
                }

                bool enabled = false;
                if (!line.empty() && line.front() == '*') {
                    enabled = true;
                    line.erase(line.begin());
                    line = TrimCopy(line);
                }

                if (line.size() == std::strlen(kRequiredPluginName)) {
                    bool same = true;
                    for (std::size_t i = 0; i < line.size(); ++i) {
                        unsigned char a = static_cast<unsigned char>(line[i]);
                        unsigned char b = static_cast<unsigned char>(kRequiredPluginName[i]);
                        if (std::tolower(a) != std::tolower(b)) {
                            same = false;
                            break;
                        }
                    }

                    if (same) {
                        return !enabled;  // only early-fail if we see it explicitly disabled
                    }
                }
            }
        }

        return false;
    }

    void EarlyPreflightCheck() {
        std::string espPath = std::string("Data\\") + kRequiredPluginName;

        if (!FileExistsA(espPath.c_str())) {
            MessageAndExit(
                "Missing required file:\n\n"
                "Data\\Simple Follower Framework.esp\n"
                "Install it (or fix your mod manager / VFS), then relaunch.");
        }

        if (PluginsTxtExplicitlyDisablesRequiredPlugin()) {
            MessageAndExit(
                "Required plugin is disabled:\n"
                "Simple Follower Framework.esp\n"
                "Enable it in your load order, then relaunch.");
        }
    }

    void ClearPerkSpecs() {
        g_perkSpecCount = 0;
        for (auto& p : g_perkSpecs) {
            p.has = false;
            p.file.clear();
            p.localID = 0;
        }
    }

    void AddPerkSpecIfValid(const std::string& spec) {
        if (g_perkSpecCount >= kMaxPerkSpecs) {
            return;
        }

        std::string file;
        std::uint32_t localID = 0;
        if (!ParsePluginFormPair(spec, file, localID)) {
            return;
        }

        auto& p = g_perkSpecs[g_perkSpecCount];
        p.has = true;
        p.file = file;
        p.localID = localID;
        ++g_perkSpecCount;
    }

    void ParsePerkFormsList(const std::string& input) {
        ClearPerkSpecs();

        std::string s = StripQuotes(TrimCopy(input));
        if (s.empty()) {
            return;
        }

        std::size_t start = 0;
        while (start < s.size() && g_perkSpecCount < kMaxPerkSpecs) {
            std::size_t comma = s.find(',', start);
            if (comma == std::string::npos) {
                comma = s.size();
            }

            std::string token = StripQuotes(TrimCopy(s.substr(start, comma - start)));
            if (!token.empty()) {
                AddPerkSpecIfValid(token);
            }

            start = comma + 1;
        }
    }

    bool HasPerkFromSpec(const std::string& file, std::uint32_t localID) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return false;
        }

        auto* form = dh->LookupForm(localID, file);
        auto* perk = form ? form->As<RE::BGSPerk>() : nullptr;
        if (!perk) {
            return false;
        }

        return player->HasPerk(perk);
    }

    std::int32_t CountOwnedPerksFromList() {
        std::int32_t owned = 0;
        for (std::size_t i = 0; i < g_perkSpecCount; ++i) {
            const auto& p = g_perkSpecs[i];
            if (!p.has) {
                continue;
            }
            if (HasPerkFromSpec(p.file, p.localID)) {
                ++owned;
            }
        }
        return owned;
    }

    RE::TESFaction* GetFaction(std::string_view editorID) {
        auto* form = RE::TESForm::LookupByEditorID(editorID);
        return form ? form->As<RE::TESFaction>() : nullptr;
    }

    RE::TESGlobal* GetFollowerCountGlobal() {
        if (!g_playerFollowerCount) {
            auto* form = RE::TESForm::LookupByEditorID("PlayerFollowerCount");
            if (form) {
                g_playerFollowerCount = form->As<RE::TESGlobal>();
            }
        }

        return g_playerFollowerCount;
    }

    RE::TESGlobal* GetSFFCanRecruitMoreGlobal() {
        if (!g_sffCanRecruitMore) {
            auto* form = RE::TESForm::LookupByEditorID("SFF_CanRecruitMore");
            if (form) {
                g_sffCanRecruitMore = form->As<RE::TESGlobal>();
            }
        }

        return g_sffCanRecruitMore;
    }

    RE::TESGlobal* GetSFFCurrentFollowerCountGlobal() {
        if (!g_sffCurrentFollowerCount) {
            auto* form = RE::TESForm::LookupByEditorID("SFF_CurrentFollowerCount");
            if (form) {
                g_sffCurrentFollowerCount = form->As<RE::TESGlobal>();
            }
        }

        return g_sffCurrentFollowerCount;
    }

    void LoadSettings() {
        const char* path = "Data\\SKSE\\Plugins\\SimpleFollowerFramework.ini";

        // hard defaults so mod still runs if INI is missing and dont shit its pants and crash
        g_maxExtraFollowers = 2;
        g_followerPerkOption = 0;
        ClearPerkSpecs();

        DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return;
        }

        int maxVal = GetPrivateProfileIntA("General", "iMaxFollowers", 3, path);

        if (maxVal < 1) {
            maxVal = 1;
        }

        if (maxVal > 8) {
            maxVal = 8;
        }

        g_maxExtraFollowers = maxVal - 1;

        int opt = GetPrivateProfileIntA("General", "iFollowerPerkOption", -1, path);

        if (opt < 0) {
            // fallback for older ini that still uses bFollowerOptionSelector
            opt = GetPrivateProfileIntA("General", "bFollowerOptionSelector", 0, path) != 0 ? 1 : 0;
        }

        if (opt < 0) {
            opt = 0;
        }

        if (opt > 1) {
            opt = 1;
        }

        g_followerPerkOption = opt;

        char buf[2048]{};
        GetPrivateProfileStringA("General", "sPerkForms", "", buf, static_cast<DWORD>(sizeof(buf)), path);
        std::string perkList = StripInlineComment(buf);

        if (perkList.empty()) {
            // fallback for older ini that uses sPerkForm
            char buf1[512]{};
            GetPrivateProfileStringA("General", "sPerkForm", "", buf1, static_cast<DWORD>(sizeof(buf1)), path);
            perkList = StripInlineComment(buf1);
        }

        ParsePerkFormsList(perkList);
    }

    std::int32_t GetTotalFollowerCapFromSettings() {
        LoadSettings();

        constexpr std::int32_t kBaseFollowerSlot = 1;
        constexpr std::int32_t kMaxExtraAliases = 7;
        constexpr std::int32_t kMaxTotalFollowers = kBaseFollowerSlot + kMaxExtraAliases;

        std::int32_t extrasAllowed = g_maxExtraFollowers;

        if (extrasAllowed < 0) {
            extrasAllowed = 0;
        }

        if (extrasAllowed > kMaxExtraAliases) {
            extrasAllowed = kMaxExtraAliases;
        }

        std::int32_t totalFromMaxFollowers = kBaseFollowerSlot + extrasAllowed;

        if (totalFromMaxFollowers < 1) {
            totalFromMaxFollowers = 1;
        }

        if (totalFromMaxFollowers > kMaxTotalFollowers) {
            totalFromMaxFollowers = kMaxTotalFollowers;
        }

        if (g_followerPerkOption == 0) {
            return totalFromMaxFollowers;
        }

        // Option 2: iMaxFollowers is useless here, cap is purely perk-based
        std::int32_t total = kBaseFollowerSlot + CountOwnedPerksFromList();

        if (total < 1) {
            total = 1;
        }
        if (total > kMaxTotalFollowers) {
            total = kMaxTotalFollowers;
        }

        return total;
    }

    void ApplyFollowerDialogueGate() {
        auto* followerCount = GetFollowerCountGlobal();
        if (!followerCount) {
            return;
        }

        const auto totalCap = GetTotalFollowerCapFromSettings();

        int currentCount = 0;
        if (auto* currentCountGlobal = GetSFFCurrentFollowerCountGlobal()) {
            currentCount = static_cast<int>(currentCountGlobal->value);
            if (currentCount < 0) {
                currentCount = 0;
            }
        }

        const bool canRecruitMore = currentCount < totalCap;

        // vanilla hire dialogue reads PlayerFollowerCount, so i fake it here
        followerCount->value = canRecruitMore ? 0.0f : 1.0f;

        if (auto* canRecruit = GetSFFCanRecruitMoreGlobal()) {
            canRecruit->value = canRecruitMore ? 1.0f : 0.0f;
        }
    }

    void LoadEssentialSetting() {
        const char* path = "Data\\SKSE\\Plugins\\SimpleFollowerFramework.ini";
        DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            g_bFollowerEssential = false;
            return;
        }
        g_bFollowerEssential = GetPrivateProfileIntA("General", "bFollowerEssential", 0, path) != 0;
    }

    bool IsInServiceEssential(RE::Actor* a) {
        if (!a) {
            return false;
        }
        auto* current = GetFaction("CurrentFollowerFaction");
        if (!current) {
            return false;
        }
        return a->IsInFaction(current) && a->IsPlayerTeammate();
    }

    void UpdateEssentialForActor(RE::Actor* a) {
        if (!a) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && a == player) {
            return;
        }

        auto* base = a->GetActorBase();
        if (!base) {
            return;
        }

        const auto id = base->GetFormID();
        const bool want = g_bFollowerEssential && IsInServiceEssential(a);

        auto it = g_essOrig.find(id);

        if (want) {
            if (it == g_essOrig.end()) {
                const bool e = base->actorData.actorBaseFlags.any(RE::ACTOR_BASE_DATA::Flag::kEssential);
                const bool p = base->actorData.actorBaseFlags.any(RE::ACTOR_BASE_DATA::Flag::kProtected);
                std::uint8_t bits = 0;
                if (e) {
                    bits |= 1;
                }
                if (p) {
                    bits |= 2;
                }
                g_essOrig.emplace(id, bits);
            }

            base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
            base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
            return;
        }

        if (it != g_essOrig.end()) {
            const std::uint8_t bits = it->second;

            if (bits & 1) {
                base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
            } else {
                base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kEssential);
            }

            if (bits & 2) {
                base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
            } else {
                base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
            }

            g_essOrig.erase(it);
        }
    }

    void VisitActorHandleEssential(RE::ActorHandle h) {
        auto p = RE::Actor::LookupByHandle(h.native_handle());
        if (p) {
            UpdateEssentialForActor(p.get());
        }
    }

    void RefreshEssentialLoadedActors() {
        LoadEssentialSetting();

        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) {
            return;
        }

        for (auto& h : pl->highActorHandles) {
            VisitActorHandleEssential(h);
        }
        for (auto& h : pl->lowActorHandles) {
            VisitActorHandleEssential(h);
        }
        for (auto& h : pl->middleHighActorHandles) {
            VisitActorHandleEssential(h);
        }
        for (auto& h : pl->middleLowActorHandles) {
            VisitActorHandleEssential(h);
        }
    }

    void ScheduleEssentialRecheckBurst() {
        std::thread([]() {
            const int marks[] = {50, 200, 500, 1000, 2000};
            int prev = 0;

            for (int m : marks) {
                ::Sleep(static_cast<DWORD>(m - prev));
                prev = m;

                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() { RefreshEssentialLoadedActors(); });
                }
            }
        }).detach();
    }

    bool AddVanillaFollower(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!IsValidActor(a)) {
            return false;
        }

        auto* potential = GetFaction("PotentialFollowerFaction");
        auto* current = GetFaction("CurrentFollowerFaction");
        if (!potential || !current) {
            return false;
        }

        if (!a->IsInFaction(potential)) {
            return false;
        }

        if (!a->IsInFaction(current)) {
            a->AddToFaction(current, 0);
        }

        a->EvaluatePackage();

        LoadEssentialSetting();
        UpdateEssentialForActor(a);

        return true;
    }

    bool IsVanillaFollower(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) {
            return false;
        }

        auto* current = GetFaction("CurrentFollowerFaction");
        return current && a->IsInFaction(current);
    }

    std::int32_t GetMaxFollowers(RE::StaticFunctionTag*) {
        // this returns TOTAL cap (like how i like to clap your mother wut wut) (base 1 + extras)
        return GetTotalFollowerCapFromSettings();
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("AddVanillaFollower", "SFF_SKSE", AddVanillaFollower);
        vm->RegisterFunction("IsVanillaFollower", "SFF_SKSE", IsVanillaFollower);
        vm->RegisterFunction("GetMaxFollowers", "SFF_SKSE", GetMaxFollowers);
        return true;
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() {
            static MenuSink s;
            return std::addressof(s);
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (e && e->menuName == "Dialogue Menu") {
                if (e->opening) {
                    // vanilla checks happen around dialogue open, so i update here
                    ApplyFollowerDialogueGate();
                    RefreshEssentialLoadedActors();
                } else {
                    ScheduleEssentialRecheckBurst();
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent> {
    public:
        static ActivateSink* GetSingleton() {
            static ActivateSink s;
            return std::addressof(s);
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* e,
                                              RE::BSTEventSource<RE::TESActivateEvent>*) override {
            if (!e) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* activatorRef = e->actionRef.get();
            auto* activatedRef = e->objectActivated.get();

            if (!player || !activatorRef || !activatedRef) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (activatorRef != player) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto* actor = activatedRef->As<RE::Actor>();
            if (!actor || actor == player) {
                return RE::BSEventNotifyControl::kContinue;
            }

            // fixes the talk stale check before the dialogue menu opens
            ApplyFollowerDialogueGate();

            LoadEssentialSetting();
            UpdateEssentialForActor(actor);

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Install() {
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        }

        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESActivateEvent>(ActivateSink::GetSingleton());
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg) {
        if (!msg) {
            return;
        }

        // Prevent you from loading the mod without the .esp file enabled.
        if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
            if (!IsRequiredPluginLoaded()) {
                MessageAndExit(
                    "Missing required plugin in load order:\n"
                    "Simple Follower Framework.esp\n"
                    "Enable it in your load order, then relaunch.");
            }

            ApplyFollowerDialogueGate();
            Install();
            RefreshEssentialLoadedActors();
        } else if (msg->type == SKSE::MessagingInterface::kPostLoadGame ||
                   msg->type == SKSE::MessagingInterface::kNewGame) {
            ApplyFollowerDialogueGate();
            RefreshEssentialLoadedActors();
        }
    }
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    EarlyPreflightCheck();

    SKSE::Init(skse);

    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(RegisterPapyrus);
    }

    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }

    return true;
}