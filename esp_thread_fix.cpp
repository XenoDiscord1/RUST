#include "context_manager.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"

void ESPRenderFrame() {
    auto ctx = ImGuiContextManager::Get().UseESP();

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("ESP");
    ImGui::Text("ESP THREAD — CONTEXT LOCKED");
    ImGui::End();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
