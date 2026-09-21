#include "context_manager.h"
#include "font_atlas_manager.h"
#include "font_safe_frame.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

void GUIRenderFrame(ID3D11DeviceContext* deviceCtx, ID3D11RenderTargetView* rtv) {
    auto& mgr     = ImGuiContextManager::Get();
    auto& atlMgr  = FontAtlasManager::Get();

    atlMgr.ProcessRebuildIfNeeded(mgr.guiCtx);

    SafeFrameScope frame(mgr.guiCtx, mgr.ctxLock);
    if (!frame.IsValid()) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGui::Begin("Main Panel");
    ImGui::Text("GUI — ATLAS SAFE");
    ImGui::End();

    ImGui::Render();

    deviceCtx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
