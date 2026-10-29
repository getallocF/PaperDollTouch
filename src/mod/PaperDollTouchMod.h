#pragma once

#include <array>
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

// Screen-space slot rectangle detected from UIControl / InventoryItemRenderer (zero heap allocation)
struct DetectedSlotRect {
    float x{0.0f};
    float y{0.0f};
    float w{0.0f};
    float h{0.0f};
};

// Minecraft Bedrock ActorRenderData layout prefix (64-bit ARM64, STL-free POD prefix)
struct McActorRenderDataPrefix {
    void *mActor{nullptr};           // +0x00 (Actor*)
    float posX{0.0f};                // +0x08 (Vec3 mPosition.x)
    float posY{0.0f};                // +0x0C (Vec3 mPosition.y)
    float posZ{0.0f};                // +0x10 (Vec3 mPosition.z)
    float rotPitch{0.0f};            // +0x14 (Vec2 mRotation.x = pitch)
    float rotYaw{0.0f};              // +0x18 (Vec2 mRotation.y = bodyYaw)
    float headRotPitch{0.0f};        // +0x1C (Vec2 mHeadRot.x = headPitch)
    float headRotYaw{0.0f};          // +0x20 (Vec2 mHeadRot.y = headYaw)
    bool glint{false};               // +0x24
    bool mIgnoreLighting{false};     // +0x25
    bool mIsInUI{false};             // +0x26
};

// Lock-free atomic runtime state shared between Input Thread and RenderDragon UI/EGL Thread
struct RuntimeTouchState {
    std::atomic<bool> modActive{false};
    std::atomic<bool> hookInstalled{false};
    std::atomic<bool> guiScreenOpen{false};
    std::atomic<bool> inventoryPreviewVisible{false};
    std::atomic<bool> inInventoryRenderPass{false};
    std::atomic<bool> bodyRotGetterCalledThisPass{false};
    std::atomic<bool> headRotGetterCalledThisPass{false};
    std::atomic<bool> physicalMouseActive{false};
    std::atomic<bool> settingsMenuOpen{false};

    // Saved ActorRotationComponent pointer & values during active UI render pass
    float *activeActorRotCompFloats{nullptr};
    float savedActorRotCompFloats[4]{0.0f, 0.0f, 0.0f, 0.0f};

    // Monotonic timestamps (ms) for GUI lifecycle & PaperDoll visibility detection
    std::atomic<std::int64_t> lastGuiRenderMs{0};
    std::atomic<std::int64_t> lastInventoryRenderMs{0};
    std::atomic<std::int64_t> currentSlotFrameBucketMs{0};
    std::atomic<int> slotsRenderedInCurrentFrame{0};

    // Fixed-capacity slot cache populated during InventoryItemRenderer::render (zero per-frame heap allocation)
    static constexpr std::size_t kMaxTrackedSlots = 128;
    std::array<DetectedSlotRect, kMaxTrackedSlots> frameSlots{};
    std::atomic<std::size_t> frameSlotCount{0};

    // Active Green Slot Hover box in physical screen pixels
    std::atomic<bool> hasHoveredSlot{false};
    std::atomic<float> hoveredSlotX{0.0f};
    std::atomic<float> hoveredSlotY{0.0f};
    std::atomic<float> hoveredSlotW{0.0f};
    std::atomic<float> hoveredSlotH{0.0f};

    // Deduplication state between pl::input callback and AInputQueue_getEvent hook
    std::atomic<std::int64_t> lastTouchEventMs{0};
    std::atomic<int> lastTouchAction{-1};
    std::atomic<float> lastRawEventX{-1.0f};
    std::atomic<float> lastRawEventY{-1.0f};

    // Active touch pointer ID and persistent Windows-style Mobile Cursor coordinates
    std::atomic<std::int32_t> activePointerId{-1};
    std::atomic<bool> cursorInitializedInGui{false};
    std::atomic<float> cursorX{960.0f};
    std::atomic<float> cursorY{540.0f};
    std::atomic<float> lastTouchX{0.0f};
    std::atomic<float> lastTouchY{0.0f};

    // Runtime-adjustable Mobile Cursor Settings (persisted to config.json)
    std::atomic<bool> cursorEnabled{true};
    std::atomic<float> cursorScale{1.00f}; // [0.75, 2.00]
    std::atomic<bool> greenSlotHoverEnabled{true};
    std::atomic<bool> paperDollFollowCursor{true};
    std::atomic<bool> trackpadMode{false};

    // Calibrated physical screen dimensions (pixels) - updated via eglQuerySurface & touch calibration
    std::atomic<float> screenWidth{1920.0f};
    std::atomic<float> screenHeight{1080.0f};

    // Normalized [0.0, 1.0] active touch bounds
    std::atomic<float> activeBoundsMinX{0.0f};
    std::atomic<float> activeBoundsMinY{0.0f};
    std::atomic<float> activeBoundsMaxX{1.0f};
    std::atomic<float> activeBoundsMaxY{1.0f};

    // Cursor-Follow target and smoothed orientation deltas (degrees)
    std::atomic<float> targetBodyYawOffset{0.0f};
    std::atomic<float> targetHeadYawOffset{0.0f};
    std::atomic<float> targetPitchOffset{0.0f};
    std::atomic<float> currentBodyYawOffset{0.0f};
    std::atomic<float> currentHeadYawOffset{0.0f};
    std::atomic<float> currentPitchOffset{0.0f};

    // Diagnostic counter of rendered GUI & preview frames
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

    // Non-consuming touch handler (consumes ONLY when tapping inside Mobile Cursor Settings popup)
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

    // Detour for GradientRenderer::render (renders modal GUI background dim on Inventory, Chest, Pause, Settings, etc.)
    void onGradientRenderDetour(
        void *self,
        void *renderContext,
        void *clientInstance,
        void *uiControl,
        int pass
    ) noexcept;

    // Detour for InventoryItemRenderer::render (tracks active container/inventory slots & GUI lifecycle)
    void onInventoryItemRenderDetour(
        void *self,
        void *renderContext,
        void *clientInstance,
        void *uiControl,
        int pass
    ) noexcept;

    // Detour for HoverRenderer::render (tracks active container slot hover)
    void onHoverRenderDetour(
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

    // Detours for Mob::getInterpolatedBodyRot / Actor::getInterpolatedHeadRot / Mob::getInterpolatedBodyYaw
    float onMobGetInterpolatedBodyRotDetour(const void *self, float partialTicks) noexcept;
    float onMobGetInterpolatedHeadRotDetour(const void *self, float partialTicks) noexcept;
    float onMobGetInterpolatedBodyYawDetour(const void *self, float partialTicks) noexcept;

    // Native AInputQueue_getEvent detour
    int32_t onInputQueueGetEventDetour(void *queue, void **outEvent) noexcept;

    // Native eglSwapBuffers detour for zero-shader hardware-crisp Windows Cursor, Green Slot Hover & Settings UI
    unsigned int onEglSwapBuffersDetour(void *display, void *surface) noexcept;

  private:
    void sanitizeAndApplyConfig() noexcept;
    void saveRuntimeSettingsToConfig() noexcept;
    bool tryResolveAndInstallHooks() noexcept;
    void registerInputHandlers() noexcept;
    void removeHooks() noexcept;
    void resetTouchAndOrientation(bool clearAngles) noexcept;
    void updateCursorFollowTargets(float x, float y) noexcept;
    void updateHoveredSlotFromCursor(float cx, float cy) noexcept;
    bool handleSettingsOverlayTouch(int maskedAction, float x, float y) noexcept;
    void renderMobileCursorOverlay(int surfaceWidth, int surfaceHeight) noexcept;
    void stepOrientationSmoothing() noexcept;
    void applyActorPitchToComponents(const void *actorPtr) noexcept;
    void restoreActorPitchComponents() noexcept;
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

    void *mGradientRenderTarget{nullptr};
    void *mGradientRenderOriginal{nullptr};

    void *mInventoryItemRenderTarget{nullptr};
    void *mInventoryItemRenderOriginal{nullptr};

    void *mHoverRenderTarget{nullptr};
    void *mHoverRenderOriginal{nullptr};

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
    void *mEglSwapBuffersTarget{nullptr};
    void *mEglSwapBuffersOriginal{nullptr};
    bool mPlInputCallbackRegistered{false};
    bool mInputQueueHookInstalled{false};
    bool mEglHookInstalled{false};
};

} // namespace paperdoll
