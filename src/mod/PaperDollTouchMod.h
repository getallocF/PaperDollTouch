#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "mod/Config.h"
#include <pl/Mod.hpp>

namespace paperdoll {

// Minecraft Bedrock internal UI RectangleArea layout (passed to HudPlayerRenderer::render)
struct McRectangleArea {
    float x0{0.0f};
    float x1{0.0f};
    float y0{0.0f};
    float y1{0.0f};
};

// Minecraft Bedrock Vec2 layout (returned by Actor::getRotation() const)
struct McVec2 {
    float x{0.0f}; // Pitch (degrees)
    float y{0.0f}; // Yaw (degrees)
};

// Lock-free atomic runtime state shared between Input Thread and RenderDragon UI Thread
struct RuntimeTouchState {
    std::atomic<bool> modActive{false};
    std::atomic<bool> hookInstalled{false};
    std::atomic<bool> inventoryPreviewVisible{false};

    // Monotonic timestamp (ms) of the most recent inventory HudPlayerRenderer::render call
    std::atomic<std::int64_t> lastInventoryRenderMs{0};

    // Active touch pointer ID locked ONLY when ACTION_DOWN starts inside paper doll preview bounds
    std::atomic<std::int32_t> activePointerId{-1};
    std::atomic<float> lastTouchX{0.0f};
    std::atomic<float> lastTouchY{0.0f};

    // Calibrated physical screen dimensions (pixels)
    std::atomic<float> screenWidth{2400.0f};
    std::atomic<float> screenHeight{1080.0f};

    // Normalized [0.0, 1.0] active preview hit-box bounds
    std::atomic<float> activeBoundsMinX{0.24f};
    std::atomic<float> activeBoundsMinY{0.14f};
    std::atomic<float> activeBoundsMaxX{0.44f};
    std::atomic<float> activeBoundsMaxY{0.56f};

    // Target and smoothed orientation deltas (degrees)
    std::atomic<float> targetYawOffset{0.0f};
    std::atomic<float> targetPitchOffset{0.0f};
    std::atomic<float> currentYawOffset{0.0f};
    std::atomic<float> currentPitchOffset{0.0f};

    // Diagnostic counter of rendered inventory preview frames
    std::atomic<std::uint64_t> renderedPreviewFrames{0};
};

class PaperDollTouchMod {
  public:
    static PaperDollTouchMod &instance();

    PaperDollTouchMod();
    ~PaperDollTouchMod() = default;

    PaperDollTouchMod(const PaperDollTouchMod &) = delete;
    PaperDollTouchMod &operator=(const PaperDollTouchMod &) = delete;

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

    // Non-consuming touch handler (always returns false so Minecraft UI slots/buttons work normally)
    bool onTouchEvent(int action, int pointerId, float x, float y) noexcept;

    // Detour handler for HudPlayerRenderer::render on ARM64
    void onHudPlayerRenderDetour(
        void *self,
        void *renderContext,
        void *clientInstance,
        void *uiControl,
        int pass,
        McRectangleArea *rectArea
    ) noexcept;

    // Optional native AInputQueue_getEvent detour fallback
    int32_t onInputQueueGetEventDetour(void *queue, void **outEvent) noexcept;

  private:
    void sanitizeAndApplyConfig() noexcept;
    bool tryResolveAndInstallHooks() noexcept;
    void registerInputHandlers() noexcept;
    void removeHooks() noexcept;
    void resetTouchAndOrientation(bool clearAngles) noexcept;
    bool isPointInsidePreviewBounds(float x, float y) const noexcept;
    void writeDiagnosticProbeReport(
        bool hookReady,
        const std::string &renderSymSource,
        const std::string &localPlayerSymSource,
        const std::string &rotationAccessMode
    ) const noexcept;

    static std::int64_t monotonicMillis() noexcept;

    ll::mod::NativeMod &mSelf;
    ModConfig mConfig{};
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile{};
    RuntimeTouchState mState{};

    void *mHudPlayerRenderTarget{nullptr};
    void *mHudPlayerRenderOriginal{nullptr};
    void *mGetLocalPlayerFn{nullptr};
    void *mActorGetRotationFn{nullptr};
    void *mActorGetYHeadRotFn{nullptr};
    void *mActorSetYHeadRotFn{nullptr};

    void *mInputQueueGetEventTarget{nullptr};
    void *mInputQueueGetEventOriginal{nullptr};
    bool mPlInputCallbackRegistered{false};
    bool mInputQueueHookInstalled{false};
};

} // namespace paperdoll
