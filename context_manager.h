#pragma once
#include "imgui.h"
#include <mutex>

class ImGuiContextManager {
public:
    static ImGuiContextManager& Get() {
        static ImGuiContextManager instance;
        return instance;
    }

    ImGuiContext* guiCtx  = nullptr;
    ImGuiContext* espCtx  = nullptr;
    std::mutex    ctxLock;

    struct ScopedContext {
        std::lock_guard<std::mutex> lock;
        ScopedContext(ImGuiContext* ctx, std::mutex& mtx)
            : lock(mtx)
        {
            ImGui::SetCurrentContext(ctx);
        }
    };

    ScopedContext UseGUI() { return ScopedContext(guiCtx, ctxLock); }
    ScopedContext UseESP() { return ScopedContext(espCtx, ctxLock); }

private:
    ImGuiContextManager() = default;
};
