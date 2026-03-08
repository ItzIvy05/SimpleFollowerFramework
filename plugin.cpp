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
#include <unordered_map>

#include "SFF_Settings.h"
#include "SFF_UI.h"

namespace {
    void Install();
    void OnMessage(SKSE::MessagingInterface::Message* msg);
    void ApplyFollowerDialogueGate();

    void UpdateEssentialForActor(RE::Actor* a);

    RE::TESGlobal* g_playerFollowerCount = nullptr;
    RE::TESGlobal* g_sffCanRecruitMore = nullptr;
    RE::TESGlobal* g_sffCurrentFollowerCount = nullptr;

    std::unordered_map<RE::FormID, std::uint8_t> g_essOrig{};

    RE::SpellItem* g_friendlyFireSpell = nullptr;

    RE::SpellItem* GetFriendlyFireSpell() {
        if (!g_friendlyFireSpell) {
            auto* form = RE::TESForm::LookupByEditorID("IvyCompanionsSafeSpell");
            if (form) g_friendlyFireSpell = form->As<RE::SpellItem>();
        }
        return g_friendlyFireSpell;
    }

    void ApplyFriendlyFire() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* spell = GetFriendlyFireSpell();
        if (!player || !spell) return;

        const bool has = player->HasSpell(spell);
        if (SFF_Settings::FriendlyFire && !has)
            player->AddSpell(spell);
        else if (!SFF_Settings::FriendlyFire && has)
            player->RemoveSpell(spell);
    }

    static constexpr const char* kRequiredPluginName = "Simple Follower Framework.esp";

    bool IsValidActor(RE::Actor* a) { return a && a != RE::PlayerCharacter::GetSingleton() && !a->IsDead(); }

    // File / plugin checks

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
        if (!dh) return false;
        if (dh->LookupLoadedModByName("Simple Follower Framework.esp")) return true;
        if (RE::TESForm::LookupByEditorID("SFF_CurrentFollowerCount")) return true;
        return false;
    }

    bool PluginsTxtExplicitlyDisablesRequiredPlugin() {
        char localAppData[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, static_cast<DWORD>(sizeof(localAppData)));

        if (n == 0 || n >= sizeof(localAppData)) return false;

        const char* dirs[] = {"Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim VR", "Skyrim"};

        for (auto* d : dirs) {
            std::string path = std::string(localAppData) + "\\" + d + "\\plugins.txt";
            if (!FileExistsA(path.c_str())) continue;

            std::ifstream in(path);
            if (!in.is_open()) continue;

            std::string line;
            while (std::getline(in, line)) {
                // Strip inline comments
                for (const char* tok : {";", "#", "//"}) {
                    auto p = line.find(tok);
                    if (p != std::string::npos) line = line.substr(0, p);
                }
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                if (line.empty()) continue;

                bool enabled = (!line.empty() && line.front() == '*');
                if (enabled) {
                    line.erase(line.begin());
                }
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
                    line.erase(line.begin());

                if (line.size() == std::strlen(kRequiredPluginName)) {
                    bool same = true;
                    for (std::size_t i = 0; i < line.size(); ++i) {
                        if (std::tolower(static_cast<unsigned char>(line[i])) !=
                            std::tolower(static_cast<unsigned char>(kRequiredPluginName[i]))) {
                            same = false;
                            break;
                        }
                    }
                    if (same) return !enabled;
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

    // ── Faction / global lookups

    RE::TESFaction* GetFaction(std::string_view editorID) {
        auto* form = RE::TESForm::LookupByEditorID(editorID);
        return form ? form->As<RE::TESFaction>() : nullptr;
    }

    RE::TESGlobal* GetFollowerCountGlobal() {
        if (!g_playerFollowerCount) {
            auto* form = RE::TESForm::LookupByEditorID("PlayerFollowerCount");
            if (form) g_playerFollowerCount = form->As<RE::TESGlobal>();
        }
        return g_playerFollowerCount;
    }

    RE::TESGlobal* GetSFFCanRecruitMoreGlobal() {
        if (!g_sffCanRecruitMore) {
            auto* form = RE::TESForm::LookupByEditorID("SFF_CanRecruitMore");
            if (form) g_sffCanRecruitMore = form->As<RE::TESGlobal>();
        }
        return g_sffCanRecruitMore;
    }

    RE::TESGlobal* GetSFFCurrentFollowerCountGlobal() {
        if (!g_sffCurrentFollowerCount) {
            auto* form = RE::TESForm::LookupByEditorID("SFF_CurrentFollowerCount");
            if (form) g_sffCurrentFollowerCount = form->As<RE::TESGlobal>();
        }
        return g_sffCurrentFollowerCount;
    }

    // Perk checking

    bool HasPerkFromSpec(const std::string& file, std::uint32_t localID) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return false;
        auto* form = dh->LookupForm(localID, file);
        auto* perk = form ? form->As<RE::BGSPerk>() : nullptr;
        return perk && player->HasPerk(perk);
    }

    std::int32_t CountOwnedPerksFromList() {
        std::int32_t owned = 0;
        for (std::size_t i = 0; i < SFF_Settings::PerkSpecCount; ++i) {
            const auto& p = SFF_Settings::PerkSpecs[i];
            if (p.has && HasPerkFromSpec(p.file, p.localID)) ++owned;
        }
        return owned;
    }

    // Speech-based cap (Option 2)

    std::int32_t GetSpeechBasedFollowerCap() {
        constexpr std::int32_t kBase = 1, kMaxExtras = 7;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return kBase;

        const auto speech =
            static_cast<std::int32_t>(player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech));
        const std::int32_t levelsPerSlot =
            (SFF_Settings::SpeechLevelsPerSlot > 0) ? SFF_Settings::SpeechLevelsPerSlot : 10;
        std::int32_t total = kBase + (speech / levelsPerSlot);
        return std::clamp(total, 1, kBase + kMaxExtras);
    }

    std::int32_t GetTotalFollowerCapFromSettings() {
        constexpr std::int32_t kBase = 1, kMaxExtras = 7;

        if (SFF_Settings::FollowerPerkOption == 0) {
            int extras = std::clamp(static_cast<int>(SFF_Settings::MaxExtraFollowers), 0, kMaxExtras);
            return std::clamp(kBase + extras, 1, kBase + kMaxExtras);
        }
        if (SFF_Settings::FollowerPerkOption == 1) {
            return std::clamp(kBase + CountOwnedPerksFromList(), 1, kBase + kMaxExtras);
        }
        // Option 2 — speech-scaled (always live)
        return GetSpeechBasedFollowerCap();
    }

    // Dialogue gate

    void ApplyFollowerDialogueGate() {
        auto* followerCount = GetFollowerCountGlobal();
        if (!followerCount) return;

        const auto totalCap = GetTotalFollowerCapFromSettings();
        int currentCount = 0;
        if (auto* ccg = GetSFFCurrentFollowerCountGlobal()) {
            currentCount = std::max(static_cast<int>(ccg->value), 0);
        }

        const bool canRecruitMore = (currentCount < totalCap);

        // Vanilla hire dialogue reads PlayerFollowerCount — fake it here
        followerCount->value = canRecruitMore ? 0.0f : 1.0f;

        if (auto* canRecruit = GetSFFCanRecruitMoreGlobal()) {
            canRecruit->value = canRecruitMore ? 1.0f : 0.0f;
        }
    }

    // Essential flag management

    bool IsInServiceEssential(RE::Actor* a) {
        if (!a) return false;
        auto* current = GetFaction("CurrentFollowerFaction");
        return current && a->IsInFaction(current) && a->IsPlayerTeammate();
    }

    void UpdateEssentialForActor(RE::Actor* a) {
        if (!a) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && a == player) return;
        auto* base = a->GetActorBase();
        if (!base) return;

        const auto id = base->GetFormID();
        const bool want = SFF_Settings::FollowerEssential && IsInServiceEssential(a);
        auto it = g_essOrig.find(id);

        if (want) {
            if (it == g_essOrig.end()) {
                const bool e = base->actorData.actorBaseFlags.any(RE::ACTOR_BASE_DATA::Flag::kEssential);
                const bool p = base->actorData.actorBaseFlags.any(RE::ACTOR_BASE_DATA::Flag::kProtected);

                std::uint8_t bits = 0;
                if (e) bits |= 1;
                if (p) bits |= 2;
                g_essOrig.emplace(id, bits);
            }
            base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
            base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
            return;
        }

        if (it != g_essOrig.end()) {
            const std::uint8_t bits = it->second;

            if (bits & 1)
                base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
            else
                base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kEssential);
            if (bits & 2)
                base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
            else
                base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);

            g_essOrig.erase(it);
        }
    }

    // Papyrus-exported functions

    bool ApplyFollowerEssential(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!IsValidActor(a)) return false;
        SFF_Settings::Load();  // cache check
        UpdateEssentialForActor(a);
        return true;
    }

    bool RestoreFollowerEssential(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return false;
        auto* base = a->GetActorBase();
        if (!base) return false;

        const auto id = base->GetFormID();
        auto it = g_essOrig.find(id);
        if (it == g_essOrig.end()) return false;

        const std::uint8_t bits = it->second;
        if (bits & 1)
            base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
        else
            base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kEssential);
        if (bits & 2)
            base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
        else
            base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
        g_essOrig.erase(it);
        return true;
    }

    bool AddVanillaFollower(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!IsValidActor(a)) return false;
        auto* potential = GetFaction("PotentialFollowerFaction");
        auto* current = GetFaction("CurrentFollowerFaction");
        if (!potential || !current) return false;
        if (!a->IsInFaction(potential)) return false;
        if (!a->IsInFaction(current)) a->AddToFaction(current, 0);
        a->EvaluatePackage();
        SFF_Settings::Load();
        UpdateEssentialForActor(a);
        return true;
    }

    bool IsVanillaFollower(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return false;
        auto* current = GetFaction("CurrentFollowerFaction");
        return current && a->IsInFaction(current);
    }

    std::int32_t GetMaxFollowers(RE::StaticFunctionTag*) { return GetTotalFollowerCapFromSettings(); }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("AddVanillaFollower", "SFF_SKSE", AddVanillaFollower);
        vm->RegisterFunction("IsVanillaFollower", "SFF_SKSE", IsVanillaFollower);
        vm->RegisterFunction("GetMaxFollowers", "SFF_SKSE", GetMaxFollowers);
        vm->RegisterFunction("ApplyFollowerEssential", "SFF_SKSE", ApplyFollowerEssential);
        vm->RegisterFunction("RestoreFollowerEssential", "SFF_SKSE", RestoreFollowerEssential);
        return true;
    }

    // Event sinks

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() {
            static MenuSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (e && e->menuName == "Dialogue Menu" && e->opening) ApplyFollowerDialogueGate();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent> {
    public:
        static ActivateSink* GetSingleton() {
            static ActivateSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* e,
                                              RE::BSTEventSource<RE::TESActivateEvent>*) override {
            if (!e) return RE::BSEventNotifyControl::kContinue;
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* activator = e->actionRef.get();
            auto* activated = e->objectActivated.get();
            if (!player || !activator || !activated) return RE::BSEventNotifyControl::kContinue;
            if (activator != player) return RE::BSEventNotifyControl::kContinue;
            auto* actor = activated->As<RE::Actor>();
            if (!actor || actor == player) return RE::BSEventNotifyControl::kContinue;
            ApplyFollowerDialogueGate();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Install() {
        if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton())
            events->AddEventSink<RE::TESActivateEvent>(ActivateSink::GetSingleton());
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg) {
        if (!msg) return;

        if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
            if (!IsRequiredPluginLoaded()) {
                MessageAndExit(
                    "Missing required plugin in load order:\n"
                    "Simple Follower Framework.esp\n"
                    "Enable it in your load order, then relaunch.");
            }
            ApplyFollowerDialogueGate();
            Install();
        } else if (msg->type == SKSE::MessagingInterface::kPostLoadGame ||
                   msg->type == SKSE::MessagingInterface::kNewGame) {
            SFF_Settings::Load(true);
            ApplyFollowerDialogueGate();
            ApplyFriendlyFire();
        }
    }

}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    EarlyPreflightCheck();

    SKSE::Init(skse);

    // Read INI immediately so settings are ready before anything else runs
    SFF_Settings::Load();

    // Give the UI a way to poke the dialogue gate after live changes
    SFF_Settings::ApplyGateCallback = []() { ApplyFollowerDialogueGate(); };
    SFF_Settings::FriendlyFireCallback = []() { ApplyFriendlyFire(); };

    if (auto* papyrus = SKSE::GetPapyrusInterface()) papyrus->Register(RegisterPapyrus);

    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);

    // Register the in-game settings panel
    SFF_UI::Register();

    return true;
}