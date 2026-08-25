#include <Windows.h>

#include <spdlog/sinks/basic_file_sink.h>

#include "SFF_Settings.h"
#include "SFF_UI.h"

namespace {
    constexpr RE::FormID kDialogueFollowerID = 0x000750BA;
    constexpr std::uint32_t kVanillaFollowerAlias = 0;
    constexpr std::uint32_t kMirrorAlias = 0;
    constexpr std::uint32_t kFirstExtraAlias = 1;
    constexpr std::uint32_t kExtraAliasCount = 7;
    constexpr std::int32_t kBaseFollowers = 1;
    constexpr std::int32_t kMaxExtras = 7;
    constexpr const char* kRequiredPluginName = "Simple Follower Framework.esp";

    RE::TESQuest* g_sffQuest = nullptr;
    RE::TESQuest* g_dialogueFollower = nullptr;
    RE::FormID g_trackedPrimary = 0;

    RE::TESGlobal* g_playerFollowerCount = nullptr;
    RE::TESGlobal* g_sffCanRecruitMore = nullptr;
    RE::TESGlobal* g_sffCurrentFollowerCount = nullptr;
    RE::TESGlobal* g_sffFollowerSandbox = nullptr;
    RE::SpellItem* g_friendlyFireSpell = nullptr;

    std::unordered_map<RE::FormID, std::uint8_t> g_essOrig{};

    void SyncState();

    void SetupLog() {
        auto folder = SKSE::log::log_directory();
        if (!folder) return;
        auto path = *folder / "SimpleFollowerFramework.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        auto log = std::make_shared<spdlog::logger>("log", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] %v");
    }

    RE::TESQuest* GetSFFQuest() {
        if (!g_sffQuest) {
            auto* form = RE::TESForm::LookupByEditorID("SFF_FollowerQuest");
            if (form) g_sffQuest = form->As<RE::TESQuest>();
        }
        return g_sffQuest;
    }

    RE::TESQuest* GetDialogueFollower() {
        if (!g_dialogueFollower) {
            auto* form = RE::TESForm::LookupByID(kDialogueFollowerID);
            if (form) g_dialogueFollower = form->As<RE::TESQuest>();
        }
        return g_dialogueFollower;
    }

    RE::BGSRefAlias* GetAlias(RE::TESQuest* quest, std::uint32_t aliasID) {
        if (!quest) return nullptr;
        for (auto* base : quest->aliases) {
            if (base && base->aliasID == aliasID) return static_cast<RE::BGSRefAlias*>(base);
        }
        return nullptr;
    }

    RE::Actor* AliasActor(RE::TESQuest* quest, std::uint32_t aliasID) {
        auto* alias = GetAlias(quest, aliasID);
        return alias ? alias->GetActorReference() : nullptr;
    }

    void FillAlias(RE::TESQuest* quest, std::uint32_t aliasID, RE::Actor* actor) {
        if (!quest || !actor) return;
        quest->ForceRefIntoAlias(aliasID, actor);
    }

    void ClearAlias(RE::TESQuest* quest, std::uint32_t aliasID) {
        auto* alias = GetAlias(quest, aliasID);
        if (!alias) return;
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) return;
        const auto handle = policy->GetHandleForObject(RE::BGSRefAlias::VMTYPEID, alias);
        if (handle == policy->EmptyHandle()) return;
        auto* args = RE::MakeFunctionArguments();
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        vm->DispatchMethodCall(handle, "ReferenceAlias"sv, "Clear"sv, args, callback);
    }

    RE::Actor* PrimaryFollower() { return AliasActor(GetDialogueFollower(), kVanillaFollowerAlias); }

    std::int32_t ExtraSlotOf(RE::Actor* actor) {
        if (!actor) return -1;
        auto* sff = GetSFFQuest();
        if (!sff) return -1;
        for (std::uint32_t i = 0; i < kExtraAliasCount; ++i) {
            if (AliasActor(sff, kFirstExtraAlias + i) == actor) {
                return static_cast<std::int32_t>(kFirstExtraAlias + i);
            }
        }
        return -1;
    }

    std::int32_t FirstFreeExtraSlot() {
        auto* sff = GetSFFQuest();
        if (!sff) return -1;
        for (std::uint32_t i = 0; i < kExtraAliasCount; ++i) {
            auto* a = AliasActor(sff, kFirstExtraAlias + i);
            if (!a || a->IsDead()) return static_cast<std::int32_t>(kFirstExtraAlias + i);
        }
        return -1;
    }

    bool IsManagedFollower(RE::Actor* actor) {
        if (!actor) return false;
        return actor == PrimaryFollower() || ExtraSlotOf(actor) >= 0;
    }

    RE::TESFaction* GetFaction(std::string_view editorID) {
        auto* form = RE::TESForm::LookupByEditorID(editorID);
        return form ? form->As<RE::TESFaction>() : nullptr;
    }

    RE::TESGlobal* LookupGlobal(RE::TESGlobal*& cache, std::string_view editorID) {
        if (!cache) {
            auto* form = RE::TESForm::LookupByEditorID(editorID);
            if (form) cache = form->As<RE::TESGlobal>();
        }
        return cache;
    }

    void UpdateEssentialForActor(RE::Actor* a, bool wantFollower) {
        if (!a) return;
        if (a == RE::PlayerCharacter::GetSingleton()) return;
        auto* base = a->GetActorBase();
        if (!base) return;

        const auto id = base->GetFormID();
        const bool want = wantFollower && SFF_Settings::FollowerEssential;
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

    void ApplySandbox() {
        if (auto* glob = LookupGlobal(g_sffFollowerSandbox, "SFF_FollowerSandbox")) {
            glob->value = SFF_Settings::FollowerSandbox ? 1.0f : 0.0f;
        }
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
            if (p.has && HasPerkFromSpec(p.file, p.localID)) ++owned;
        }
        return owned;
    }

    std::int32_t GetSpeechBasedFollowerCap() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return kBaseFollowers;

        const auto speech =
            static_cast<std::int32_t>(player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech));
        const std::int32_t levelsPerSlot =
            (SFF_Settings::SpeechLevelsPerSlot > 0) ? SFF_Settings::SpeechLevelsPerSlot : 10;
        return std::clamp(kBaseFollowers + (speech / levelsPerSlot), 1, kBaseFollowers + kMaxExtras);
    }

    std::int32_t GetTotalFollowerCap() {
        if (SFF_Settings::FollowerPerkOption == 0) {
            const int extras = std::clamp(static_cast<int>(SFF_Settings::MaxExtraFollowers), 0, kMaxExtras);
            return std::clamp(kBaseFollowers + extras, 1, kBaseFollowers + kMaxExtras);
        }
        if (SFF_Settings::FollowerPerkOption == 1) {
            return std::clamp(kBaseFollowers + CountOwnedPerksFromList(), 1, kBaseFollowers + kMaxExtras);
        }
        return GetSpeechBasedFollowerCap();
    }

    void SyncState() {
        auto* sff = GetSFFQuest();
        auto* df = GetDialogueFollower();
        if (!sff || !df) return;

        auto* primary = PrimaryFollower();
        if (primary && primary->IsDead()) primary = nullptr;

        if (AliasActor(sff, kMirrorAlias) != primary) {
            if (primary)
                FillAlias(sff, kMirrorAlias, primary);
            else
                ClearAlias(sff, kMirrorAlias);
        }

        std::unordered_set<RE::FormID> live{};
        std::int32_t count = 0;

        if (primary) {
            ++count;
            UpdateEssentialForActor(primary, true);
            if (auto* base = primary->GetActorBase()) live.insert(base->GetFormID());
        }

        for (std::uint32_t i = 0; i < kExtraAliasCount; ++i) {
            auto* a = AliasActor(sff, kFirstExtraAlias + i);
            if (!a) continue;
            if (a->IsDead()) {
                ClearAlias(sff, kFirstExtraAlias + i);
                continue;
            }
            ++count;
            UpdateEssentialForActor(a, true);
            if (auto* base = a->GetActorBase()) live.insert(base->GetFormID());
        }

        std::vector<RE::FormID> stale{};
        for (const auto& [baseID, bits] : g_essOrig) {
            if (!live.contains(baseID)) stale.push_back(baseID);
        }
        for (auto baseID : stale) {
            auto it = g_essOrig.find(baseID);
            if (it == g_essOrig.end()) continue;
            auto* form = RE::TESForm::LookupByID(baseID);
            auto* base = form ? form->As<RE::TESNPC>() : nullptr;
            if (base) {
                const std::uint8_t bits = it->second;
                if (bits & 1)
                    base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
                else
                    base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kEssential);
                if (bits & 2)
                    base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
                else
                    base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
            }
            g_essOrig.erase(baseID);
        }

        const auto cap = GetTotalFollowerCap();
        const bool canRecruitMore = (count < cap);

        if (auto* glob = LookupGlobal(g_playerFollowerCount, "PlayerFollowerCount")) {
            glob->value = canRecruitMore ? 0.0f : 1.0f;
        }
        if (auto* glob = LookupGlobal(g_sffCanRecruitMore, "SFF_CanRecruitMore")) {
            glob->value = canRecruitMore ? 1.0f : 0.0f;
        }
        if (auto* glob = LookupGlobal(g_sffCurrentFollowerCount, "SFF_CurrentFollowerCount")) {
            glob->value = static_cast<float>(count);
        }
    }

    RE::Actor* CurrentDialogueSpeaker() {
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        if (!mtm) return nullptr;
        auto ref = mtm->speaker.get();
        if (!ref) ref = mtm->lastSpeaker.get();
        return ref ? ref->As<RE::Actor>() : nullptr;
    }

    void SwapIntoVanillaAlias(RE::Actor* actor) {
        auto* sff = GetSFFQuest();
        auto* df = GetDialogueFollower();
        if (!sff || !df || !actor) return;

        auto* current = PrimaryFollower();
        if (current == actor) return;

        const auto slot = ExtraSlotOf(actor);
        if (slot < 0) return;

        if (current)
            FillAlias(sff, static_cast<std::uint32_t>(slot), current);
        else
            ClearAlias(sff, static_cast<std::uint32_t>(slot));

        FillAlias(df, kVanillaFollowerAlias, actor);
        g_trackedPrimary = actor->GetFormID();
        logger::info("swap: {:08X} slot{} -> vanilla alias (displaced {:08X})", actor->GetFormID(), slot,
                     current ? current->GetFormID() : 0);

        actor->EvaluatePackage();
        if (current) current->EvaluatePackage();

        SyncState();
    }

    void ReconcileVanillaAlias() {
        auto* sff = GetSFFQuest();
        auto* df = GetDialogueFollower();
        if (!sff || !df) return;

        auto* cur = PrimaryFollower();
        const RE::FormID curID = cur ? cur->GetFormID() : 0;
        if (curID == g_trackedPrimary) return;

        if (g_trackedPrimary) {
            auto* form = RE::TESForm::LookupByID(g_trackedPrimary);
            auto* prev = form ? form->As<RE::Actor>() : nullptr;
            if (prev && prev != cur && !prev->IsDead() && prev->IsPlayerTeammate() && ExtraSlotOf(prev) < 0) {
                const auto slot = FirstFreeExtraSlot();
                if (slot >= 0) {
                    FillAlias(sff, static_cast<std::uint32_t>(slot), prev);
                    prev->EvaluatePackage();
                    logger::info("hire: {:08X} took vanilla alias, moved {:08X} -> slot{}",
                                 cur ? cur->GetFormID() : 0, prev->GetFormID(), slot);
                }
            }
        }

        g_trackedPrimary = curID;
        SyncState();
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
        if (!dh) return false;
        if (dh->LookupLoadedModByName(kRequiredPluginName)) return true;
        return RE::TESForm::LookupByEditorID("SFF_FollowerQuest") != nullptr;
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
                for (const char* tok : {";", "#", "//"}) {
                    auto p = line.find(tok);
                    if (p != std::string::npos) line = line.substr(0, p);
                }
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                if (line.empty()) continue;

                bool enabled = (line.front() == '*');
                if (enabled) line.erase(line.begin());
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

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() {
            static MenuSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (e && e->menuName == RE::DialogueMenu::MENU_NAME) {
                if (e->opening) {
                    auto* speaker = CurrentDialogueSpeaker();
                    logger::info("dialogue open: speaker={:08X} slot={} primary={:08X}",
                                 speaker ? speaker->GetFormID() : 0, speaker ? ExtraSlotOf(speaker) : -1,
                                 PrimaryFollower() ? PrimaryFollower()->GetFormID() : 0);
                    if (speaker && ExtraSlotOf(speaker) >= 0) SwapIntoVanillaAlias(speaker);
                    SyncState();
                } else {
                    ReconcileVanillaAlias();
                }
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

            if (ExtraSlotOf(actor) >= 0) SwapIntoVanillaAlias(actor);
            SyncState();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent> {
    public:
        static DeathSink* GetSingleton() {
            static DeathSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* e,
                                              RE::BSTEventSource<RE::TESDeathEvent>*) override {
            if (!e || !e->dead) return RE::BSEventNotifyControl::kContinue;
            auto* actor = e->actorDying ? e->actorDying->As<RE::Actor>() : nullptr;
            if (actor && IsManagedFollower(actor)) SyncState();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Install() {
        if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESActivateEvent>(ActivateSink::GetSingleton());
            events->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
        }
    }

    void ResetCaches() {
        g_sffQuest = nullptr;
        g_dialogueFollower = nullptr;
        g_playerFollowerCount = nullptr;
        g_sffCanRecruitMore = nullptr;
        g_sffCurrentFollowerCount = nullptr;
        g_sffFollowerSandbox = nullptr;
        g_friendlyFireSpell = nullptr;
        g_trackedPrimary = 0;
    }

    void EnsureQuestRunning() {
        auto* sff = GetSFFQuest();
        if (sff && !sff->IsRunning()) sff->Start();
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
            Install();
        } else if (msg->type == SKSE::MessagingInterface::kPostLoadGame ||
                   msg->type == SKSE::MessagingInterface::kNewGame) {
            ResetCaches();
            SFF_Settings::Load(true);
            EnsureQuestRunning();
            if (auto* primary = PrimaryFollower()) g_trackedPrimary = primary->GetFormID();
            SyncState();
            ApplyFriendlyFire();
            ApplySandbox();
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SetupLog();
    EarlyPreflightCheck();

    SKSE::Init(skse);

    SFF_Settings::Load();

    SFF_Settings::ApplyGateCallback = []() { SyncState(); };
    SFF_Settings::FriendlyFireCallback = []() { ApplyFriendlyFire(); };
    SFF_Settings::SandboxCallback = []() { ApplySandbox(); };

    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);

    SFF_UI::Register();

    return true;
}
