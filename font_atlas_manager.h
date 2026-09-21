#pragma once
#include "imgui.h"
#include <mutex>
#include <atomic>

class FontAtlasManager {
public:
    static FontAtlasManager& Get() {
        static FontAtlasManager instance;
        return instance;
    }

    std::mutex atlasMutex;
    std::atomic<bool> rebuildPending{false};
    std::atomic<bool> atlasReady{false};

    void RequestRebuild() {
        rebuildPending.store(true);
    }

    bool ProcessRebuildIfNeeded(ImGuiContext* ctx) {
        if (!rebuildPending.load()) return false;

        std::lock_guard<std::mutex> lock(atlasMutex);

        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();

        atlasReady.store(false);
        io.Fonts->Build();
        atlasReady.store(true);
        rebuildPending.store(false);

        return true;
    }

    bool IsReady() const {
        return atlasReady.load();
    }

private:
    FontAtlasManager() = default;
};
