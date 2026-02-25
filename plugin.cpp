#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace {
    RE::TESGlobal* g_playerFollowerCount = nullptr;

    void ForceFollowerCountZero() {
        if (!g_playerFollowerCount) {
            auto* form = RE::TESForm::LookupByEditorID("PlayerFollowerCount");
            if (form) {
                g_playerFollowerCount = form->As<RE::TESGlobal>();
            }
        }

        if (g_playerFollowerCount) {
            g_playerFollowerCount->value = 0.0f;
        }
    }

    class DialogueMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static DialogueMenuSink* GetSingleton() {
            static DialogueMenuSink singleton;
            return std::addressof(singleton);
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }

            if (a_event->opening && a_event->menuName == "Dialogue Menu") {
                ForceFollowerCountZero();
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Install() {
        auto* ui = RE::UI::GetSingleton();
        if (ui) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(DialogueMenuSink::GetSingleton());
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }

        if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
            ForceFollowerCountZero();
            Install();
        } else if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame) {
            ForceFollowerCountZero();
        } else if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
            ForceFollowerCountZero();
        }
    }
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);

    auto* messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(OnMessage);
    }

    return true;
}