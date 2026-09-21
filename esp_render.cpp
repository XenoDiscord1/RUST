#include "context_manager.h"
#include "font_atlas_manager.h"
#include "font_safe_frame.h"
#include "imgui_impl_dx11.h"

void ESPRenderFrame(ID3D11DeviceContext* deviceCtx, ID3D11RenderTargetView* rtv) {
    auto& mgr    = ImGuiContextManager::Get();
    auto& atlMgr = FontAtlasManager::Get();

    atlMgr.ProcessRebuildIfNeeded(mgr.espCtx);

    SafeFrameScope frame(mgr.espCtx, mgr.ctxLock);
    if (!frame.IsValid()) return;

    ImGui_ImplDX11_NewFrame();

    ImGui::Begin("ESP");
    ImGui::Text("ESP — ATLAS SAFE");
    ImGui::End();

    ImGui::Render();

    deviceCtx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
