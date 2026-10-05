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
    std::int32_t g_slotCapacity = 8;

    constexpr const char* kRequiredPluginName = "Simple Follower Framework.esp";

    template <class T> T* ResolveForm(RE::FormID localID, std::string_view plugin, std::string_view editorID) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (auto* form = dh ? dh->LookupForm<T>(localID, plugin) : nullptr) {
            return form;
        }
        auto* form = RE::TESForm::LookupByEditorID(editorID);
        return form ? form->As<T>() : nullptr;
    }

    void ResolveForms() {
        g_playerFollowerCount = ResolveForm<RE::TESGlobal>(0x0BCC98, "Skyrim.esm", "PlayerFollowerCount");
        g_currentFollowerFaction = ResolveForm<RE::TESFaction>(0x05C84E, "Skyrim.esm", "CurrentFollowerFaction");
        g_potentialFollowerFaction = ResolveForm<RE::TESFaction>(0x05C84D, "Skyrim.esm", "PotentialFollowerFaction");
        g_dialogueFollower = ResolveForm<RE::TESQuest>(0x0750BA, "Skyrim.esm", "DialogueFollower");
        g_sffCanRecruitMore = ResolveForm<RE::TESGlobal>(0x001, kRequiredPluginName, "SFF_CanRecruitMore");
        g_sffCurrentFollowerCount = ResolveForm<RE::TESGlobal>(0x002, kRequiredPluginName, "SFF_CurrentFollowerCount");
        g_sffFollowerSandbox = ResolveForm<RE::TESGlobal>(0x805, kRequiredPluginName, "SFF_FollowerSandbox");
        g_sffFollowerHomes = ResolveForm<RE::TESGlobal>(0x986, kRequiredPluginName, "SFF_FollowerHomes");
        g_friendlyFireSpell = ResolveForm<RE::SpellItem>(0x800, kRequiredPluginName, "IvyCompanionsSafeSpell");
    }

    void SetAbility(RE::Actor* a, bool want) {
        if (!a || !g_friendlyFireSpell || a->HasSpell(g_friendlyFireSpell) == want) return;
        if (want) {
            a->AddSpell(g_friendlyFireSpell);
        } else {
            a->RemoveSpell(g_friendlyFireSpell);
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
        if (g_sffFollowerSandbox) g_sffFollowerSandbox->value = SFF_Settings::FollowerSandbox ? 1.0f : 0.0f;
    }

    void ApplyHomes() {
        if (g_sffFollowerHomes) g_sffFollowerHomes->value = SFF_Settings::FollowerHomes ? 1.0f : 0.0f;
    }

    bool HasPerkFromSpec(const std::string& file, std::uint32_t localID) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return false;
        auto* mod = dh->LookupModByName(file);
        if (!mod) return false;
        auto* perk = dh->LookupForm<RE::BGSPerk>(localID & (mod->IsLight() ? 0xFFFu : 0xFFFFFFu), file);
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

    std::int32_t GetEffectiveFollowerCap() { return std::min(GetTotalFollowerCapFromSettings(), g_slotCapacity); }

    void CountFollowerSlots() {
        if (!g_dialogueFollower) {
            return;
        }
        std::int32_t slots = 1;
        for (auto* alias : g_dialogueFollower->aliases) {
            if (alias && std::string_view(alias->aliasName.c_str()).starts_with("ExtraFollower")) {
                ++slots;
            }
        }
        g_slotCapacity = slots;
    }

    void ApplyFollowerDialogueGate(RE::Actor* speaker) {
        if (!g_playerFollowerCount) return;

        const int count = g_sffCurrentFollowerCount ? std::max(static_cast<int>(g_sffCurrentFollowerCount->value), 0) : 0;
        const bool canRecruitMore = count < GetEffectiveFollowerCap();
        const bool hireable = speaker && !speaker->IsDead() && g_potentialFollowerFaction && speaker->IsInFaction(g_potentialFollowerFaction) && !speaker->IsPlayerTeammate() && !(g_currentFollowerFaction && speaker->IsInFaction(g_currentFollowerFaction));
        g_playerFollowerCount->value = hireable ? (canRecruitMore ? 0.0f : 1.0f) : (count > 0 ? 1.0f : 0.0f);
        if (g_sffCanRecruitMore) g_sffCanRecruitMore->value = canRecruitMore ? 1.0f : 0.0f;
    }

    RE::Actor* DialogueSpeaker() {
        auto* topics = RE::MenuTopicManager::GetSingleton();
        auto speaker = topics ? topics->speaker.get() : nullptr;
        return speaker ? speaker->As<RE::Actor>() : nullptr;
    }

    bool IsInServiceEssential(RE::Actor* a) { return a && g_currentFollowerFaction && a->IsInFaction(g_currentFollowerFaction) && a->IsPlayerTeammate(); }

    void SetBaseFlag(RE::TESNPC* base, RE::ACTOR_BASE_DATA::Flag flag, bool on) {
        if (on) {
            base->actorData.actorBaseFlags.set(flag);
        } else {
            base->actorData.actorBaseFlags.reset(flag);
        }
    }

    void ApplyOriginalFlags(RE::TESNPC* base, std::uint8_t orig) {
        SetBaseFlag(base, RE::ACTOR_BASE_DATA::Flag::kEssential, orig & 1);
        SetBaseFlag(base, RE::ACTOR_BASE_DATA::Flag::kProtected, orig & 2);
    }

    bool RestoreEssentialFlags(RE::TESNPC* base) {
        auto it = g_essOrig.find(base->GetFormID());
        if (it == g_essOrig.end()) return false;
        ApplyOriginalFlags(base, it->second);
        g_essOrig.erase(it);
        return true;
    }

    void RestoreAllEssentialFlags() {
        for (const auto& [formID, orig] : g_essOrig) {
            if (auto* base = RE::TESForm::LookupByID<RE::TESNPC>(formID)) {
                ApplyOriginalFlags(base, orig);
            }
        }
        g_essOrig.clear();
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
        if (!g_dialogueFollower) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (auto* alias : g_dialogueFollower->aliases) {
            if (!alias || alias->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) continue;
            auto* a = static_cast<RE::BGSRefAlias*>(alias)->GetActorReference();
            if (!a) continue;
            const bool inService = IsInServiceEssential(a);
            UpdateEssentialForActor(a);
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
        if (!g_potentialFollowerFaction || !g_currentFollowerFaction || !a->IsInFaction(g_potentialFollowerFaction)) return false;
        if (!a->IsInFaction(g_currentFollowerFaction)) a->AddToFaction(g_currentFollowerFaction, 0);
        a->EvaluatePackage();
        UpdateEssentialForActor(a);
        ApplyCrossfireForActor(a, SFF_Settings::FollowerCrossfire);
        return true;
    }

    std::int32_t GetMaxFollowers(RE::StaticFunctionTag*) { return GetEffectiveFollowerCap(); }

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
            if (e->menuName == "Dialogue Menu") {
                ApplyFollowerDialogueGate(e->opening ? DialogueSpeaker() : nullptr);
                if (e->opening && SFF_Settings::FollowerCrossfire) DeferSyncParty();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    struct DialogueStartHook {
        static bool Thunk(RE::Actor* actor, bool start, bool forceGreet, RE::TESTopicInfo* topic) {
            if (start) {
                ApplyFollowerDialogueGate(actor);
            }
            const bool result = original(actor, start, forceGreet, topic);
            if (start) {
                ApplyFollowerDialogueGate(DialogueSpeaker());
            }
            return result;
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    class TravelSink final : public RE::BSTEventSink<RE::TESFastTravelEndEvent> {
    public:
        static TravelSink* GetSingleton() {
            static TravelSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override {
            DeferSyncParty(true);
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Install() {
        if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        REL::Relocation<std::uintptr_t> characterVtbl{RE::VTABLE_Character[0]};
        DialogueStartHook::original = characterVtbl.write_vfunc(0x41, DialogueStartHook::Thunk);
        auto* events = RE::ScriptEventSourceHolder::GetSingleton();
        if (!events) return;
        if (events->GetEventSource<RE::TESFastTravelEndEvent>()) events->AddEventSink<RE::TESFastTravelEndEvent>(TravelSink::GetSingleton());
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg) {
        if (msg->type == SKSE::MessagingInterface::kDataLoaded) {
            ResolveForms();
            if (!g_sffCurrentFollowerCount) MessageAndExit("Missing required plugin in load order:\nSimple Follower Framework.esp\nEnable it in your load order, then relaunch.");
            CountFollowerSlots();
            ApplyFollowerDialogueGate(nullptr);
            Install();
        } else if (msg->type == SKSE::MessagingInterface::kPreLoadGame) {
            RestoreAllEssentialFlags();
        } else if (msg->type == SKSE::MessagingInterface::kPostLoadGame || msg->type == SKSE::MessagingInterface::kNewGame) {
            if (msg->type == SKSE::MessagingInterface::kNewGame) {
                RestoreAllEssentialFlags();
            }
            SFF_Settings::Load(true);
            ApplyFollowerDialogueGate(nullptr);
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

    SFF_Settings::ApplyGateCallback = []() { ApplyFollowerDialogueGate(nullptr); };
    SFF_Settings::FriendlyFireCallback = []() { ApplyFriendlyFire(); };
    SFF_Settings::SandboxCallback = []() { ApplySandbox(); };
    SFF_Settings::HomesCallback = []() { ApplyHomes(); };
    SFF_Settings::EssentialCallback = []() { DeferSyncParty(); };
    SFF_Settings::CrossfireCallback = []() { DeferSyncParty(); };

    if (auto* papyrus = SKSE::GetPapyrusInterface()) papyrus->Register(RegisterPapyrus);
    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);

    SFF_UI::Register();
    return true;
}
