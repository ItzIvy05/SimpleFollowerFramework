#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>

#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {
    void LoadSettings();
    void Install();
    void OnMessage(SKSE::MessagingInterface::Message* msg);
    void ApplyFollowerDialogueGate();

    RE::TESGlobal* g_playerFollowerCount = nullptr;
    RE::TESGlobal* g_sffCanRecruitMore = nullptr;
    RE::TESGlobal* g_sffCurrentFollowerCount = nullptr;

    std::int32_t g_maxExtraFollowers = 3;
    bool g_usePerkLock = false;

    bool g_hasPerkSpec = false;
    std::string g_perkFile;
    std::uint32_t g_perkLocalFormID = 0;

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

    bool ParsePluginFormPair(const std::string& input, std::string& outFile, std::uint32_t& outLocalFormID) {
        auto s = TrimCopy(input);
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

        auto file = TrimCopy(s.substr(0, sep));
        auto idStr = TrimCopy(s.substr(sep + 1));

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
        g_maxExtraFollowers = 3;
        g_usePerkLock = false;
        g_hasPerkSpec = false;
        g_perkFile.clear();
        g_perkLocalFormID = 0;

        DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return;
        }

        int maxVal = GetPrivateProfileIntA("General", "iMaxFollowers", 3, path);

        if (maxVal < 0) {
            maxVal = 0;
        }

        if (maxVal > 7) {
            maxVal = 7;
        }

        g_maxExtraFollowers = maxVal;

        g_usePerkLock = GetPrivateProfileIntA("General", "bUsePerkLock", 0, path) != 0;

        char perkBuf[512]{};
        GetPrivateProfileStringA("General", "sPerkForm", "", perkBuf, static_cast<DWORD>(sizeof(perkBuf)), path);

        std::string perkSpec = StripInlineComment(perkBuf);
        if (!perkSpec.empty()) {
            std::string file;
            std::uint32_t localID = 0;
            if (ParsePluginFormPair(perkSpec, file, localID)) {

                g_perkFile = file;
                g_perkLocalFormID = localID;
                g_hasPerkSpec = true;
            }
        }
    }

    bool PlayerHasRequiredPerk() {
        if (!g_usePerkLock) {
            return true;
        }

        // if perk lock is on but perk is not set, it treat it as locked
        if (!g_hasPerkSpec) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return false;
        }

        auto* form = dh->LookupForm(g_perkLocalFormID, g_perkFile);
        auto* perk = form ? form->As<RE::BGSPerk>() : nullptr;
        if (!perk) {
            return false;
        }

        return player->HasPerk(perk);
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

        // perk lock only gates extras, never the base vanilla slot
        if (g_usePerkLock && !PlayerHasRequiredPerk()) {
            return kBaseFollowerSlot;
        }

        std::int32_t total = kBaseFollowerSlot + extrasAllowed;

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

        // this returns TOTAL cap (like i like to clap your mother wut wut) (base 1 + extras)
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
            if (e && e->opening && e->menuName == "Dialogue Menu") {

                // vanilla checks happen around dialogue open, so i update here
                ApplyFollowerDialogueGate();

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

        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* e, RE::BSTEventSource<RE::TESActivateEvent>*) override {

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

        if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
            ApplyFollowerDialogueGate();
            Install();
        } else if (msg->type == SKSE::MessagingInterface::kPostLoadGame ||
                   msg->type == SKSE::MessagingInterface::kNewGame) {
            ApplyFollowerDialogueGate();
        }
    }
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);

    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(RegisterPapyrus);
    }

    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }

    return true;
}