#include "context_manager.h"
#include "imgui.h"

void InitContexts() {
    auto& mgr = ImGuiContextManager::Get();

    mgr.guiCtx = ImGui::CreateContext();
    mgr.espCtx = ImGui::CreateContext();

    ImGui::SetCurrentContext(mgr.guiCtx);
    ImGuiIO& guiIO = ImGui::GetIO();
    guiIO.DisplaySize = ImVec2(1920, 1080);

    ImGui::SetCurrentContext(mgr.espCtx);
    ImGuiIO& espIO = ImGui::GetIO();
    espIO.DisplaySize = ImVec2(1920, 1080);

    ImGui::SetCurrentContext(nullptr);
}

void ShutdownContexts() {
    auto& mgr = ImGuiContextManager::Get();

    if (mgr.guiCtx) { ImGui::DestroyContext(mgr.guiCtx); mgr.guiCtx = nullptr; }
    if (mgr.espCtx) { ImGui::DestroyContext(mgr.espCtx); mgr.espCtx = nullptr; }
}
