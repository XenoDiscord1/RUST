#include "context_manager.h"
#include "font_atlas_manager.h"
#include "imgui.h"

void InitFonts(const char* fontPath, float fontSize) {
    auto& mgr    = ImGuiContextManager::Get();
    auto& atlMgr = FontAtlasManager::Get();

    std::lock_guard<std::mutex> lock(mgr.ctxLock);

    ImGui::SetCurrentContext(mgr.guiCtx);
    ImGuiIO& guiIO = ImGui::GetIO();
    guiIO.Fonts->Clear();

    if (fontPath) {
        guiIO.Fonts->AddFontFromFileTTF(fontPath, fontSize);
    } else {
        guiIO.Fonts->AddFontDefault();
    }

    guiIO.Fonts->Build();

    ImGui::SetCurrentContext(mgr.espCtx);
    ImGuiIO& espIO = ImGui::GetIO();
    espIO.Fonts->Clear();

    if (fontPath) {
        espIO.Fonts->AddFontFromFileTTF(fontPath, fontSize);
    } else {
        espIO.Fonts->AddFontDefault();
    }

    espIO.Fonts->Build();

    atlMgr.atlasReady.store(true);
    atlMgr.rebuildPending.store(false);
}

void RequestFontRebuild() {
    FontAtlasManager::Get().RequestRebuild();
}
