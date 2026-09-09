#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "SFF_Settings.h"
#include "SFF_UI.h"

namespace {

    constexpr RE::FormID kDialogueFollowerID = 0x000750BA;
    constexpr std::uint32_t kVanillaFollowerAlias = 0;
    constexpr std::size_t kSlotCount = 8;
    constexpr std::int32_t kBaseFollowers = 1;
    constexpr std::int32_t kMaxExtras = static_cast<std::int32_t>(kSlotCount) - 1;
    constexpr float kLeftBehindDistance = 2500.0f;

    constexpr std::uint32_t kSerializationID = 'SFFW';
    constexpr std::uint32_t kRosterRecord = 'ROST';
    constexpr std::uint32_t kRosterVersion = 2;
    constexpr std::uint32_t kLegacyRosterVersion = 1;

    RE::TESQuest* g_sffQuest = nullptr;
    RE::TESQuest* g_dialogueFollower = nullptr;
    RE::BGSRefAlias* g_vanillaAlias = nullptr;
    std::array<RE::BGSRefAlias*, kSlotCount> g_slots{};
    std::array<RE::FormID, kSlotCount> g_slotRoster{};

    RE::ObjectRefHandle g_dialogueTarget;
    std::recursive_mutex g_stateLock;

    RE::TESGlobal* g_playerFollowerCount = nullptr;
    RE::TESGlobal* g_sffFollowerSandbox = nullptr;
    RE::SpellItem* g_friendlyFireSpell = nullptr;
    RE::TESFaction* g_potentialFollower = nullptr;
    RE::TESFaction* g_potentialHireling = nullptr;
    RE::TESFaction* g_dismissedFollower = nullptr;
    RE::TESFaction* g_currentFollower = nullptr;
    RE::TESFaction* g_playerFollower = nullptr;
    RE::TESFaction* g_wiFollowerComment = nullptr;
    RE::TESFaction* g_sffFollowerFaction = nullptr;

    std::array<RE::BGSPerk*, SFF_Settings::kMaxPerkSpecs> g_perkCache{};
    std::uint32_t g_perkCacheGeneration = 0;
    bool g_perkCacheValid = false;

    std::unordered_map<RE::FormID, std::uint8_t> g_essOrig{};

    void SetupLog() {
        auto folder = SKSE::log::log_directory();
        if (!folder) return;
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*folder / "SimpleFollowerFramework.log").string(), true);
        auto log = std::make_shared<spdlog::logger>("log", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] %v");
    }

    template <class T> T* LookupCached(T*& cache, std::string_view editorID) {
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
        if (g_vanillaAlias && g_slots[0]) return true;

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
            if (base && base->aliasID < kSlotCount) g_slots[base->aliasID] = static_cast<RE::BGSRefAlias*>(base);
        }
        return g_vanillaAlias && g_slots[0];
    }

    RE::Actor* AliasActor(RE::BGSRefAlias* alias) { return alias ? alias->GetActorReference() : nullptr; }

    void FillAlias(RE::BGSRefAlias* alias, RE::Actor* actor) {
        if (!alias || !actor) return;
        using func_t = void (*)(RE::BSScript::Internal::VirtualMachine*, std::uint32_t, RE::BGSRefAlias*, RE::TESObjectREFR*);
        static REL::Relocation<func_t> forceFn{ RELOCATION_ID(54634, 55288) };
        forceFn(RE::BSScript::Internal::VirtualMachine::GetSingleton(), 0, alias, actor);
    }

    void ClearAlias(RE::BGSRefAlias* alias) {
        if (!alias) return;
        using func_t = void (*)(RE::BSScript::Internal::VirtualMachine*, std::uint32_t, RE::BGSRefAlias*);
        static REL::Relocation<func_t> clearFn{ RELOCATION_ID(54632, 55286) };
        clearFn(RE::BSScript::Internal::VirtualMachine::GetSingleton(), 0, alias);
    }

    std::int32_t SlotOf(RE::Actor* actor) {
        if (!actor) return -1;
        for (std::size_t i = 0; i < kSlotCount; ++i) {
            if (AliasActor(g_slots[i]) == actor) return static_cast<std::int32_t>(i);
        }
        return -1;
    }

    bool VanillaDismissed(RE::Actor* a) {
        auto* fac = LookupCached(g_dismissedFollower, "DismissedFollowerFaction"sv);
        return a && fac && a->IsInFaction(fac);
    }

    bool Recruitable(RE::Actor* a) {
        if (!a) return false;
        auto* potential = LookupCached(g_potentialFollower, "PotentialFollowerFaction"sv);
        auto* hireling = LookupCached(g_potentialHireling, "PotentialHireling"sv);
        return (potential && a->IsInFaction(potential)) || (hireling && a->IsInFaction(hireling));
    }

    std::int32_t FirstFreeSlot() {
        for (std::size_t i = 0; i < kSlotCount; ++i) {
            auto* a = AliasActor(g_slots[i]);
            if (!a || a->IsDead() || VanillaDismissed(a)) return static_cast<std::int32_t>(i);
        }
        return -1;
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

    void SetFollowerFactions(RE::Actor* a, bool want) {
        if (!a) return;
        RE::TESFaction* factions[3] = { LookupCached(g_currentFollower, "CurrentFollowerFaction"sv), LookupCached(g_playerFollower, "PlayerFollowerFaction"sv), LookupCached(g_wiFollowerComment, "WIFollowerCommentFaction"sv) };
        for (auto* fac : factions) {
            if (!fac) continue;
            const bool has = a->IsInFaction(fac);
            if (want && !has)
                a->AddToFaction(fac, 0);
            else if (!want && has)
                a->RemoveFromFaction(fac);
        }
    }

    void ReleaseAllEssential() {
        for (const auto& entry : g_essOrig) {
            auto* form = RE::TESForm::LookupByID(entry.first);
            RestoreEssentialBase(form ? form->As<RE::TESNPC>() : nullptr, entry.second);
        }
        g_essOrig.clear();
    }

    RE::SpellItem* GetFriendlyFireSpell() { return LookupCached(g_friendlyFireSpell, "IvyCompanionsSafeSpell"sv); }

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
        if (want && !a->HasSpell(spell))
            a->AddSpell(spell);
        else if (!want && a->HasSpell(spell))
            a->RemoveSpell(spell);
    }

    RE::Actor* ResolveActorForm(RE::FormID id) {
        if (!id) return nullptr;
        auto* form = RE::TESForm::LookupByID(id);
        return form ? form->As<RE::Actor>() : nullptr;
    }

    void SetVanillaDialogueHidden(RE::Actor* a, bool hidden) {
        auto* fac = LookupCached(g_sffFollowerFaction, "SFF_FollowerFaction"sv);
        if (!a || !fac) return;
        const bool tagged = a->IsInFaction(fac);
        if (hidden && !tagged)
            a->AddToFaction(fac, 0);
        else if (!hidden && tagged)
            a->RemoveFromFaction(fac);
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

        const auto speech = static_cast<std::int32_t>(player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech));
        const std::int32_t levelsPerSlot = (SFF_Settings::SpeechLevelsPerSlot > 0) ? SFF_Settings::SpeechLevelsPerSlot : 10;
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

    RE::Actor* ActiveDialogueSpeaker() {
        if (auto ref = g_dialogueTarget.get()) return ref->As<RE::Actor>();
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) return nullptr;
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        if (!mtm) return nullptr;
        auto ref = mtm->speaker.get();
        return ref ? ref->As<RE::Actor>() : nullptr;
    }

    void ReleaseRetiredSlots() {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        if (!EnsureAliases()) return;

        for (std::size_t i = 0; i < kSlotCount; ++i) {
            auto* a = AliasActor(g_slots[i]);
            if (!a || (!a->IsDead() && !VanillaDismissed(a))) continue;

            ClearAlias(g_slots[i]);
            g_slotRoster[i] = 0;
            if (AliasActor(g_vanillaAlias) == a) ClearAlias(g_vanillaAlias);
            SetFollowerFactions(a, false);
            UpdateEssentialForActor(a, false);
            ApplyCrossfireForActor(a, false);
            SetVanillaDialogueHidden(a, false);
            a->EvaluatePackage();
        }
    }

    void SyncState() {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        if (!EnsureAliases()) return;
        if (g_sffQuest && !g_sffQuest->IsRunning()) g_sffQuest->Start();

        ReleaseRetiredSlots();

        auto* borrowed = AliasActor(g_vanillaAlias);
        const bool wantCrossfire = SFF_Settings::FollowerCrossfire;
        std::int32_t count = 0;

        for (std::size_t i = 0; i < kSlotCount; ++i) {
            auto* a = AliasActor(g_slots[i]);
            if (!a) {
                g_slotRoster[i] = 0;
                continue;
            }

            ++count;
            g_slotRoster[i] = a->GetFormID();
            SetFollowerFactions(a, true);
            UpdateEssentialForActor(a, true);
            ApplyCrossfireForActor(a, wantCrossfire);
            SetVanillaDialogueHidden(a, a != borrowed);
        }

        auto* speaker = ActiveDialogueSpeaker();
        const bool speakerFollows = speaker && (speaker->IsPlayerTeammate() || SlotOf(speaker) >= 0);
        const bool offerHire = (count < GetTotalFollowerCap()) && !speakerFollows && Recruitable(speaker);

        if (auto* glob = LookupCached(g_playerFollowerCount, "PlayerFollowerCount"sv)) {
            glob->value = offerHire ? 0.0f : ((count > 0) ? 1.0f : 0.0f);
        }
    }

    bool AdoptOrphan(RE::Actor* actor) {
        if (!actor || actor == RE::PlayerCharacter::GetSingleton()) return false;
        if (actor->IsDead() || !actor->IsPlayerTeammate() || SlotOf(actor) >= 0) return false;
        auto* potential = LookupCached(g_potentialFollower, "PotentialFollowerFaction"sv);
        if (!potential || !actor->IsInFaction(potential)) return false;
        if (VanillaDismissed(actor)) return false;

        const auto slot = FirstFreeSlot();
        if (slot < 0) return false;

        FillAlias(g_slots[slot], actor);
        g_slotRoster[slot] = actor->GetFormID();
        actor->EvaluatePackage();
        return true;
    }

    void AbsorbVanillaAlias() {
        auto* occupant = AliasActor(g_vanillaAlias);
        if (!occupant || occupant->IsDead()) return;
        if (!occupant->IsPlayerTeammate() || SlotOf(occupant) >= 0) return;

        if (VanillaDismissed(occupant)) return;

        const auto slot = FirstFreeSlot();
        if (slot < 0) return;

        FillAlias(g_slots[slot], occupant);
        g_slotRoster[slot] = occupant->GetFormID();
        occupant->EvaluatePackage();
    }

    void OnFollowerActivated(RE::Actor* actor) {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        if (!actor || !EnsureAliases()) return;
        g_dialogueTarget = actor->GetHandle();

        ReleaseRetiredSlots();
        AbsorbVanillaAlias();
        AdoptOrphan(actor);
        if (SlotOf(actor) >= 0 && !VanillaDismissed(actor)) {
            if (AliasActor(g_vanillaAlias) != actor) {
                FillAlias(g_vanillaAlias, actor);
                actor->EvaluatePackage();
            }
        } else {
            SetVanillaDialogueHidden(actor, false);
            auto* held = AliasActor(g_vanillaAlias);
            if (held && (held == actor || VanillaDismissed(held) || (Recruitable(actor) && SlotOf(held) >= 0))) ClearAlias(g_vanillaAlias);
        }
        SyncState();
    }

    void EndDialogue() {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        g_dialogueTarget = {};
        if (!EnsureAliases()) return;

        AbsorbVanillaAlias();
        SyncState();
    }

    void GatherFollowersAfterTravel() {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        if (!EnsureAliases()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        for (std::size_t i = 0; i < kSlotCount; ++i) {
            auto* a = AliasActor(g_slots[i]);
            if (!a || a->IsDead()) continue;
            if (a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) continue;
            if (a->GetParentCell() == player->GetParentCell() && a->GetDistance(player) < kLeftBehindDistance) continue;

            a->MoveTo(player);
            a->EvaluatePackage();
        }
    }

    void CaptureRoster() {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        g_slotRoster.fill(0);
        if (!EnsureAliases()) return;

        for (std::size_t i = 0; i < kSlotCount; ++i) {
            auto* a = AliasActor(g_slots[i]);
            if (a && !a->IsDead() && a->IsPlayerTeammate()) g_slotRoster[i] = a->GetFormID();
        }
    }

    void RestoreRoster() {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        if (!EnsureAliases()) return;

        ClearAlias(g_vanillaAlias);

        for (std::size_t i = 0; i < kSlotCount; ++i) {
            if (AliasActor(g_slots[i])) continue;
            auto* a = ResolveActorForm(g_slotRoster[i]);
            if (!a || a->IsDead() || SlotOf(a) >= 0 || VanillaDismissed(a)) continue;
            FillAlias(g_slots[i], a);
        }

        for (std::size_t i = 0; i < kSlotCount; ++i) {
            auto* a = AliasActor(g_slots[i]);
            if (!a || a->IsDead()) continue;
            if (VanillaDismissed(a)) continue;
            if (!a->IsPlayerTeammate()) a->GetActorRuntimeData().boolBits.set(RE::Actor::BOOL_BITS::kPlayerTeammate);
            a->EvaluatePackage();
        }
    }

    [[noreturn]] void MessageAndExit(const char* msg) {
        MessageBoxA(nullptr, msg, "SimpleFollowerFramework.dll", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        ExitProcess(1);
    }

    void ApplySettings() {
        ApplyFriendlyFire();
        ApplySandbox();
        SyncState();
    }

    void Defer(void (*fn)()) {
        if (auto* task = SKSE::GetTaskInterface())
            task->AddTask(fn);
        else
            fn();
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuSink* GetSingleton() {
            static MenuSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (e && e->menuName == RE::DialogueMenu::MENU_NAME) {
                if (e->opening) {
                    if (!g_dialogueTarget.get()) SyncState();
                } else {
                    Defer(EndDialogue);
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
        RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* e, RE::BSTEventSource<RE::TESActivateEvent>*) override {
            if (!e) return RE::BSEventNotifyControl::kContinue;

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* activated = e->objectActivated.get();
            if (!player || e->actionRef.get() != player || !activated) return RE::BSEventNotifyControl::kContinue;

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
        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* e, RE::BSTEventSource<RE::TESDeathEvent>*) override {
            if (!e || !e->dead) return RE::BSEventNotifyControl::kContinue;
            auto* actor = e->actorDying ? e->actorDying->As<RE::Actor>() : nullptr;
            if (!actor || !actor->IsPlayerTeammate()) return RE::BSEventNotifyControl::kContinue;
            std::lock_guard<std::recursive_mutex> lock(g_stateLock);
            if (SlotOf(actor) < 0) return RE::BSEventNotifyControl::kContinue;
            Defer(SyncState);
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class FastTravelSink final : public RE::BSTEventSink<RE::TESFastTravelEndEvent> {
    public:
        static FastTravelSink* GetSingleton() {
            static FastTravelSink s;
            return &s;
        }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override {
            Defer(GatherFollowersAfterTravel);
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

    void OnGameReady() {
        ApplyFriendlyFire();
        ApplySandbox();

        auto* sff = GetSFFQuest();
        if (sff && !sff->IsRunning()) sff->Start();
        if (!EnsureAliases()) return;

        RestoreRoster();
        SyncState();
    }

    void OnCosaveSave(SKSE::SerializationInterface* intfc) {
        CaptureRoster();
        if (!intfc->OpenRecord(kRosterRecord, kRosterVersion)) return;
        intfc->WriteRecordData(g_slotRoster.data(), static_cast<std::uint32_t>(g_slotRoster.size() * sizeof(RE::FormID)));
    }

    void OnCosaveLoad(SKSE::SerializationInterface* intfc) {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        g_slotRoster.fill(0);

        auto resolve = [intfc](RE::FormID id) -> RE::FormID {
            RE::FormID out = 0;
            return (id && intfc->ResolveFormID(id, out)) ? out : 0;
        };

        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kRosterRecord) continue;

            if (version == kRosterVersion) {
                std::array<RE::FormID, kSlotCount> slots{};
                intfc->ReadRecordData(slots.data(), static_cast<std::uint32_t>(slots.size() * sizeof(RE::FormID)));
                for (std::size_t i = 0; i < kSlotCount; ++i) g_slotRoster[i] = resolve(slots[i]);
            } else if (version == kLegacyRosterVersion) {
                RE::FormID primary = 0;
                std::array<RE::FormID, kSlotCount - 1> extras{};
                intfc->ReadRecordData(&primary, static_cast<std::uint32_t>(sizeof(primary)));
                intfc->ReadRecordData(extras.data(), static_cast<std::uint32_t>(extras.size() * sizeof(RE::FormID)));
                g_slotRoster[0] = resolve(primary);
                for (std::size_t i = 0; i < extras.size(); ++i) g_slotRoster[i + 1] = resolve(extras[i]);
            }
        }
    }

    void OnCosaveRevert(SKSE::SerializationInterface*) {
        std::lock_guard<std::recursive_mutex> lock(g_stateLock);
        ReleaseAllEssential();
        g_slotRoster.fill(0);
        g_dialogueTarget = {};
    }

    void OnMessage(SKSE::MessagingInterface::Message* msg) {
        if (!msg) return;

        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            if (!RE::TESForm::LookupByEditorID("SFF_FollowerQuest")) {
                MessageAndExit("Simple Follower Framework.esp is missing or not active.\n\nEnable it in your load order, then relaunch.");
            }
            Install();
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            SFF_Settings::Load(true);
            Defer(OnGameReady);
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
    SFF_Settings::SettingsChangedCallback = []() { Defer(ApplySettings); };

    if (auto* messaging = SKSE::GetMessagingInterface()) messaging->RegisterListener(OnMessage);

    if (auto* serialization = SKSE::GetSerializationInterface()) {
        serialization->SetUniqueID(kSerializationID);
        serialization->SetSaveCallback(OnCosaveSave);
        serialization->SetLoadCallback(OnCosaveLoad);
        serialization->SetRevertCallback(OnCosaveRevert);
    } else {
        logger::error("no serialization interface: followers will not survive a reload");
    }

    SFF_UI::Register();

    return true;
}
