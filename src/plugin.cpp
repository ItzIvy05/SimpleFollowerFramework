#include <Windows.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "SFF_Settings.h"
#include "SFF_UI.h"

namespace {
    RE::TESGlobal* g_playerFollowerCount = nullptr;
    RE::TESGlobal* g_sffCanRecruitMore = nullptr;
    RE::TESGlobal* g_sffCurrentFollowerCount = nullptr;
    RE::TESGlobal* g_sffFollowerSandbox = nullptr;
    RE::TESGlobal* g_sffFollowerHomes = nullptr;
    RE::TESFaction* g_currentFollowerFaction = nullptr;
    RE::TESFaction* g_potentialFollowerFaction = nullptr;
    RE::TESQuest* g_dialogueFollower = nullptr;
    RE::SpellItem* g_friendlyFireSpell = nullptr;

    std::unordered_map<RE::FormID, std::uint8_t> g_essOrig{};

    constexpr const char* kRequiredPluginName = "Simple Follower Framework.esp";

    template <class T> T* Cached(T*& slot, std::string_view editorID) {
        if (!slot) {
            auto* form = RE::TESForm::LookupByEditorID(editorID);
            slot = form ? form->As<T>() : nullptr;
        }
        return slot;
    }

    void SetAbility(RE::Actor* a, bool want) {
        auto* spell = Cached(g_friendlyFireSpell, "IvyCompanionsSafeSpell");
        if (!a || !spell || a->HasSpell(spell) == want) return;
        if (want) {
            a->AddSpell(spell);
        } else {
            a->RemoveSpell(spell);
        }
    }

    void ApplyFriendlyFire() { SetAbility(RE::PlayerCharacter::GetSingleton(), SFF_Settings::FriendlyFire); }

    void ApplyCrossfireForActor(RE::Actor* a, bool want) {
        if (a != RE::PlayerCharacter::GetSingleton()) SetAbility(a, want);
    }

    bool IsValidActor(RE::Actor* a) { return a && a != RE::PlayerCharacter::GetSingleton() && !a->IsDead(); }

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
        if (dh->LookupLoadedModByName(kRequiredPluginName)) return true;
        if (RE::TESForm::LookupByEditorID("SFF_CurrentFollowerCount")) return true;
        return false;
    }

    bool PluginsTxtExplicitlyDisablesRequiredPlugin() {
        char localAppData[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, static_cast<DWORD>(sizeof(localAppData)));
        if (n == 0 || n >= sizeof(localAppData)) return false;

        const char* dirs[] = {"Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim VR", "Skyrim"};

        for (auto* d : dirs) {
            std::ifstream in(std::string(localAppData) + "\\" + d + "\\plugins.txt");
            if (!in.is_open()) continue;

            std::string line;
            while (std::getline(in, line)) {
                for (const char* tok : {";", "#", "//"}) {
                    auto p = line.find(tok);
                    if (p != std::string::npos) line.resize(p);
                }
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                if (line.empty()) continue;

                const bool enabled = line.front() == '*';
                const auto first = line.find_first_not_of(" \t\r\n\v\f", enabled ? 1 : 0);
                if (first != std::string::npos && _stricmp(line.c_str() + first, kRequiredPluginName) == 0) return !enabled;
            }
        }
        return false;
    }

    void EarlyPreflightCheck() {
        std::string espPath = std::string("Data\\") + kRequiredPluginName;
        if (!FileExistsA(espPath.c_str())) MessageAndExit("Missing required file:\n\nData\\Simple Follower Framework.esp\nInstall it (or fix your mod manager / VFS), then relaunch.");
        if (PluginsTxtExplicitlyDisablesRequiredPlugin()) MessageAndExit("Required plugin is disabled:\nSimple Follower Framework.esp\nEnable it in your load order, then relaunch.");
    }

    void ApplySandbox() {
        if (auto* glob = Cached(g_sffFollowerSandbox, "SFF_FollowerSandbox")) glob->value = SFF_Settings::FollowerSandbox ? 1.0f : 0.0f;
    }

    void ApplyHomes() {
        if (auto* glob = Cached(g_sffFollowerHomes, "SFF_FollowerHomes")) glob->value = SFF_Settings::FollowerHomes ? 1.0f : 0.0f;
    }

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
            if (HasPerkFromSpec(p.file, p.localID)) ++owned;
        }
        return owned;
    }

    std::int32_t GetSpeechBasedFollowerCap() {
        constexpr std::int32_t kBase = 1, kMaxExtras = 7;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return kBase;

        const auto speech = static_cast<std::int32_t>(player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech));
        const std::int32_t levelsPerSlot = (SFF_Settings::SpeechLevelsPerSlot > 0) ? SFF_Settings::SpeechLevelsPerSlot : 10;
        std::int32_t total = kBase + (speech / levelsPerSlot);
        return std::clamp(total, 1, kBase + kMaxExtras);
    }

    std::int32_t GetTotalFollowerCapFromSettings() {
        constexpr std::int32_t kBase = 1, kMaxExtras = 7;

        if (SFF_Settings::FollowerPerkOption == 0) return kBase + std::clamp(static_cast<int>(SFF_Settings::MaxExtraFollowers), 0, kMaxExtras);
        if (SFF_Settings::FollowerPerkOption == 1) return std::clamp(kBase + CountOwnedPerksFromList(), 1, kBase + kMaxExtras);
        return GetSpeechBasedFollowerCap();
    }

    void ApplyFollowerDialogueGate() {
        auto* followerCount = Cached(g_playerFollowerCount, "PlayerFollowerCount");
        if (!followerCount) return;

        auto* current = Cached(g_sffCurrentFollowerCount, "SFF_CurrentFollowerCount");
        const bool canRecruitMore = (current ? std::max(static_cast<int>(current->value), 0) : 0) < GetTotalFollowerCapFromSettings();
        followerCount->value = canRecruitMore ? 0.0f : 1.0f;
        if (auto* canRecruit = Cached(g_sffCanRecruitMore, "SFF_CanRecruitMore")) canRecruit->value = canRecruitMore ? 1.0f : 0.0f;
    }

    bool IsInServiceEssential(RE::Actor* a) {
        auto* current = Cached(g_currentFollowerFaction, "CurrentFollowerFaction");
        return a && current && a->IsInFaction(current) && a->IsPlayerTeammate();
    }

    void SetBaseFlag(RE::TESNPC* base, RE::ACTOR_BASE_DATA::Flag flag, bool on) {
        if (on) {
            base->actorData.actorBaseFlags.set(flag);
        } else {
            base->actorData.actorBaseFlags.reset(flag);
        }
    }

    bool RestoreEssentialFlags(RE::TESNPC* base) {
        auto it = g_essOrig.find(base->GetFormID());
        if (it == g_essOrig.end()) return false;
        SetBaseFlag(base, RE::ACTOR_BASE_DATA::Flag::kEssential, it->second & 1);
        SetBaseFlag(base, RE::ACTOR_BASE_DATA::Flag::kProtected, it->second & 2);
        g_essOrig.erase(it);
        return true;
    }

    void UpdateEssentialForActor(RE::Actor* a) {
        auto* base = a->GetActorBase();
        if (!base) return;
        if (!SFF_Settings::FollowerEssential || !IsInServiceEssential(a)) {
            RestoreEssentialFlags(base);
            return;
        }
        auto& flags = base->actorData.actorBaseFlags;
        g_essOrig.try_emplace(base->GetFormID(), static_cast<std::uint8_t>((flags.any(RE::ACTOR_BASE_DATA::Flag::kEssential) ? 1 : 0) | (flags.any(RE::ACTOR_BASE_DATA::Flag::kProtected) ? 2 : 0)));
        flags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
        flags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
    }

    void SyncParty(bool pull) {
        auto* quest = Cached(g_dialogueFollower, "DialogueFollower");
        if (!quest) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (auto* alias : quest->aliases) {
            if (!alias || alias->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) continue;
            auto* a = static_cast<RE::BGSRefAlias*>(alias)->GetActorReference();
            if (!a) continue;
            const bool inService = IsInServiceEssential(a);
            ApplyCrossfireForActor(a, SFF_Settings::FollowerCrossfire && inService);
            if (pull && inService && !a->IsOnMount() && a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) == 0.0f && a->GetParentCell() != player->GetParentCell()) a->MoveTo(player);
        }
    }

    void DeferSyncParty(bool pull = false) {
        if (auto* task = SKSE::GetTaskInterface()) task->AddTask([pull]() { SyncParty(pull); });
    }

    bool ApplyFollowerEssential(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!IsValidActor(a)) return false;
        UpdateEssentialForActor(a);
        ApplyCrossfireForActor(a, SFF_Settings::FollowerCrossfire && IsInServiceEssential(a));
        return true;
    }

    bool RestoreFollowerEssential(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!a) return false;
        ApplyCrossfireForActor(a, false);
        auto* base = a->GetActorBase();
        return base && RestoreEssentialFlags(base);
    }

    bool AddVanillaFollower(RE::StaticFunctionTag*, RE::Actor* a) {
        if (!IsValidActor(a)) return false;
        auto* potential = Cached(g_potentialFollowerFaction, "PotentialFollowerFaction");
        auto* current = Cached(g_currentFollowerFaction, "CurrentFollowerFaction");
        if (!potential || !current || !a->IsInFaction(potential)) return false;
        if (!a->IsInFaction(current)) a->AddToFaction(current, 0);
        a->EvaluatePackage();
        UpdateEssentialForActor(a);
        ApplyCrossfireForActor(a, SFF_Settings::FollowerCrossfire);
        return true;
    }

    std::int32_t GetMaxFollowers(RE::StaticFunctionTag*) { return GetTotalFollowerCapFromSettings(); }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("AddVanillaFollower", "SFF_SKSE", AddVanillaFollower);
        vm->RegisterFunction("GetMaxFollowers", "SFF_SKSE", GetMaxFollowers);
        vm->RegisterFunction("ApplyFollowerEssential", "SFF_SKSE", ApplyFollowerEssential);
        vm->RegisterFunction("RestoreFollowerEssential", "SFF_SKSE", RestoreFollowerEssential);
        return true;
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() {
            static MenuSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (e->menuName == "Dialogue Menu" && e->opening) {
                ApplyFollowerDialogueGate();
                if (SFF_Settings::FollowerCrossfire) DeferSyncParty();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent> {
    public:
        static ActivateSink* GetSingleton() {
            static ActivateSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* e, RE::BSTEventSource<RE::TESActivateEvent>*) override {
            if (e->actionRef.get() == RE::PlayerCharacter::GetSingleton() && e->objectActivated && e->objectActivated->As<RE::Actor>()) ApplyFollowerDialogueGate();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    struct TravelSink final : RE::BSTEventSink<RE::TESFastTravelEndEvent> {
        RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override { DeferSyncParty(true); return RE::BSEventNotifyControl::kContinue; }
    } g_travelSink;
    void Install() {
        if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) events->AddEventSink<RE::TESActivateEvent>(ActivateSink::GetSingleton());
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton(); events && events->GetEventSource<RE::TESFastTravelEndEvent>()) events->AddEventSink<RE::TESFastTravelEndEvent>(&g_travelSink);
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg) {
        if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
            if (!IsRequiredPluginLoaded()) MessageAndExit("Missing required plugin in load order:\nSimple Follower Framework.esp\nEnable it in your load order, then relaunch.");
            ApplyFollowerDialogueGate();
            Install();
        } else if (msg->type == SKSE::MessagingInterface::kPostLoadGame || msg->type == SKSE::MessagingInterface::kNewGame) {
            SFF_Settings::Load(true);
            ApplyFollowerDialogueGate();
            ApplyFriendlyFire();
            ApplySandbox();
            ApplyHomes();
            DeferSyncParty();
        }
    }
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    EarlyPreflightCheck();
    SKSE::Init(skse);
    SFF_Settings::Load();

    SFF_Settings::ApplyGateCallback = []() { ApplyFollowerDialogueGate(); };
    SFF_Settings::FriendlyFireCallback = []() { ApplyFriendlyFire(); };
    SFF_Settings::SandboxCallback = []() { ApplySandbox(); };
    SFF_Settings::HomesCallback = []() { ApplyHomes(); };
    SFF_Settings::CrossfireCallback = []() { DeferSyncParty(); };

    if (auto* papyrus = SKSE::GetPapyrusInterface()) papyrus->Register(RegisterPapyrus);
    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);

    SFF_UI::Register();
    return true;
}
