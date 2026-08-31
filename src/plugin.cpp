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
    constexpr std::int32_t kMaxExtras = static_cast<std::int32_t>(kExtraAliasCount);
    constexpr std::size_t kMaxFollowers = kExtraAliasCount + 1;
    constexpr float kLeftBehindDistance = 2500.0f;

    RE::TESQuest* g_sffQuest = nullptr;
    RE::TESQuest* g_dialogueFollower = nullptr;
    RE::BGSRefAlias* g_vanillaAlias = nullptr;
    RE::BGSRefAlias* g_mirrorAlias = nullptr;
    std::array<RE::BGSRefAlias*, kExtraAliasCount> g_extraAliases{};

    RE::FormID g_trackedPrimary = 0;

    RE::TESGlobal* g_playerFollowerCount = nullptr;
    RE::TESGlobal* g_sffCanRecruitMore = nullptr;
    RE::TESGlobal* g_sffCurrentFollowerCount = nullptr;
    RE::TESGlobal* g_sffFollowerSandbox = nullptr;
    RE::SpellItem* g_friendlyFireSpell = nullptr;
    RE::TESFaction* g_potentialFollower = nullptr;

    std::array<RE::BGSPerk*, SFF_Settings::kMaxPerkSpecs> g_perkCache{};
    std::uint32_t g_perkCacheGeneration = 0;
    bool g_perkCacheValid = false;

    std::unordered_map<RE::FormID, std::uint8_t> g_essOrig{};
    std::unordered_set<RE::FormID> g_crossfireGranted{};

    void SyncState();

    void SetupLog() {
        auto folder = SKSE::log::log_directory();
        if (!folder) return;
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            (*folder / "SimpleFollowerFramework.log").string(), true);
        auto log = std::make_shared<spdlog::logger>("log", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] %v");
    }

    template <class T>
    T* LookupCached(T*& cache, std::string_view editorID) {
        if (!cache) {
            auto* form = RE::TESForm::LookupByEditorID(editorID);
            if (form) cache = form->As<T>();
        }
        return cache;
    }

    RE::TESQuest* GetSFFQuest() { return LookupCached(g_sffQuest, "SFF_FollowerQuest"sv); }

    RE::TESQuest* GetDialogueFollower() {
        if (!g_dialogueFollower) {
            auto* form = RE::TESForm::LookupByID(kDialogueFollowerID);
            if (form) g_dialogueFollower = form->As<RE::TESQuest>();
        }
        return g_dialogueFollower;
    }

    bool EnsureAliases() {
        if (g_vanillaAlias && g_mirrorAlias) return true;

        auto* sff = GetSFFQuest();
        auto* df = GetDialogueFollower();
        if (!sff || !df) return false;

        for (auto* base : df->aliases) {
            if (base && base->aliasID == kVanillaFollowerAlias) {
                g_vanillaAlias = static_cast<RE::BGSRefAlias*>(base);
                break;
            }
        }
        for (auto* base : sff->aliases) {
            if (!base) continue;
            if (base->aliasID == kMirrorAlias) {
                g_mirrorAlias = static_cast<RE::BGSRefAlias*>(base);
            } else if (base->aliasID >= kFirstExtraAlias && base->aliasID < kFirstExtraAlias + kExtraAliasCount) {
                g_extraAliases[base->aliasID - kFirstExtraAlias] = static_cast<RE::BGSRefAlias*>(base);
            }
        }
        return g_vanillaAlias && g_mirrorAlias;
    }

    RE::Actor* AliasActor(RE::BGSRefAlias* alias) { return alias ? alias->GetActorReference() : nullptr; }

    void FillAlias(RE::BGSRefAlias* alias, RE::Actor* actor) {
        if (alias && actor) alias->ForceRefTo(actor);
    }

    void ClearAlias(RE::BGSRefAlias* alias) {
        if (!alias) return;
        using func_t = void (*)(RE::BSScript::Internal::VirtualMachine*, std::uint32_t, RE::BGSRefAlias*);
        static REL::Relocation<func_t> clearFn{ RELOCATION_ID(54632, 55286) };
        clearFn(RE::BSScript::Internal::VirtualMachine::GetSingleton(), 0, alias);
        if (auto* still = alias->GetActorReference()) {
            logger::info("clear FAILED: alias{} still holds {:08X}", alias->aliasID, still->GetFormID());
        }
    }

    RE::Actor* PrimaryFollower() { return AliasActor(g_vanillaAlias); }

    std::int32_t ExtraSlotOf(RE::Actor* actor) {
        if (!actor) return -1;
        for (std::uint32_t i = 0; i < kExtraAliasCount; ++i) {
            if (AliasActor(g_extraAliases[i]) == actor) return static_cast<std::int32_t>(i);
        }
        return -1;
    }

    std::int32_t FirstFreeExtraSlot() {
        for (std::uint32_t i = 0; i < kExtraAliasCount; ++i) {
            auto* a = AliasActor(g_extraAliases[i]);
            if (!a || a->IsDead()) return static_cast<std::int32_t>(i);
        }
        return -1;
    }

    bool IsManagedFollower(RE::Actor* actor) {
        if (!actor) return false;
        return actor == PrimaryFollower() || ExtraSlotOf(actor) >= 0;
    }

    void RestoreEssentialBase(RE::TESNPC* base, std::uint8_t bits) {
        if (!base) return;
        auto& flags = base->actorData.actorBaseFlags;
        if (bits & 1)
            flags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
        else
            flags.reset(RE::ACTOR_BASE_DATA::Flag::kEssential);
        if (bits & 2)
            flags.set(RE::ACTOR_BASE_DATA::Flag::kProtected);
        else
            flags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
    }

    void UpdateEssentialForActor(RE::Actor* a, bool wantFollower) {
        if (!a || a == RE::PlayerCharacter::GetSingleton()) return;
        auto* base = a->GetActorBase();
        if (!base) return;

        const auto id = base->GetFormID();
        const bool want = wantFollower && SFF_Settings::FollowerEssential;
        auto it = g_essOrig.find(id);

        if (want) {
            if (it == g_essOrig.end()) {
                auto& flags = base->actorData.actorBaseFlags;
                std::uint8_t bits = 0;
                if (flags.any(RE::ACTOR_BASE_DATA::Flag::kEssential)) bits |= 1;
                if (flags.any(RE::ACTOR_BASE_DATA::Flag::kProtected)) bits |= 2;
                g_essOrig.emplace(id, bits);
            }
            base->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kEssential);
            base->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kProtected);
            return;
        }

        if (it != g_essOrig.end()) {
            RestoreEssentialBase(base, it->second);
            g_essOrig.erase(it);
        }
    }

    RE::SpellItem* GetFriendlyFireSpell() {
        return LookupCached(g_friendlyFireSpell, "IvyCompanionsSafeSpell"sv);
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

    void ApplyCrossfireForActor(RE::Actor* a, bool want) {
        auto* spell = GetFriendlyFireSpell();
        if (!a || !spell) return;

        const auto id = a->GetFormID();
        const bool tracked = g_crossfireGranted.contains(id);
        if (want == tracked) return;

        if (want) {
            if (!a->HasSpell(spell)) a->AddSpell(spell);
            g_crossfireGranted.insert(id);
        } else {
            if (a->HasSpell(spell)) a->RemoveSpell(spell);
            g_crossfireGranted.erase(id);
        }
    }

    void RevokeStaleCrossfire(const RE::FormID* liveActors, std::size_t liveCount) {
        if (g_crossfireGranted.empty()) return;
        auto* spell = GetFriendlyFireSpell();
        for (auto it = g_crossfireGranted.begin(); it != g_crossfireGranted.end();) {
            const auto id = *it;
            if (std::find(liveActors, liveActors + liveCount, id) != liveActors + liveCount) {
                ++it;
                continue;
            }
            auto* form = RE::TESForm::LookupByID(id);
            auto* a = form ? form->As<RE::Actor>() : nullptr;
            if (a && spell && a->HasSpell(spell)) a->RemoveSpell(spell);
            it = g_crossfireGranted.erase(it);
        }
    }

    void ApplySandbox() {
        if (auto* glob = LookupCached(g_sffFollowerSandbox, "SFF_FollowerSandbox"sv)) {
            glob->value = SFF_Settings::FollowerSandbox ? 1.0f : 0.0f;
        }
    }

    void RebuildPerkCache() {
        g_perkCache.fill(nullptr);
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (dh) {
            for (std::size_t i = 0; i < SFF_Settings::PerkSpecCount && i < g_perkCache.size(); ++i) {
                const auto& p = SFF_Settings::PerkSpecs[i];
                if (!p.has) continue;
                auto* form = dh->LookupForm(p.localID, p.file);
                if (form) g_perkCache[i] = form->As<RE::BGSPerk>();
            }
        }
        g_perkCacheGeneration = SFF_Settings::PerkListGeneration;
        g_perkCacheValid = true;
    }

    std::int32_t CountOwnedPerks() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return 0;
        if (!g_perkCacheValid || g_perkCacheGeneration != SFF_Settings::PerkListGeneration) RebuildPerkCache();

        std::int32_t owned = 0;
        for (std::size_t i = 0; i < SFF_Settings::PerkSpecCount && i < g_perkCache.size(); ++i) {
            if (g_perkCache[i] && player->HasPerk(g_perkCache[i])) ++owned;
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
        return std::clamp(kBaseFollowers + (speech / levelsPerSlot), kBaseFollowers, kBaseFollowers + kMaxExtras);
    }

    std::int32_t GetTotalFollowerCap() {
        switch (SFF_Settings::FollowerPerkOption) {
        case 0: {
            const auto extras = std::clamp(SFF_Settings::MaxExtraFollowers, 0, kMaxExtras);
            return kBaseFollowers + extras;
        }
        case 1:
            return std::clamp(kBaseFollowers + CountOwnedPerks(), kBaseFollowers, kBaseFollowers + kMaxExtras);
        default:
            return GetSpeechBasedFollowerCap();
        }
    }

    void SyncState() {
        if (!EnsureAliases()) return;

        auto* primary = PrimaryFollower();
        if (primary && primary->IsDead()) primary = nullptr;

        if (AliasActor(g_mirrorAlias) != primary) {
            if (primary)
                FillAlias(g_mirrorAlias, primary);
            else
                ClearAlias(g_mirrorAlias);
        }

        std::array<RE::FormID, kMaxFollowers> liveActors{};
        std::array<RE::FormID, kMaxFollowers> liveBases{};
        std::size_t liveCount = 0;
        std::size_t baseCount = 0;

        const bool wantCrossfire = SFF_Settings::FollowerCrossfire;

        auto track = [&](RE::Actor* a) {
            UpdateEssentialForActor(a, true);
            ApplyCrossfireForActor(a, wantCrossfire);
            liveActors[liveCount++] = a->GetFormID();
            if (auto* base = a->GetActorBase()) liveBases[baseCount++] = base->GetFormID();
        };

        std::int32_t count = 0;
        if (primary) {
            ++count;
            track(primary);
        }

        for (std::uint32_t i = 0; i < kExtraAliasCount; ++i) {
            auto* a = AliasActor(g_extraAliases[i]);
            if (!a) continue;
            if (a->IsDead() || a == primary || !a->IsPlayerTeammate()) {
                logger::info("release slot{}: {:08X} (dead={} dup={} teammate={})", i + kFirstExtraAlias,
                             a->GetFormID(), a->IsDead(), a == primary, a->IsPlayerTeammate());
                UpdateEssentialForActor(a, false);
                ApplyCrossfireForActor(a, false);
                ClearAlias(g_extraAliases[i]);
                a->EvaluatePackage();
                continue;
            }
            ++count;
            track(a);
        }

        RevokeStaleCrossfire(liveActors.data(), liveCount);

        for (auto it = g_essOrig.begin(); it != g_essOrig.end();) {
            if (std::find(liveBases.data(), liveBases.data() + baseCount, it->first) !=
                liveBases.data() + baseCount) {
                ++it;
                continue;
            }
            auto* form = RE::TESForm::LookupByID(it->first);
            RestoreEssentialBase(form ? form->As<RE::TESNPC>() : nullptr, it->second);
            it = g_essOrig.erase(it);
        }

        const bool canRecruitMore = (count < GetTotalFollowerCap());

        if (auto* glob = LookupCached(g_playerFollowerCount, "PlayerFollowerCount"sv)) {
            glob->value = canRecruitMore ? 0.0f : 1.0f;
        }
        if (auto* glob = LookupCached(g_sffCanRecruitMore, "SFF_CanRecruitMore"sv)) {
            glob->value = canRecruitMore ? 1.0f : 0.0f;
        }
        if (auto* glob = LookupCached(g_sffCurrentFollowerCount, "SFF_CurrentFollowerCount"sv)) {
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

    void SwapIntoVanillaAlias(RE::Actor* actor, std::int32_t slot) {
        if (!actor || slot < 0 || !EnsureAliases()) return;

        auto* current = PrimaryFollower();
        if (current == actor) return;

        if (current)
            FillAlias(g_extraAliases[slot], current);
        else
            ClearAlias(g_extraAliases[slot]);

        FillAlias(g_vanillaAlias, actor);
        g_trackedPrimary = actor->GetFormID();

        actor->EvaluatePackage();
        if (current) current->EvaluatePackage();

        SyncState();
    }

    bool IsOrphanedFollower(RE::Actor* actor) {
        if (!actor || actor == RE::PlayerCharacter::GetSingleton()) return false;
        if (!actor->IsPlayerTeammate() || actor->IsDead()) return false;
        auto* fac = LookupCached(g_potentialFollower, "PotentialFollowerFaction"sv);
        if (!fac || !actor->IsInFaction(fac)) return false;
        if (actor == PrimaryFollower() || ExtraSlotOf(actor) >= 0) return false;
        return true;
    }

    bool AdoptOrphan(RE::Actor* actor) {
        if (!IsOrphanedFollower(actor)) return false;

        if (!PrimaryFollower()) {
            FillAlias(g_vanillaAlias, actor);
            g_trackedPrimary = actor->GetFormID();
            actor->EvaluatePackage();
            logger::info("adopt: {:08X} -> vanilla alias", actor->GetFormID());
            return true;
        }

        const auto slot = FirstFreeExtraSlot();
        if (slot >= 0) {
            FillAlias(g_extraAliases[slot], actor);
            actor->EvaluatePackage();
            logger::info("adopt: {:08X} -> slot{}", actor->GetFormID(), slot + kFirstExtraAlias);
            return true;
        }

        actor->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kPlayerTeammate);
        actor->EvaluatePackage();
        logger::info("release: {:08X} orphaned with no free slot", actor->GetFormID());
        return false;
    }

    void RecoverLegacyAliases() {
        auto* df = GetDialogueFollower();
        if (!df) return;
        for (const auto& entry : df->refAliasMap) {
            if (entry.first < 2 || entry.first > 8) continue;
            auto ref = entry.second.get();
            auto* a = ref ? ref->As<RE::Actor>() : nullptr;
            if (!a || !IsOrphanedFollower(a)) continue;
            logger::info("legacy: recovering {:08X} from retired alias {}", a->GetFormID(), entry.first);
            AdoptOrphan(a);
        }
    }

    void OnFollowerActivated(RE::Actor* actor) {
        if (!EnsureAliases()) return;
        const auto slot = actor ? ExtraSlotOf(actor) : -1;
        if (slot >= 0) {
            SwapIntoVanillaAlias(actor, slot);
            return;
        }
        if (AdoptOrphan(actor)) {
            if (const auto s2 = ExtraSlotOf(actor); s2 >= 0) {
                SwapIntoVanillaAlias(actor, s2);
                return;
            }
        }
        SyncState();
    }

    void GatherFollowersAfterTravel() {
        if (!EnsureAliases()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto bring = [&](RE::Actor* a) {
            if (!a || a->IsDead()) return;
            if (a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) return;

            const bool otherCell = a->GetParentCell() != player->GetParentCell();
            if (!otherCell && a->GetDistance(player) < kLeftBehindDistance) return;

            a->MoveTo(player);
            a->EvaluatePackage();
            logger::info("gather: pulled {:08X} to player after travel (otherCell={})", a->GetFormID(), otherCell);
        };

        bring(PrimaryFollower());
        for (std::uint32_t i = 0; i < kExtraAliasCount; ++i) bring(AliasActor(g_extraAliases[i]));
    }

    void ReconcileVanillaAlias() {
        if (!EnsureAliases()) return;

        auto* cur = PrimaryFollower();
        const RE::FormID curID = cur ? cur->GetFormID() : 0;
        if (curID == g_trackedPrimary) return;

        if (g_trackedPrimary) {
            auto* form = RE::TESForm::LookupByID(g_trackedPrimary);
            auto* prev = form ? form->As<RE::Actor>() : nullptr;
            if (prev && prev != cur && !prev->IsDead() && prev->IsPlayerTeammate() && ExtraSlotOf(prev) < 0) {
                const auto slot = FirstFreeExtraSlot();
                if (slot >= 0) {
                    FillAlias(g_extraAliases[slot], prev);
                } else {
                    prev->GetActorRuntimeData().boolBits.reset(RE::Actor::BOOL_BITS::kPlayerTeammate);
                    logger::info("release: {:08X} displaced with no free slot", prev->GetFormID());
                }
                prev->EvaluatePackage();
            }
        }

        g_trackedPrimary = curID;
        SyncState();
    }

    [[noreturn]] void MessageAndExit(const char* msg) {
        MessageBoxA(nullptr, msg, "SimpleFollowerFramework.dll", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        ExitProcess(1);
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
                if (e->opening)
                    OnFollowerActivated(CurrentDialogueSpeaker());
                else if (auto* task = SKSE::GetTaskInterface())
                    task->AddTask([]() { ReconcileVanillaAlias(); });
                else
                    ReconcileVanillaAlias();
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
            if (!player || activator != player || !activated) return RE::BSEventNotifyControl::kContinue;

            auto* actor = activated->As<RE::Actor>();
            if (!actor || actor == player) return RE::BSEventNotifyControl::kContinue;

            OnFollowerActivated(actor);
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

    class FastTravelSink final : public RE::BSTEventSink<RE::TESFastTravelEndEvent> {
    public:
        static FastTravelSink* GetSingleton() {
            static FastTravelSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*,
                                              RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override {
            if (auto* task = SKSE::GetTaskInterface())
                task->AddTask([]() { GatherFollowersAfterTravel(); });
            else
                GatherFollowersAfterTravel();
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Install() {
        if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESActivateEvent>(ActivateSink::GetSingleton());
            events->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
            events->AddEventSink<RE::TESFastTravelEndEvent>(FastTravelSink::GetSingleton());
        }
    }

    void ResetCaches() {
        g_sffQuest = nullptr;
        g_dialogueFollower = nullptr;
        g_vanillaAlias = nullptr;
        g_mirrorAlias = nullptr;
        g_extraAliases.fill(nullptr);
        g_playerFollowerCount = nullptr;
        g_sffCanRecruitMore = nullptr;
        g_sffCurrentFollowerCount = nullptr;
        g_sffFollowerSandbox = nullptr;
        g_friendlyFireSpell = nullptr;
        g_potentialFollower = nullptr;
        g_trackedPrimary = 0;
        g_perkCache.fill(nullptr);
        g_perkCacheValid = false;
        g_essOrig.clear();
        g_crossfireGranted.clear();
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg) {
        if (!msg) return;

        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            if (!RE::TESForm::LookupByEditorID("SFF_FollowerQuest")) {
                MessageAndExit(
                    "Simple Follower Framework.esp is missing or not active.\n\n"
                    "Enable it in your load order, then relaunch.");
            }
            Install();
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            ResetCaches();
            SFF_Settings::Load(true);
            if (auto* sff = GetSFFQuest(); sff && !sff->IsRunning()) sff->Start();
            if (EnsureAliases()) {
                if (auto* primary = PrimaryFollower()) g_trackedPrimary = primary->GetFormID();
            }
            RecoverLegacyAliases();
            SyncState();
            ApplyFriendlyFire();
            ApplySandbox();
            break;

        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SetupLog();
    SKSE::Init(skse);

    SFF_Settings::Load();

    SFF_Settings::ApplyGateCallback = []() { SyncState(); };
    SFF_Settings::FriendlyFireCallback = []() { ApplyFriendlyFire(); };
    SFF_Settings::SandboxCallback = []() { ApplySandbox(); };
    SFF_Settings::CrossfireCallback = []() { SyncState(); };

    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);

    SFF_UI::Register();

    return true;
}
