#pragma once
#include "imgui.h"
#include "font_atlas_manager.h"
#include <mutex>

class SafeFrameScope {
public:
    SafeFrameScope(ImGuiContext* ctx, std::mutex& ctxMutex)
        : lock_(ctxMutex), ctx_(ctx), valid_(false)
    {
        ImGui::SetCurrentContext(ctx_);

        auto& atlas = FontAtlasManager::Get();

        if (!atlas.IsReady()) {
            return;
        }

        if (atlas.rebuildPending.load()) {
            return;
        }

        ImGuiIO& io = ImGui::GetIO();
        if (!io.Fonts || !io.Fonts->IsBuilt()) {
            return;
        }

        ImGui::NewFrame();
        valid_ = true;
    }

    ~SafeFrameScope() {
        if (valid_) {
            ImGui::EndFrame();
        }
    }

    bool IsValid() const { return valid_; }

private:
    std::lock_guard<std::mutex> lock_;
    ImGuiContext* ctx_;
    bool valid_;
};
