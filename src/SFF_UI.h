#pragma once
#include "SKSEMenuFramework.h"

namespace SFF_UI {
    // Call once from SKSEPlugin_Load (after SKSEMenuFramework is available).
    void Register();

    // ImGui render callback – do not call directly.
    void __stdcall RenderSettings();
}
