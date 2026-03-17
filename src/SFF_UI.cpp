#include "SFF_UI.h"

#include "SFF_Settings.h"

// Copied from SKSE Example File
// If you are wondering what these do check that on SKSE authors GitHub Page

static void BeginDisabled(bool disabled) {
    if (disabled) {
        ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_Alpha, ImGuiMCP::GetStyle()->Alpha * 0.35f);
        ImGuiMCP::PushItemFlag(ImGuiMCP::ImGuiItemFlags_Disabled, true);
    }
}
static void EndDisabled(bool disabled) {
    if (disabled) {
        ImGuiMCP::PopItemFlag();
        ImGuiMCP::PopStyleVar();
    }
}

static void HelpMarker(const char* desc) {
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.55f, 0.55f, 0.55f, 1.0f});
    ImGuiMCP::TextUnformatted("(?)");
    ImGuiMCP::PopStyleColor();
    if (ImGuiMCP::IsItemHovered()) {
        ImGuiMCP::BeginTooltip();
        ImGuiMCP::PushTextWrapPos(ImGuiMCP::GetFontSize() * 28.0f);
        ImGuiMCP::TextUnformatted(desc);
        ImGuiMCP::PopTextWrapPos();
        ImGuiMCP::EndTooltip();
    }
}

static bool StyledRadio(const char* label, const char* desc, bool selected) {
    ImGuiMCP::ImVec4 labelCol =
        selected ? ImGuiMCP::ImVec4{0.85f, 0.72f, 0.40f, 1.0f} : ImGuiMCP::ImVec4{0.90f, 0.90f, 0.90f, 1.0f};
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, labelCol);
    bool hit = ImGuiMCP::RadioButton(label, selected);
    ImGuiMCP::PopStyleColor();
    ImGuiMCP::SameLine();
    ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.50f, 0.50f, 0.50f, 1.0f});
    ImGuiMCP::TextUnformatted(desc);
    ImGuiMCP::PopStyleColor();
    return hit;
}

void SFF_UI::Register() {
    if (!SKSEMenuFramework::IsInstalled()) return;
    SKSEMenuFramework::SetSection("Simple Follower Framework");
    SKSEMenuFramework::AddSectionItem("Settings", SFF_UI::RenderSettings);
}

void __stdcall SFF_UI::RenderSettings() {
    SFF_Settings::Load();
    bool changed = false;

    // Follower Limit Mode
    ImGuiMCP::SetWindowFontScale(0.93f);
    ImGuiMCP::SeparatorText("FOLLOWER LIMIT MODE");

    // Option 0
    if (StyledRadio("Max Followers", "", SFF_Settings::FollowerPerkOption == 0)) {
        SFF_Settings::FollowerPerkOption = 0;
        changed = true;
    }
    {
        bool dis = (SFF_Settings::FollowerPerkOption != 0);
        BeginDisabled(dis);
        ImGuiMCP::Indent(22.0f);
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.55f, 0.55f, 0.55f, 1.0f});
        ImGuiMCP::TextUnformatted("Max Followers");
        ImGuiMCP::PopStyleColor();
        ImGuiMCP::SameLine(160.0f);
        ImGuiMCP::SetNextItemWidth(250.0f);
        int displayMax = SFF_Settings::MaxExtraFollowers + 1;
        if (ImGuiMCP::SliderInt("##iMaxFollowers", &displayMax, 1, 8)) {
            SFF_Settings::MaxExtraFollowers = displayMax - 1;
            changed = true;
        }
        ImGuiMCP::SameLine();
        HelpMarker("Total follower slots.\n1 = vanilla (one follower).\nMaximum is 8.");
        ImGuiMCP::Unindent(22.0f);
        EndDisabled(dis);
    }

    ImGuiMCP::Spacing();

    // Option 1
    if (StyledRadio("Perk Gated", "", SFF_Settings::FollowerPerkOption == 1)) {
        SFF_Settings::FollowerPerkOption = 1;
        changed = true;
    }
    {
        bool dis = (SFF_Settings::FollowerPerkOption != 1);
        BeginDisabled(dis);
        ImGuiMCP::Indent(22.0f);
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.55f, 0.55f, 0.55f, 1.0f});
        ImGuiMCP::TextUnformatted("Perk List");
        ImGuiMCP::PopStyleColor();
        ImGuiMCP::SameLine();
        HelpMarker(
            "Comma-separated list, up to 8 entries.\n"
            "Format: PluginName.esp|FormID\n"
            "Example: Skyrim.esm|00058F75\n"
            "Each perk the player owns grants one extra follower slot.");
        ImGuiMCP::SetNextItemWidth(-1.0f);
        if (ImGuiMCP::InputText("##PerkList", SFF_Settings::PerkListBuffer, sizeof(SFF_Settings::PerkListBuffer))) {
            changed = true;
            SFF_Settings::ParsePerkListIntoSpecs(SFF_Settings::PerkListBuffer);
        }

        // Copied Style code from SKSE Example (i know i know i copy too much but what u want me to do??? this is my first time making something like this)
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, SFF_Settings::PerkSpecCount > 0 ? ImGuiMCP::ImVec4{0.45f, 0.75f, 0.45f, 1.0f} : ImGuiMCP::ImVec4{0.50f, 0.50f, 0.50f, 1.0f});
        ImGuiMCP::Text("  %zu / %zu Perks Parsed", SFF_Settings::PerkSpecCount, SFF_Settings::kMaxPerkSpecs);
        ImGuiMCP::PopStyleColor();
        ImGuiMCP::Unindent(22.0f);
        EndDisabled(dis);
    }

    ImGuiMCP::Spacing();

    // Option 2
    if (StyledRadio("Speech Scaled", "- Scales With Speechcraft Skill", SFF_Settings::FollowerPerkOption == 2)) {
        SFF_Settings::FollowerPerkOption = 2;
        changed = true;
    }
    {
        bool dis = (SFF_Settings::FollowerPerkOption != 2);
        BeginDisabled(dis);
        ImGuiMCP::Indent(22.0f);
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.55f, 0.55f, 0.55f, 1.0f});
        ImGuiMCP::TextUnformatted("Levels Per Slot");
        ImGuiMCP::PopStyleColor();
        ImGuiMCP::SameLine(160.0f);
        ImGuiMCP::SetNextItemWidth(250.0f);
        if (ImGuiMCP::SliderInt("##iSpeechLevelsPerSlot", &SFF_Settings::SpeechLevelsPerSlot, 1, 30)) {
            changed = true;
        }
        ImGuiMCP::SameLine();
        HelpMarker(
            "Speech levels needed for each extra follower slot.\n"
            "Default is 10: Meaning: Speech 0-9 = 1 follower.\n");
        ImGuiMCP::Unindent(22.0f);
        EndDisabled(dis);
    }

    ImGuiMCP::Spacing();

    // Follower Protection
    ImGuiMCP::SeparatorText("FOLLOWER PROTECTION");

    {
        bool ess = SFF_Settings::FollowerEssential;
        if (ImGuiMCP::Checkbox("##EssCheck", &ess)) {
            SFF_Settings::FollowerEssential = ess;
            changed = true;
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ess ? ImGuiMCP::ImVec4{0.85f, 0.72f, 0.40f, 1.0f}
                                                              : ImGuiMCP::ImVec4{0.90f, 0.90f, 0.90f, 1.0f});
        ImGuiMCP::TextUnformatted("Make Followers Essential\n");
        ImGuiMCP::PopStyleColor();
        ImGuiMCP::SameLine();

        HelpMarker(
            "Flags every actor in CurrentFollowerFaction as Essential,\n"
            "so they cannot be killed while following you.\n"
            "Their original Essential or Protected state is fully\n"
            "restored when they leave the party."
        );

        if (ess) {
            ImGuiMCP::Indent(22.0f);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.45f, 0.75f, 0.45f, 1.0f});
            ImGuiMCP::TextUnformatted("Active - Current Followers Cannot Die");
            ImGuiMCP::PopStyleColor();
            ImGuiMCP::Unindent(22.0f);
        }
    }

    ImGuiMCP::Spacing();

    // Friendly Fire Protection
    {
        bool ff = SFF_Settings::FriendlyFire;
        if (ImGuiMCP::Checkbox("##FFCheck", &ff)) {
            SFF_Settings::FriendlyFire = ff;
            changed = true;
            if (SFF_Settings::FriendlyFireCallback) SFF_Settings::FriendlyFireCallback();
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ff ? ImGuiMCP::ImVec4{0.85f, 0.72f, 0.40f, 1.0f}
                                                             : ImGuiMCP::ImVec4{0.90f, 0.90f, 0.90f, 1.0f});
        ImGuiMCP::TextUnformatted("Friendly Fire Protection");
        ImGuiMCP::PopStyleColor();
        ImGuiMCP::SameLine();
        HelpMarker(
            "Your attacks, shouts, and destruction spells do no\n"
            "damage to your followers while in combat.\n"
        );
        if (ff) {
            ImGuiMCP::Indent(22.0f);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.45f, 0.75f, 0.45f, 1.0f});
            ImGuiMCP::TextUnformatted("Active - Followers Are Safe From Friendly Fire While in Combat");
            ImGuiMCP::PopStyleColor();
            ImGuiMCP::Unindent(22.0f);
        }
    }

    ImGuiMCP::Spacing();

    // Follower Sandbox
    {
        bool sb = SFF_Settings::FollowerSandbox;
        if (ImGuiMCP::Checkbox("##SandboxCheck", &sb)) {
            SFF_Settings::FollowerSandbox = sb;
            changed = true;
            if (SFF_Settings::SandboxCallback) SFF_Settings::SandboxCallback();
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, sb ? ImGuiMCP::ImVec4{0.85f, 0.72f, 0.40f, 1.0f}
                                                             : ImGuiMCP::ImVec4{0.90f, 0.90f, 0.90f, 1.0f});
        ImGuiMCP::TextUnformatted("Follower Sandbox");
        ImGuiMCP::PopStyleColor();
        ImGuiMCP::SameLine();

        HelpMarker(
            "Allows followers to sandbox (wander, sit, idle) in Dwellings and Habitation\n"
            "Exmaple: Towns, Homes and anyother places marked as Dwellings and Habitation");

        if (sb) {
            ImGuiMCP::Indent(22.0f);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, ImGuiMCP::ImVec4{0.45f, 0.75f, 0.45f, 1.0f});
            ImGuiMCP::TextUnformatted("Active - Followers Will Sandbox When Idle");
            ImGuiMCP::PopStyleColor();
            ImGuiMCP::Unindent(22.0f);
        }
    }

    ImGuiMCP::Spacing();

    // INI File
    ImGuiMCP::SeparatorText("INI FILE");

    if (ImGuiMCP::Button("Save Settings")) SFF_Settings::Save();
    ImGuiMCP::SameLine();
    HelpMarker(
        "Writes the current values to:\n"
        "Data\\SKSE\\Plugins\\SimpleFollowerFramework.ini\n"
        "Changes are already live in-game. This only saves them.");

    ImGuiMCP::SameLine(0.0f, 14.0f);

    if (ImGuiMCP::Button("Reload Settings")) {
        SFF_Settings::Load(true);
        changed = true;
    }
    ImGuiMCP::SameLine();
    HelpMarker("Discard unsaved UI changes and reload values from the INI file.");
    ImGuiMCP::SetWindowFontScale(1.0f);

    if (changed && SFF_Settings::ApplyGateCallback) SFF_Settings::ApplyGateCallback();
}