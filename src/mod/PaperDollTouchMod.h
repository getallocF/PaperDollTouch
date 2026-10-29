#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "mod/Config.h"
#include <pl/Mod.hpp>

namespace paperdoll {

// Minecraft Bedrock Vec2 layout (pitch, yaw in degrees)
struct McVec2 {
    float x{0.0f};
    float y{0.0f};
};

// Minecraft Bedrock ActorRenderData layout prefix (64-bit ARM64, STL-free POD prefix)
struct McActorRenderDataPrefix {
    void *mActor{nullptr};           // +0x00
    const void *mItemStack{nullptr}; // +0x08
    float posX{0.0f};                // +0x10
    float posY{0.0f};                // +0x14
    float posZ{0.0f};                // +0x18
    float rotPitch{0.0f};            // +0x1C (rotation.x)
    float rotYaw{0.0f};              // +0x20 (rotation.y)
    float headRotPitch{0.0f};        // +0x24 (mHeadRot.x)
    float headRotYaw{0.0f};          // +0x28 (mHeadRot.y)
    bool glint{false};               // +0x2C
    bool mIgnoreLighting{false};     // +0x2D
    bool mIsInUI{false};             // +0x2E
};

// Lock-free atomic runtime state shared between Input Thread and RenderDragon UI Thread
struct RuntimeTouchState {
    std::atomic<bool> modActive{false};
    std::atomic<bool> hookInstalled{false};
    std::atomic<bool> inventoryPreviewVisible{false};
    std::atomic<bool> inInventoryRenderPass{false};

    // Monotonic timestamp (ms) of the most recent Inventory LivePlayerRenderer / PaperDollRenderer render call
    std::atomic<std::int64_t> lastInventoryRenderMs{0};

    // Deduplication state between pl::input callback and AInputQueue_getEvent hook
    std::atomic<std::int64_t> lastTouchEventMs{0};
    std::atomic<int> lastTouchAction{-1};
    std::atomic<float> lastRawEventX{-1.0f};
    std::atomic<float> lastRawEventY{-1.0f};

    // Active touch pointer ID and virtual cursor coordinates
    std::atomic<std::int32_t> activePointerId{-1};
    std::atomic<float> lastTouchX{0.0f};
    std::atomic<float> lastTouchY{0.0f};

    // Calibrated physical screen dimensions (pixels) - starts at 1920x1080 and self-calibrates
    std::atomic<float> screenWidth{1920.0f};
    std::atomic<float> screenHeight{1080.0f};

    // Normalized [0.0, 1.0] active touch bounds
    std::atomic<float> activeBoundsMinX{0.0f};
    std::atomic<float> activeBoundsMinY{0.0f};
    std::atomic<float> activeBoundsMaxX{1.0f};
    std::atomic<float> activeBoundsMaxY{1.0f};

    // Cursor-Follow target and smoothed orientation deltas (degrees)
    // Decouples Body Yaw (~26 deg) from Head Yaw (~54 deg) and Head Pitch (~34 deg)
    // so the character's head and eyes track the finger like a PC mouse cursor.
    std::atomic<float> targetBodyYawOffset{0.0f};
    std::atomic<float> targetHeadYawOffset{0.0f};
    std::atomic<float> targetPitchOffset{0.0f};
    std::atomic<float> currentBodyYawOffset{0.0f};
    std::atomic<float> currentHeadYawOffset{0.0f};
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

    // Detour for LivePlayerRenderer::render(MinecraftUIRenderContext&, IClientInstance&, UIControl&, int)
    void onLivePlayerRenderDetour(
        void *self,
        void *renderContext,
        void *clientInstance,
        void *uiControl,
        int pass
    ) noexcept;

    // Detour for PaperDollRenderer::render(MinecraftUIRenderContext&, IClientInstance&, UIControl&, int)
    void onPaperDollRenderDetour(
        void *self,
        void *renderContext,
        void *clientInstance,
        void *uiControl,
        int pass
    ) noexcept;

    // Detour for PaperDollBaseActorRenderContext::renderGui(HashedString const&, ActorRenderData&, OffscreenCaptureDescription const*)
    void onPaperDollRenderGuiDetour(
        void *self,
        const void *rendererName,
        McActorRenderDataPrefix *actorRenderData,
        const void *captureDesc
    ) noexcept;

    // Detour for DataDrivenRenderer::render(BaseActorRenderContext&, ActorRenderData&)
    void onDataDrivenRenderDetour(
        void *self,
        void *baseActorRenderContext,
        McActorRenderDataPrefix *actorRenderData
    ) noexcept;

    // Detours for Mob::getInterpolatedBodyRot / getInterpolatedHeadRot / getInterpolatedBodyYaw
    float onMobGetInterpolatedBodyRotDetour(const void *self, float partialTicks) noexcept;
    float onMobGetInterpolatedHeadRotDetour(const void *self, float partialTicks) noexcept;
    float onMobGetInterpolatedBodyYawDetour(const void *self, float partialTicks) noexcept;

    // Native AInputQueue_getEvent detour
    int32_t onInputQueueGetEventDetour(void *queue, void **outEvent) noexcept;

  private:
    void sanitizeAndApplyConfig() noexcept;
    bool tryResolveAndInstallHooks() noexcept;
    void registerInputHandlers() noexcept;
    void removeHooks() noexcept;
    void resetTouchAndOrientation(bool clearAngles) noexcept;
    void updateCursorFollowTargets(float x, float y) noexcept;
    void stepOrientationSmoothing() noexcept;
    bool isPointInsidePreviewBounds(float x, float y) const noexcept;
    void writeDiagnosticProbeReport(
        bool hookReady,
        const std::string &liveRenderSource,
        const std::string &paperDollSource,
        const std::string &dataDrivenSource,
        const std::string &mobInterpSource
    ) const noexcept;

    static std::int64_t monotonicMillis() noexcept;

    ll::mod::NativeMod &mSelf;
    ModConfig mConfig{};
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile{};
    RuntimeTouchState mState{};

    void *mLivePlayerRenderTarget{nullptr};
    void *mLivePlayerRenderOriginal{nullptr};

    void *mPaperDollRenderTarget{nullptr};
    void *mPaperDollRenderOriginal{nullptr};

    void *mPaperDollRenderGuiTarget{nullptr};
    void *mPaperDollRenderGuiOriginal{nullptr};

    void *mDataDrivenRenderTarget{nullptr};
    void *mDataDrivenRenderOriginal{nullptr};

    void *mMobBodyRotTarget{nullptr};
    void *mMobBodyRotOriginal{nullptr};
    void *mMobHeadRotTarget{nullptr};
    void *mMobHeadRotOriginal{nullptr};
    void *mMobBodyYawTarget{nullptr};
    void *mMobBodyYawOriginal{nullptr};

    void *mInputQueueGetEventTarget{nullptr};
    void *mInputQueueGetEventOriginal{nullptr};
    bool mPlInputCallbackRegistered{false};
    bool mInputQueueHookInstalled{false};
};

} // namespace paperdoll
