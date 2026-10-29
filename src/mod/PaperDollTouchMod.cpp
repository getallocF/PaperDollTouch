#include "mod/PaperDollTouchMod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fstream>
#include <string_view>

#include <android/input.h>
#include <android/log.h>

#if __has_include(<pl/Input.hpp>)
#include <pl/Input.hpp>
#define PDT_HAS_PL_INPUT 1
#else
#define PDT_HAS_PL_INPUT 0
#endif

#if __has_include(<pl/memory/Hook.hpp>)
#include <pl/memory/Hook.hpp>
#define PDT_HAS_PL_HOOK 1
#else
#define PDT_HAS_PL_HOOK 0
#endif

#if __has_include(<pl/memory/Vtable.hpp>)
#include <pl/memory/Vtable.hpp>
#define PDT_HAS_PL_VTABLE 1
#else
#define PDT_HAS_PL_VTABLE 0
#endif

#if __has_include(<pl/memory/Signature.hpp>)
#include <pl/memory/Signature.hpp>
#define PDT_HAS_PL_SIG 1
#elif __has_include(<pl/Signature.hpp>)
#include <pl/Signature.hpp>
#define PDT_HAS_PL_SIG 1
#else
#define PDT_HAS_PL_SIG 0
#endif

namespace paperdoll {

namespace {

// AArch64 AAPCS64 function pointer types for Minecraft Bedrock 26.60 virtual methods
using UICustomRendererRenderFn = void (*)(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
);

using PaperDollRenderGuiFn = void (*)(
    void *self,
    const void *rendererName,
    McActorRenderDataPrefix *actorRenderData,
    const void *captureDesc
);

using DataDrivenRenderFn = void (*)(
    void *self,
    void *baseActorRenderContext,
    McActorRenderDataPrefix *actorRenderData
);

using MobGetInterpolatedRotFn = float (*)(const void *self, float partialTicks);
using AInputQueueGetEventFn = int32_t (*)(void *queue, void **outEvent);
using EglSwapBuffersFn = unsigned int (*)(void *display, void *surface);
using EglQuerySurfaceFn = unsigned int (*)(void *display, void *surface, int attribute, int *value);

// Minimal OpenGL ES 2.0 scissor-clear function pointers resolved via dlsym (zero shader/VBO state impact)
using GlEnableFn = void (*)(unsigned int cap);
using GlDisableFn = void (*)(unsigned int cap);
using GlIsEnabledFn = unsigned char (*)(unsigned int cap);
using GlGetIntegervFn = void (*)(unsigned int pname, int *data);
using GlGetFloatvFn = void (*)(unsigned int pname, float *data);
using GlScissorFn = void (*)(int x, int y, int width, int height);
using GlClearColorFn = void (*)(float r, float g, float b, float a);
using GlClearFn = void (*)(unsigned int mask);

constexpr unsigned int kGlScissorTest = 0x0C11;
constexpr unsigned int kGlScissorBox = 0x0C10;
constexpr unsigned int kGlColorClearValue = 0x0C22;
constexpr unsigned int kGlColorBufferBit = 0x00004000;
constexpr int kEglWidth = 0x3057;
constexpr int kEglHeight = 0x3056;

// Timeout (ms) since last GUI render call to consider GUI open (immediately hides cursor on close)
constexpr std::int64_t kGuiOpenTimeoutMs = 120;
constexpr std::int64_t kInventoryRenderTimeoutMs = 200;

// Normalized screen position of the Paper Doll eye/chest center in Inventory UI
constexpr float kPaperDollAnchorX = 0.28f;
constexpr float kPaperDollAnchorY = 0.34f;

// Crisp 12x19 Windows Standard Arrow Pointer Bitmap (0 = transparent, 1 = dark outline #111827, 2 = pure white #FFFFFF)
constexpr std::array<std::string_view, 19> kWindowsCursorBitmap = {
    "100000000000",
    "110000000000",
    "121000000000",
    "122100000000",
    "122210000000",
    "122221000000",
    "122222100000",
    "122222210000",
    "122222221000",
    "122222222100",
    "122222222210",
    "122222211111",
    "122212210000",
    "122101221000",
    "121001221000",
    "110000122100",
    "100000122100",
    "000000012210",
    "000000001100"
};

bool isValidRotationFloat(float val) noexcept {
    return std::isfinite(val) && std::fabs(val) <= 36000.0f;
}

bool isLikelyHeapPtr(std::uintptr_t rawAddr) noexcept {
    if ((rawAddr & 0x7ULL) != 0ULL)
        return false;
    // Strip ARM64 Top-Byte-Ignore (TBI / MTE) tag used by Android 11+ Scudo allocator
    const std::uintptr_t untagged = rawAddr & 0x00FFFFFFFFFFFFFFULL;
    return untagged >= 0x10000000ULL && untagged <= 0x00007FFFFFFFFFFFULL;
}

// Scoped RAII guard for inInventoryRenderPass flag
struct ScopedInventoryPass {
    std::atomic<bool> &flag;
    explicit ScopedInventoryPass(std::atomic<bool> &f) noexcept : flag(f) {
        flag.store(true, std::memory_order_release);
    }
    ~ScopedInventoryPass() noexcept {
        flag.store(false, std::memory_order_release);
    }
};

void detourLivePlayerRendererRender(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    PaperDollTouchMod::instance().onLivePlayerRenderDetour(
        self, renderContext, clientInstance, uiControl, pass
    );
}

void detourPaperDollRendererRender(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    PaperDollTouchMod::instance().onPaperDollRenderDetour(
        self, renderContext, clientInstance, uiControl, pass
    );
}

void detourGradientRendererRender(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    PaperDollTouchMod::instance().onGradientRenderDetour(
        self, renderContext, clientInstance, uiControl, pass
    );
}

void detourInventoryItemRendererRender(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    PaperDollTouchMod::instance().onInventoryItemRenderDetour(
        self, renderContext, clientInstance, uiControl, pass
    );
}

void detourHoverRendererRender(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    PaperDollTouchMod::instance().onHoverRenderDetour(
        self, renderContext, clientInstance, uiControl, pass
    );
}

void detourPaperDollRenderGui(
    void *self,
    const void *rendererName,
    McActorRenderDataPrefix *actorRenderData,
    const void *captureDesc
) noexcept {
    PaperDollTouchMod::instance().onPaperDollRenderGuiDetour(
        self, rendererName, actorRenderData, captureDesc
    );
}

void detourDataDrivenRendererRender(
    void *self,
    void *baseActorRenderContext,
    McActorRenderDataPrefix *actorRenderData
) noexcept {
    PaperDollTouchMod::instance().onDataDrivenRenderDetour(
        self, baseActorRenderContext, actorRenderData
    );
}

float detourMobGetInterpolatedBodyRot(const void *self, float partialTicks) noexcept {
    return PaperDollTouchMod::instance().onMobGetInterpolatedBodyRotDetour(self, partialTicks);
}

float detourMobGetInterpolatedHeadRot(const void *self, float partialTicks) noexcept {
    return PaperDollTouchMod::instance().onMobGetInterpolatedHeadRotDetour(self, partialTicks);
}

float detourMobGetInterpolatedBodyYaw(const void *self, float partialTicks) noexcept {
    return PaperDollTouchMod::instance().onMobGetInterpolatedBodyYawDetour(self, partialTicks);
}

int32_t detourAInputQueueGetEvent(void *queue, void **outEvent) noexcept {
    return PaperDollTouchMod::instance().onInputQueueGetEventDetour(queue, outEvent);
}

unsigned int detourEglSwapBuffers(void *display, void *surface) noexcept {
    return PaperDollTouchMod::instance().onEglSwapBuffersDetour(display, surface);
}

} // namespace

PaperDollTouchMod &PaperDollTouchMod::instance() {
    static PaperDollTouchMod sInstance;
    return sInstance;
}

PaperDollTouchMod::PaperDollTouchMod() : mSelf(*ll::mod::NativeMod::current()) {}

std::int64_t PaperDollTouchMod::monotonicMillis() noexcept {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000LL +
           static_cast<std::int64_t>(ts.tv_nsec / 1000000LL);
}

void PaperDollTouchMod::sanitizeAndApplyConfig() noexcept {
    if (mConfig.version < 4) {
        mConfig.version = 4;
        mConfig.boundsMinX = 0.0f;
        mConfig.boundsMinY = 0.0f;
        mConfig.boundsMaxX = 1.0f;
        mConfig.boundsMaxY = 1.0f;
        mConfig.sensitivityYaw = 1.0f;
        mConfig.sensitivityPitch = 1.0f;
        mConfig.smoothingFactor = 0.45f;
        mConfig.cursorEnabled = true;
        mConfig.cursorSizePreset = "medium";
        mConfig.cursorScale = 1.00f;
        mConfig.greenSlotHoverEnabled = true;
        mConfig.paperDollFollowCursor = true;
        mConfig.hideWhenMouseConnected = true;
    }

    if (mConfig.cursorSizePreset == "small" && std::fabs(mConfig.cursorScale - 1.0f) < 0.01f) {
        mConfig.cursorScale = 0.75f;
    } else if (mConfig.cursorSizePreset == "large" && std::fabs(mConfig.cursorScale - 1.0f) < 0.01f) {
        mConfig.cursorScale = 1.50f;
    }
    mConfig.cursorScale = std::clamp(mConfig.cursorScale, 0.75f, 2.00f);
    mConfig.trackpadSensitivity = std::clamp(mConfig.trackpadSensitivity, 0.4f, 3.0f);

    mConfig.sensitivityYaw = std::clamp(mConfig.sensitivityYaw, 0.1f, 5.0f);
    mConfig.sensitivityPitch = std::clamp(mConfig.sensitivityPitch, 0.0f, 5.0f);
    mConfig.maxPitchDegrees = std::clamp(mConfig.maxPitchDegrees, 0.0f, 85.0f);
    mConfig.smoothingFactor = std::clamp(mConfig.smoothingFactor, 0.05f, 1.0f);

    mConfig.boundsMinX = std::clamp(mConfig.boundsMinX, 0.0f, 0.95f);
    mConfig.boundsMinY = std::clamp(mConfig.boundsMinY, 0.0f, 0.95f);
    mConfig.boundsMaxX = std::clamp(mConfig.boundsMaxX, mConfig.boundsMinX + 0.05f, 1.0f);
    mConfig.boundsMaxY = std::clamp(mConfig.boundsMaxY, mConfig.boundsMinY + 0.05f, 1.0f);

    mState.cursorEnabled.store(mConfig.cursorEnabled, std::memory_order_relaxed);
    mState.cursorScale.store(mConfig.cursorScale, std::memory_order_relaxed);
    mState.greenSlotHoverEnabled.store(mConfig.greenSlotHoverEnabled, std::memory_order_relaxed);
    mState.paperDollFollowCursor.store(mConfig.paperDollFollowCursor, std::memory_order_relaxed);
    mState.trackpadMode.store(mConfig.touchInputMode == "trackpad", std::memory_order_relaxed);

    mState.activeBoundsMinX.store(mConfig.boundsMinX, std::memory_order_relaxed);
    mState.activeBoundsMinY.store(mConfig.boundsMinY, std::memory_order_relaxed);
    mState.activeBoundsMaxX.store(mConfig.boundsMaxX, std::memory_order_relaxed);
    mState.activeBoundsMaxY.store(mConfig.boundsMaxY, std::memory_order_relaxed);
}

void PaperDollTouchMod::saveRuntimeSettingsToConfig() noexcept {
    mConfig.cursorEnabled = mState.cursorEnabled.load(std::memory_order_relaxed);
    const float scale = std::clamp(mState.cursorScale.load(std::memory_order_relaxed), 0.75f, 2.00f);
    mConfig.cursorScale = scale;
    if (scale <= 0.85f) {
        mConfig.cursorSizePreset = "small";
    } else if (scale >= 1.35f) {
        mConfig.cursorSizePreset = "large";
    } else {
        mConfig.cursorSizePreset = "medium";
    }
    mConfig.greenSlotHoverEnabled = mState.greenSlotHoverEnabled.load(std::memory_order_relaxed);
    mConfig.paperDollFollowCursor = mState.paperDollFollowCursor.load(std::memory_order_relaxed);
    mConfig.touchInputMode = mState.trackpadMode.load(std::memory_order_relaxed) ? "trackpad" : "direct";

    std::error_code ec;
    const auto cfgPath = getSelf().getConfigDir() / "config.json";
    std::ofstream out(cfgPath, std::ios::binary | std::ios::trunc);
    if (out) {
        out << pl::config::toJson(mConfig).dump(2) << '\n';
    }
}

bool PaperDollTouchMod::load() {
    auto &self = getSelf();
    self.getLogger().info(
        "Loading PaperDollTouch v2.0.0 (Minecraft Mobile Cursor — Windows-Style GUI System + PaperDoll Head Follow) for Minecraft Bedrock {} (arm64-v8a)...",
        mConfig.expectedMinecraftVersion
    );

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create data directory {}: {}",
                               self.getDataDir().string(), ec.message());
        return false;
    }

    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create config directory {}: {}",
                               self.getConfigDir().string(), ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Using default Mobile Cursor & PaperDollTouch configuration values.");
    } else {
        mConfig = mConfigFile->value();
    }

    sanitizeAndApplyConfig();
    resetTouchAndOrientation(true);

    self.getLogger().info("Loaded {} from {}", self.getName(), self.getModDir().string());
    return true;
}

void PaperDollTouchMod::writeDiagnosticProbeReport(
    bool hookReady,
    const std::string &liveRenderSource,
    const std::string &paperDollSource,
    const std::string &dataDrivenSource,
    const std::string &mobInterpSource
) const noexcept {
    std::error_code ec;
    const auto reportPath = getSelf().getDataDir() / "symbol_probe_26_60.json";
    std::ofstream out(reportPath, std::ios::binary | std::ios::trunc);
    if (!out)
        return;

    nlohmann::json report = {
        {"mod", "PaperDollTouch"},
        {"version", "2.0.0"},
        {"mode", "windows_mobile_cursor_and_paperdoll"},
        {"targetMinecraftVersion", mConfig.expectedMinecraftVersion},
        {"hookInstalled", hookReady},
        {"livePlayerRendererResolved", mLivePlayerRenderTarget != nullptr},
        {"livePlayerRendererSource", liveRenderSource},
        {"paperDollRendererResolved", mPaperDollRenderTarget != nullptr},
        {"paperDollRendererSource", paperDollSource},
        {"gradientRendererResolved", mGradientRenderTarget != nullptr},
        {"inventoryItemRendererResolved", mInventoryItemRenderTarget != nullptr},
        {"hoverRendererResolved", mHoverRenderTarget != nullptr},
        {"dataDrivenRendererResolved", mDataDrivenRenderTarget != nullptr},
        {"dataDrivenRendererSource", dataDrivenSource},
        {"mobInterpolatedRotResolved", mMobBodyRotTarget != nullptr},
        {"mobInterpolatedHeadRotResolved", mMobHeadRotTarget != nullptr},
        {"mobInterpolatedRotSource", mobInterpSource},
        {"eglOverlayHookInstalled", mEglHookInstalled},
        {"plInputRegistered", mPlInputCallbackRegistered},
        {"inputQueueHookInstalled", mInputQueueHookInstalled}
    };
    out << report.dump(2) << '\n';
}

bool PaperDollTouchMod::tryResolveAndInstallHooks() noexcept {
    auto &logger = getSelf().getLogger();

    std::uintptr_t liveRenderAddr = 0;
    std::uintptr_t paperDollRenderAddr = 0;
    std::uintptr_t gradientRenderAddr = 0;
    std::uintptr_t inventoryItemRenderAddr = 0;
    std::uintptr_t hoverRenderAddr = 0;
    std::uintptr_t paperDollRenderGuiAddr = 0;
    std::uintptr_t dataDrivenRenderAddr = 0;
    std::uintptr_t mobBodyRotAddr = 0;
    std::uintptr_t mobHeadRotAddr = 0;
    std::uintptr_t mobBodyYawAddr = 0;

    std::string liveSource = "unresolved";
    std::string paperDollSource = "unresolved";
    std::string dataDrivenSource = "unresolved";
    std::string mobInterpSource = "unresolved";

#if PDT_HAS_PL_SIG
    if (!mConfig.hudPlayerRendererRenderSig.empty()) {
        liveRenderAddr = pl::memory::resolveSignature(mConfig.hudPlayerRendererRenderSig, "libminecraftpe.so");
        if (liveRenderAddr != 0) {
            liveSource = "config:hudPlayerRendererRenderSig";
        }
    }
#endif

#if PDT_HAS_PL_VTABLE
    if (mConfig.allowSymbolAutoProbe) {
        auto resolveMcVtable = [](std::string_view typeInfoName, std::size_t slot) noexcept -> std::uintptr_t {
            return pl::memory::resolveVtableFunction(typeInfoName, slot, "libminecraftpe.so");
        };

        // 1. Resolve MinecraftUICustomRenderer subclasses (slot 17 in 26.60)
        std::size_t resolvedUiRenderSlot = 17;
        constexpr std::array<std::size_t, 9> kUiRenderSlotOrder = {17, 16, 18, 15, 19, 14, 20, 21, 22};
        for (std::size_t slot : kUiRenderSlotOrder) {
            const std::uintptr_t candLive = resolveMcVtable("18LivePlayerRenderer", slot);
            const std::uintptr_t candHud = resolveMcVtable("17HudPlayerRenderer", slot);
            const std::uintptr_t candPaper = resolveMcVtable("17PaperDollRenderer", slot);
            const std::uintptr_t candBase = resolveMcVtable("25MinecraftUICustomRenderer", slot);

            if (candLive != 0 && candHud != 0 && candLive != candHud && candLive != candBase) {
                resolvedUiRenderSlot = slot;
                if (liveRenderAddr == 0) {
                    liveRenderAddr = candLive;
                    liveSource = "vtable:18LivePlayerRenderer#" + std::to_string(slot);
                }
                if (paperDollRenderAddr == 0 && candPaper != 0 && candPaper != candHud && candPaper != candBase) {
                    paperDollRenderAddr = candPaper;
                    paperDollSource = "vtable:17PaperDollRenderer#" + std::to_string(slot);
                }
                break;
            }
        }

        if (liveRenderAddr == 0) {
            liveRenderAddr = resolveMcVtable("18LivePlayerRenderer", resolvedUiRenderSlot);
            if (liveRenderAddr != 0)
                liveSource = "vtable:18LivePlayerRenderer#" + std::to_string(resolvedUiRenderSlot);
        }
        if (paperDollRenderAddr == 0) {
            paperDollRenderAddr = resolveMcVtable("17PaperDollRenderer", resolvedUiRenderSlot);
            if (paperDollRenderAddr != 0)
                paperDollSource = "vtable:17PaperDollRenderer#" + std::to_string(resolvedUiRenderSlot);
        }

        // Resolve GradientRenderer (active on all modal GUI screens: Inventory, Chest, Crafting, Furnace, Pause, Settings)
        gradientRenderAddr = resolveMcVtable("16GradientRenderer", resolvedUiRenderSlot);
        // Resolve InventoryItemRenderer (active on all container/inventory slots)
        inventoryItemRenderAddr = resolveMcVtable("21InventoryItemRenderer", resolvedUiRenderSlot);
        // Resolve HoverRenderer (active on slot hover / tooltip pass)
        hoverRenderAddr = resolveMcVtable("13HoverRenderer", resolvedUiRenderSlot);

        // 2. Resolve PaperDollBaseActorRenderContext::renderGui (slot 6)
        paperDollRenderGuiAddr = resolveMcVtable("31PaperDollBaseActorRenderContext", 6);

        // 3. Resolve DataDrivenRenderer::render(BaseActorRenderContext&, ActorRenderData&) (slot 3)
        constexpr std::array<std::size_t, 5> kDataDrivenSlotOrder = {3, 2, 4, 5, 6};
        for (std::size_t slot : kDataDrivenSlotOrder) {
            const std::uintptr_t candDd = resolveMcVtable("18DataDrivenRenderer", slot);
            const std::uintptr_t candBase = resolveMcVtable("13ActorRenderer", slot);
            if (candDd != 0 && candDd != candBase) {
                dataDrivenRenderAddr = candDd;
                dataDrivenSource = "vtable:18DataDrivenRenderer#" + std::to_string(slot);
                break;
            }
        }
        if (dataDrivenRenderAddr == 0) {
            dataDrivenRenderAddr = resolveMcVtable("18DataDrivenRenderer", 3);
            if (dataDrivenRenderAddr != 0)
                dataDrivenSource = "vtable:18DataDrivenRenderer#3";
        }

        // 4. Resolve Mob::getInterpolatedBodyRot (slot 14), Actor::getInterpolatedHeadRot (slot 15), Mob::getInterpolatedBodyYaw (slot 16)
        bool foundMobSlots = false;
        for (std::size_t baseSlot : {14ULL, 13ULL, 15ULL, 12ULL, 16ULL, 11ULL, 17ULL}) {
            const std::uintptr_t m0 = resolveMcVtable("3Mob", baseSlot);
            const std::uintptr_t m1 = resolveMcVtable("3Mob", baseSlot + 1);
            const std::uintptr_t m2 = resolveMcVtable("3Mob", baseSlot + 2);
            const std::uintptr_t a0 = resolveMcVtable("5Actor", baseSlot);
            const std::uintptr_t a1 = resolveMcVtable("5Actor", baseSlot + 1);
            const std::uintptr_t a2 = resolveMcVtable("5Actor", baseSlot + 2);
            const std::uintptr_t prevM = baseSlot > 0 ? resolveMcVtable("3Mob", baseSlot - 1) : 0;
            const std::uintptr_t prevA = baseSlot > 0 ? resolveMcVtable("5Actor", baseSlot - 1) : 0;

            if (m0 != 0 && m1 != 0 && m2 != 0 &&
                a0 != 0 && a1 != 0 && a2 != 0 &&
                (prevM == prevA) && (m0 != a0) && (m1 == a1) && (m2 != a2)) {
                mobBodyRotAddr = m0;
                mobHeadRotAddr = m1;
                mobBodyYawAddr = m2;
                mobInterpSource = "vtable:3Mob/5Actor#" + std::to_string(baseSlot) + ".." + std::to_string(baseSlot + 2);
                foundMobSlots = true;
                break;
            }
        }
        if (!foundMobSlots) {
            for (std::size_t baseSlot : {14ULL, 13ULL, 15ULL, 12ULL}) {
                const std::uintptr_t m0 = resolveMcVtable("3Mob", baseSlot);
                const std::uintptr_t m1 = resolveMcVtable("3Mob", baseSlot + 1);
                const std::uintptr_t m2 = resolveMcVtable("3Mob", baseSlot + 2);
                const std::uintptr_t a0 = resolveMcVtable("5Actor", baseSlot);
                const std::uintptr_t a1 = resolveMcVtable("5Actor", baseSlot + 1);
                const std::uintptr_t a2 = resolveMcVtable("5Actor", baseSlot + 2);
                const std::uintptr_t prev1M = baseSlot > 0 ? resolveMcVtable("3Mob", baseSlot - 1) : 0;
                const std::uintptr_t prev1A = baseSlot > 0 ? resolveMcVtable("5Actor", baseSlot - 1) : 0;
                const std::uintptr_t prev2M = baseSlot > 1 ? resolveMcVtable("3Mob", baseSlot - 2) : 0;
                const std::uintptr_t prev2A = baseSlot > 1 ? resolveMcVtable("5Actor", baseSlot - 2) : 0;

                if (m0 != 0 && m1 != 0 && m2 != 0 &&
                    a0 != 0 && a1 != 0 && a2 != 0 &&
                    (prev1M == prev1A) && (prev2M == prev2A) &&
                    (m0 != a0) && (m1 != a1) && (m2 != a2)) {
                    mobBodyRotAddr = m0;
                    mobHeadRotAddr = m1;
                    mobBodyYawAddr = m2;
                    mobInterpSource = "vtable:3Mob#" + std::to_string(baseSlot) + ".." + std::to_string(baseSlot + 2);
                    foundMobSlots = true;
                    break;
                }
            }
        }
        if (!foundMobSlots) {
            mobBodyRotAddr = resolveMcVtable("3Mob", 14);
            mobHeadRotAddr = resolveMcVtable("3Mob", 15);
            mobBodyYawAddr = resolveMcVtable("3Mob", 16);
            if (mobBodyRotAddr != 0)
                mobInterpSource = "vtable:3Mob#14..16";
        }
    }
#endif

#if PDT_HAS_PL_HOOK
    bool anyRendererHooked = false;

    if (liveRenderAddr != 0) {
        mLivePlayerRenderTarget = reinterpret_cast<void *>(liveRenderAddr);
        if (pl::memory::hook(
                mLivePlayerRenderTarget,
                reinterpret_cast<void *>(&detourLivePlayerRendererRender),
                &mLivePlayerRenderOriginal,
                pl::memory::HookPriority::Normal) == 0 &&
            mLivePlayerRenderOriginal != nullptr) {
            anyRendererHooked = true;
            logger.info("[MobileCursor] Hooked LivePlayerRenderer::render ({})", liveSource);
        } else {
            mLivePlayerRenderTarget = nullptr;
            mLivePlayerRenderOriginal = nullptr;
        }
    }

    if (paperDollRenderAddr != 0) {
        mPaperDollRenderTarget = reinterpret_cast<void *>(paperDollRenderAddr);
        if (pl::memory::hook(
                mPaperDollRenderTarget,
                reinterpret_cast<void *>(&detourPaperDollRendererRender),
                &mPaperDollRenderOriginal,
                pl::memory::HookPriority::Normal) == 0 &&
            mPaperDollRenderOriginal != nullptr) {
            anyRendererHooked = true;
            logger.info("[MobileCursor] Hooked PaperDollRenderer::render ({})", paperDollSource);
        } else {
            mPaperDollRenderTarget = nullptr;
            mPaperDollRenderOriginal = nullptr;
        }
    }

    if (gradientRenderAddr != 0 && gradientRenderAddr != liveRenderAddr && gradientRenderAddr != paperDollRenderAddr) {
        mGradientRenderTarget = reinterpret_cast<void *>(gradientRenderAddr);
        if (pl::memory::hook(
                mGradientRenderTarget,
                reinterpret_cast<void *>(&detourGradientRendererRender),
                &mGradientRenderOriginal,
                pl::memory::HookPriority::Normal) == 0 &&
            mGradientRenderOriginal != nullptr) {
            anyRendererHooked = true;
            logger.info("[MobileCursor] Hooked GradientRenderer::render for modal GUI lifecycle detection.");
        } else {
            mGradientRenderTarget = nullptr;
            mGradientRenderOriginal = nullptr;
        }
    }

    if (inventoryItemRenderAddr != 0 && inventoryItemRenderAddr != liveRenderAddr) {
        mInventoryItemRenderTarget = reinterpret_cast<void *>(inventoryItemRenderAddr);
        if (pl::memory::hook(
                mInventoryItemRenderTarget,
                reinterpret_cast<void *>(&detourInventoryItemRendererRender),
                &mInventoryItemRenderOriginal,
                pl::memory::HookPriority::Normal) == 0 &&
            mInventoryItemRenderOriginal != nullptr) {
            anyRendererHooked = true;
            logger.info("[MobileCursor] Hooked InventoryItemRenderer::render for real-time slot hover detection.");
        } else {
            mInventoryItemRenderTarget = nullptr;
            mInventoryItemRenderOriginal = nullptr;
        }
    }

    if (hoverRenderAddr != 0 && hoverRenderAddr != liveRenderAddr && hoverRenderAddr != inventoryItemRenderAddr) {
        mHoverRenderTarget = reinterpret_cast<void *>(hoverRenderAddr);
        if (pl::memory::hook(
                mHoverRenderTarget,
                reinterpret_cast<void *>(&detourHoverRendererRender),
                &mHoverRenderOriginal,
                pl::memory::HookPriority::Normal) != 0) {
            mHoverRenderTarget = nullptr;
            mHoverRenderOriginal = nullptr;
        }
    }

    if (paperDollRenderGuiAddr != 0) {
        mPaperDollRenderGuiTarget = reinterpret_cast<void *>(paperDollRenderGuiAddr);
        if (pl::memory::hook(
                mPaperDollRenderGuiTarget,
                reinterpret_cast<void *>(&detourPaperDollRenderGui),
                &mPaperDollRenderGuiOriginal,
                pl::memory::HookPriority::Normal) != 0) {
            mPaperDollRenderGuiTarget = nullptr;
            mPaperDollRenderGuiOriginal = nullptr;
        }
    }

    if (dataDrivenRenderAddr != 0) {
        mDataDrivenRenderTarget = reinterpret_cast<void *>(dataDrivenRenderAddr);
        if (pl::memory::hook(
                mDataDrivenRenderTarget,
                reinterpret_cast<void *>(&detourDataDrivenRendererRender),
                &mDataDrivenRenderOriginal,
                pl::memory::HookPriority::Normal) == 0 &&
            mDataDrivenRenderOriginal != nullptr) {
            logger.info("[MobileCursor] Hooked DataDrivenRenderer::render ({})", dataDrivenSource);
        } else {
            mDataDrivenRenderTarget = nullptr;
            mDataDrivenRenderOriginal = nullptr;
        }
    }

    if (mobBodyRotAddr != 0 && mobHeadRotAddr != 0 && mobBodyYawAddr != 0) {
        mMobBodyRotTarget = reinterpret_cast<void *>(mobBodyRotAddr);
        mMobHeadRotTarget = reinterpret_cast<void *>(mobHeadRotAddr);
        mMobBodyYawTarget = reinterpret_cast<void *>(mobBodyYawAddr);

        if (pl::memory::hook(
                mMobBodyRotTarget,
                reinterpret_cast<void *>(&detourMobGetInterpolatedBodyRot),
                &mMobBodyRotOriginal,
                pl::memory::HookPriority::Normal) != 0) {
            mMobBodyRotTarget = nullptr;
            mMobBodyRotOriginal = nullptr;
        }
        if (pl::memory::hook(
                mMobHeadRotTarget,
                reinterpret_cast<void *>(&detourMobGetInterpolatedHeadRot),
                &mMobHeadRotOriginal,
                pl::memory::HookPriority::Normal) != 0) {
            mMobHeadRotTarget = nullptr;
            mMobHeadRotOriginal = nullptr;
        }
        if (pl::memory::hook(
                mMobBodyYawTarget,
                reinterpret_cast<void *>(&detourMobGetInterpolatedBodyYaw),
                &mMobBodyYawOriginal,
                pl::memory::HookPriority::Normal) != 0) {
            mMobBodyYawTarget = nullptr;
            mMobBodyYawOriginal = nullptr;
        }
        logger.info("[MobileCursor] Hooked Mob/Actor interpolated body+head rotation getters ({})", mobInterpSource);
    }

    // Hook eglSwapBuffers in libEGL.so to draw the crisp Windows white arrow cursor, green slot hover & settings menu
    if (!mEglHookInstalled) {
        void *eglSym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        if (eglSym != nullptr) {
            mEglSwapBuffersTarget = eglSym;
            if (pl::memory::hook(
                    mEglSwapBuffersTarget,
                    reinterpret_cast<void *>(&detourEglSwapBuffers),
                    &mEglSwapBuffersOriginal,
                    pl::memory::HookPriority::Normal) == 0 &&
                mEglSwapBuffersOriginal != nullptr) {
                mEglHookInstalled = true;
                logger.info("[MobileCursor] Attached eglSwapBuffers overlay renderer (Windows Cursor + Green Slot Hover).");
            } else {
                mEglSwapBuffersTarget = nullptr;
                mEglSwapBuffersOriginal = nullptr;
            }
        }
    }

    if (anyRendererHooked) {
        mState.hookInstalled.store(true, std::memory_order_release);
        writeDiagnosticProbeReport(true, liveSource, paperDollSource, dataDrivenSource, mobInterpSource);
        return true;
    }
#endif

    logger.warn("[MobileCursor] Could not resolve UI renderer vtables (live={}, paperDoll={}).",
                liveSource, paperDollSource);
    writeDiagnosticProbeReport(false, liveSource, paperDollSource, dataDrivenSource, mobInterpSource);
    return false;
}

void PaperDollTouchMod::registerInputHandlers() noexcept {
    if (!mPlInputCallbackRegistered) {
#if PDT_HAS_PL_INPUT
        pl::input::registerTouchCallback(
            [](const pl::input::TouchEvent &event) -> bool {
                return PaperDollTouchMod::instance().onTouchEvent(
                    event.action,
                    event.pointerId,
                    event.x,
                    event.y
                );
            }
        );
        mPlInputCallbackRegistered = true;
#endif
    }

#if PDT_HAS_PL_HOOK
    if (mConfig.allowNativeInputQueueFallback && !mInputQueueHookInstalled) {
        void *sym = dlsym(RTLD_DEFAULT, "AInputQueue_getEvent");
        if (sym != nullptr) {
            mInputQueueGetEventTarget = sym;
            if (pl::memory::hook(
                    mInputQueueGetEventTarget,
                    reinterpret_cast<void *>(&detourAInputQueueGetEvent),
                    &mInputQueueGetEventOriginal,
                    pl::memory::HookPriority::Normal) == 0 &&
                mInputQueueGetEventOriginal != nullptr) {
                mInputQueueHookInstalled = true;
                getSelf().getLogger().info("[MobileCursor] Attached native AInputQueue_getEvent touch observer.");
            } else {
                mInputQueueGetEventTarget = nullptr;
                mInputQueueGetEventOriginal = nullptr;
            }
        }
    }
#endif
}

void PaperDollTouchMod::removeHooks() noexcept {
#if PDT_HAS_PL_HOOK
    if (mLivePlayerRenderTarget != nullptr) {
        pl::memory::unhook(mLivePlayerRenderTarget, reinterpret_cast<void *>(&detourLivePlayerRendererRender));
        mLivePlayerRenderTarget = nullptr;
        mLivePlayerRenderOriginal = nullptr;
    }
    if (mPaperDollRenderTarget != nullptr) {
        pl::memory::unhook(mPaperDollRenderTarget, reinterpret_cast<void *>(&detourPaperDollRendererRender));
        mPaperDollRenderTarget = nullptr;
        mPaperDollRenderOriginal = nullptr;
    }
    if (mGradientRenderTarget != nullptr) {
        pl::memory::unhook(mGradientRenderTarget, reinterpret_cast<void *>(&detourGradientRendererRender));
        mGradientRenderTarget = nullptr;
        mGradientRenderOriginal = nullptr;
    }
    if (mInventoryItemRenderTarget != nullptr) {
        pl::memory::unhook(mInventoryItemRenderTarget, reinterpret_cast<void *>(&detourInventoryItemRendererRender));
        mInventoryItemRenderTarget = nullptr;
        mInventoryItemRenderOriginal = nullptr;
    }
    if (mHoverRenderTarget != nullptr) {
        pl::memory::unhook(mHoverRenderTarget, reinterpret_cast<void *>(&detourHoverRendererRender));
        mHoverRenderTarget = nullptr;
        mHoverRenderOriginal = nullptr;
    }
    if (mPaperDollRenderGuiTarget != nullptr) {
        pl::memory::unhook(mPaperDollRenderGuiTarget, reinterpret_cast<void *>(&detourPaperDollRenderGui));
        mPaperDollRenderGuiTarget = nullptr;
        mPaperDollRenderGuiOriginal = nullptr;
    }
    if (mDataDrivenRenderTarget != nullptr) {
        pl::memory::unhook(mDataDrivenRenderTarget, reinterpret_cast<void *>(&detourDataDrivenRendererRender));
        mDataDrivenRenderTarget = nullptr;
        mDataDrivenRenderOriginal = nullptr;
    }
    if (mMobBodyRotTarget != nullptr) {
        pl::memory::unhook(mMobBodyRotTarget, reinterpret_cast<void *>(&detourMobGetInterpolatedBodyRot));
        mMobBodyRotTarget = nullptr;
        mMobBodyRotOriginal = nullptr;
    }
    if (mMobHeadRotTarget != nullptr) {
        pl::memory::unhook(mMobHeadRotTarget, reinterpret_cast<void *>(&detourMobGetInterpolatedHeadRot));
        mMobHeadRotTarget = nullptr;
        mMobHeadRotOriginal = nullptr;
    }
    if (mMobBodyYawTarget != nullptr) {
        pl::memory::unhook(mMobBodyYawTarget, reinterpret_cast<void *>(&detourMobGetInterpolatedBodyYaw));
        mMobBodyYawTarget = nullptr;
        mMobBodyYawOriginal = nullptr;
    }
    if (mEglHookInstalled && mEglSwapBuffersTarget != nullptr) {
        pl::memory::unhook(mEglSwapBuffersTarget, reinterpret_cast<void *>(&detourEglSwapBuffers));
        mEglHookInstalled = false;
        mEglSwapBuffersTarget = nullptr;
        mEglSwapBuffersOriginal = nullptr;
    }
    if (mInputQueueHookInstalled && mInputQueueGetEventTarget != nullptr) {
        pl::memory::unhook(mInputQueueGetEventTarget, reinterpret_cast<void *>(&detourAInputQueueGetEvent));
        mInputQueueHookInstalled = false;
        mInputQueueGetEventTarget = nullptr;
        mInputQueueGetEventOriginal = nullptr;
    }
#endif
    mState.hookInstalled.store(false, std::memory_order_release);
}

bool PaperDollTouchMod::enable() {
    auto &self = getSelf();
    if (!mConfig.enabled) {
        self.getLogger().info("MobileCursor & PaperDollTouch is disabled in config.json.");
        return true;
    }

    resetTouchAndOrientation(true);
    registerInputHandlers();
    tryResolveAndInstallHooks();

    mState.modActive.store(true, std::memory_order_release);
    self.getLogger().info(
        "PaperDollTouch v2.0.0 (Mobile Cursor + Green Slot Hover + PaperDoll Head Follow) enabled (Hooks: {}, EGL: {}, Input: plInput={}, AInputQueue={}).",
        mState.hookInstalled.load(std::memory_order_acquire) ? "ACTIVE" : "STANDBY",
        mEglHookInstalled,
        mPlInputCallbackRegistered,
        mInputQueueHookInstalled
    );
    return true;
}

bool PaperDollTouchMod::disable() {
    mState.modActive.store(false, std::memory_order_release);
    resetTouchAndOrientation(true);
    removeHooks();
    getSelf().getLogger().info("MobileCursor & PaperDollTouch disabled and state restored.");
    return true;
}

bool PaperDollTouchMod::unload() {
    disable();
    mConfigFile.reset();
    return true;
}

void PaperDollTouchMod::resetTouchAndOrientation(bool clearAngles) noexcept {
    mState.activePointerId.store(-1, std::memory_order_relaxed);
    mState.hasHoveredSlot.store(false, std::memory_order_relaxed);
    mState.settingsMenuOpen.store(false, std::memory_order_relaxed);
    mState.frameSlotCount.store(0, std::memory_order_relaxed);
    if (clearAngles) {
        mState.targetBodyYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.targetHeadYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.targetPitchOffset.store(0.0f, std::memory_order_relaxed);
        mState.currentBodyYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.currentHeadYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.currentPitchOffset.store(0.0f, std::memory_order_relaxed);
    }
}

bool PaperDollTouchMod::isPointInsidePreviewBounds(float x, float y) const noexcept {
    const float sw = std::max(1.0f, mState.screenWidth.load(std::memory_order_relaxed));
    const float sh = std::max(1.0f, mState.screenHeight.load(std::memory_order_relaxed));

    const float nx = std::clamp(x / sw, 0.0f, 1.0f);
    const float ny = std::clamp(y / sh, 0.0f, 1.0f);

    const float minX = mState.activeBoundsMinX.load(std::memory_order_relaxed);
    const float minY = mState.activeBoundsMinY.load(std::memory_order_relaxed);
    const float maxX = mState.activeBoundsMaxX.load(std::memory_order_relaxed);
    const float maxY = mState.activeBoundsMaxY.load(std::memory_order_relaxed);

    return (nx >= minX && nx <= maxX && ny >= minY && ny <= maxY);
}

void PaperDollTouchMod::updateCursorFollowTargets(float x, float y) noexcept {
    if (!mState.paperDollFollowCursor.load(std::memory_order_relaxed)) {
        mState.targetBodyYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.targetHeadYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.targetPitchOffset.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const float sw = std::max(1.0f, mState.screenWidth.load(std::memory_order_relaxed));
    const float sh = std::max(1.0f, mState.screenHeight.load(std::memory_order_relaxed));

    const float nx = std::clamp(x / sw, 0.0f, 1.0f);
    const float ny = std::clamp(y / sh, 0.0f, 1.0f);

    const float dx = nx - kPaperDollAnchorX;
    const float dy = ny - kPaperDollAnchorY;

    const float atanX = std::atan(dx * 3.8f);
    const float atanY = std::atan(dy * 3.8f);

    const float bodyYaw = std::clamp(-atanX * (26.0f * mConfig.sensitivityYaw), -45.0f, 45.0f);
    const float headYaw = std::clamp(-atanX * (62.0f * mConfig.sensitivityYaw), -75.0f, 75.0f);

    const float pitchSign = mConfig.invertVertical ? -1.0f : 1.0f;
    const float rawPitch = (atanY * pitchSign) * (38.0f * mConfig.sensitivityPitch);
    const float pitch = std::clamp(rawPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);

    mState.targetBodyYawOffset.store(bodyYaw, std::memory_order_relaxed);
    mState.targetHeadYawOffset.store(headYaw, std::memory_order_relaxed);
    mState.targetPitchOffset.store(pitch, std::memory_order_relaxed);
}

void PaperDollTouchMod::updateHoveredSlotFromCursor(float cx, float cy) noexcept {
    if (!mState.greenSlotHoverEnabled.load(std::memory_order_relaxed)) {
        mState.hasHoveredSlot.store(false, std::memory_order_relaxed);
        return;
    }

    const std::size_t count = std::min(
        mState.frameSlotCount.load(std::memory_order_acquire),
        RuntimeTouchState::kMaxTrackedSlots
    );

    // 1. Check exact slot rectangles collected from UIControl during InventoryItemRenderer::render
    for (std::size_t i = 0; i < count; ++i) {
        const DetectedSlotRect &r = mState.frameSlots[i];
        if (cx >= r.x && cx <= (r.x + r.w) && cy >= r.y && cy <= (r.y + r.h)) {
            mState.hoveredSlotX.store(r.x, std::memory_order_relaxed);
            mState.hoveredSlotY.store(r.y, std::memory_order_relaxed);
            mState.hoveredSlotW.store(r.w, std::memory_order_relaxed);
            mState.hoveredSlotH.store(r.h, std::memory_order_relaxed);
            mState.hasHoveredSlot.store(true, std::memory_order_release);
            return;
        }
    }

    // 2. If Inventory/Container is open and cursor is over an empty container grid cell,
    // snap to the calibrated container slot grid if inside the active inventory panel region
    const std::int64_t nowMs = monotonicMillis();
    const bool invVisible = (nowMs - mState.lastInventoryRenderMs.load(std::memory_order_relaxed)) <= kInventoryRenderTimeoutMs ||
                            mState.slotsRenderedInCurrentFrame.load(std::memory_order_relaxed) >= 6;
    if (invVisible) {
        const float sw = std::max(1.0f, mState.screenWidth.load(std::memory_order_relaxed));
        const float sh = std::max(1.0f, mState.screenHeight.load(std::memory_order_relaxed));
        const float nx = cx / sw;
        const float ny = cy / sh;

        // Standard Bedrock Inventory/Container right-hand & lower slot grid bounds
        if (nx >= 0.44f && nx <= 0.88f && ny >= 0.16f && ny <= 0.84f) {
            const float cellW = (0.44f / 9.0f) * sw;
            const float cellH = (0.68f / 6.0f) * sh;
            const int col = std::clamp(static_cast<int>((nx - 0.44f) / (0.44f / 9.0f)), 0, 8);
            const int row = std::clamp(static_cast<int>((ny - 0.16f) / (0.68f / 6.0f)), 0, 5);
            const float sx = 0.44f * sw + static_cast<float>(col) * cellW + 2.0f;
            const float sy = 0.16f * sh + static_cast<float>(row) * cellH + 2.0f;
            mState.hoveredSlotX.store(sx, std::memory_order_relaxed);
            mState.hoveredSlotY.store(sy, std::memory_order_relaxed);
            mState.hoveredSlotW.store(std::max(16.0f, cellW - 4.0f), std::memory_order_relaxed);
            mState.hoveredSlotH.store(std::max(16.0f, cellH - 4.0f), std::memory_order_relaxed);
            mState.hasHoveredSlot.store(true, std::memory_order_release);
            return;
        }
    }

    mState.hasHoveredSlot.store(false, std::memory_order_release);
}

bool PaperDollTouchMod::handleSettingsOverlayTouch(int maskedAction, float x, float y) noexcept {
    if (maskedAction != AMOTION_EVENT_ACTION_DOWN && maskedAction != AMOTION_EVENT_ACTION_MOVE)
        return false;

    const float sw = std::max(1.0f, mState.screenWidth.load(std::memory_order_relaxed));
    const float sh = std::max(1.0f, mState.screenHeight.load(std::memory_order_relaxed));

    // Top-right [Cursor] gear button rect: [sw - 132, 14] .. [sw - 16, 60]
    const float btnX0 = sw - 132.0f;
    const float btnY0 = 14.0f;
    const float btnX1 = sw - 16.0f;
    const float btnY1 = 60.0f;

    if (maskedAction == AMOTION_EVENT_ACTION_DOWN &&
        x >= btnX0 && x <= btnX1 && y >= btnY0 && y <= btnY1) {
        const bool nextOpen = !mState.settingsMenuOpen.load(std::memory_order_relaxed);
        mState.settingsMenuOpen.store(nextOpen, std::memory_order_release);
        return true;
    }

    if (!mState.settingsMenuOpen.load(std::memory_order_acquire))
        return false;

    // Centered Mobile Cursor Settings modal: 460x320 px
    const float panelW = std::min(460.0f, sw * 0.86f);
    const float panelH = std::min(320.0f, sh * 0.80f);
    const float px0 = (sw - panelW) * 0.5f;
    const float py0 = (sh - panelH) * 0.5f;

    if (x < px0 || x > (px0 + panelW) || y < py0 || y > (py0 + panelH)) {
        if (maskedAction == AMOTION_EVENT_ACTION_DOWN) {
            mState.settingsMenuOpen.store(false, std::memory_order_release);
        }
        return false;
    }

    const float relX = (x - px0) / panelW;
    const float relY = (y - py0) / panelH;

    // Row 1 (relY 0.20..0.42): Small (0.75x) | Medium (1.00x) | Large (1.50x)
    if (maskedAction == AMOTION_EVENT_ACTION_DOWN && relY >= 0.20f && relY <= 0.42f) {
        if (relX < 0.34f) {
            mState.cursorScale.store(0.75f, std::memory_order_relaxed);
        } else if (relX < 0.67f) {
            mState.cursorScale.store(1.00f, std::memory_order_relaxed);
        } else {
            mState.cursorScale.store(1.50f, std::memory_order_relaxed);
        }
        saveRuntimeSettingsToConfig();
        return true;
    }

    // Row 2 (relY 0.46..0.66): Continuous Cursor Scale Slider [0.75x .. 2.00x]
    if (relY >= 0.46f && relY <= 0.66f && relX >= 0.08f && relX <= 0.92f) {
        const float t = std::clamp((relX - 0.08f) / 0.84f, 0.0f, 1.0f);
        const float newScale = std::round((0.75f + t * 1.25f) * 20.0f) / 20.0f;
        mState.cursorScale.store(newScale, std::memory_order_relaxed);
        saveRuntimeSettingsToConfig();
        return true;
    }

    // Row 3 (relY 0.70..0.92): 3 Feature Toggles (Cursor ON/OFF | Green Hover ON/OFF | Head Follow ON/OFF)
    if (maskedAction == AMOTION_EVENT_ACTION_DOWN && relY >= 0.70f && relY <= 0.94f) {
        if (relX < 0.34f) {
            mState.cursorEnabled.store(!mState.cursorEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else if (relX < 0.67f) {
            mState.greenSlotHoverEnabled.store(!mState.greenSlotHoverEnabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else {
            mState.paperDollFollowCursor.store(!mState.paperDollFollowCursor.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        saveRuntimeSettingsToConfig();
        return true;
    }

    return true;
}

int32_t PaperDollTouchMod::onInputQueueGetEventDetour(void *queue, void **outEvent) noexcept {
    auto orig = reinterpret_cast<AInputQueueGetEventFn>(mInputQueueGetEventOriginal);
    if (!orig)
        return -1;

    const int32_t res = orig(queue, outEvent);
    if (res >= 0 && outEvent != nullptr && *outEvent != nullptr) {
        const auto *ev = static_cast<const AInputEvent *>(*outEvent);
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
            const int32_t source = AInputEvent_getSource(ev);
            const bool isMouse = (source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE;
            mState.physicalMouseActive.store(isMouse, std::memory_order_relaxed);
            if (isMouse && mConfig.hideWhenMouseConnected) {
                return res;
            }

            const int32_t combinedAction = AMotionEvent_getAction(ev);
            const int32_t actionMask = combinedAction & AMOTION_EVENT_ACTION_MASK;
            const size_t actionIdx = static_cast<size_t>(
                (combinedAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT
            );

            if (actionMask == AMOTION_EVENT_ACTION_MOVE) {
                const size_t count = AMotionEvent_getPointerCount(ev);
                for (size_t i = 0; i < count; ++i) {
                    const int32_t pid = AMotionEvent_getPointerId(ev, i);
                    const float px = AMotionEvent_getX(ev, i);
                    const float py = AMotionEvent_getY(ev, i);
                    onTouchEvent(actionMask, pid, px, py);
                }
            } else {
                const int32_t pid = AMotionEvent_getPointerId(ev, actionIdx);
                const float px = AMotionEvent_getX(ev, actionIdx);
                const float py = AMotionEvent_getY(ev, actionIdx);
                onTouchEvent(actionMask, pid, px, py);
            }
        }
    }
    return res;
}

bool PaperDollTouchMod::onTouchEvent(int action, int pointerId, float x, float y) noexcept {
    if (!mState.modActive.load(std::memory_order_acquire) ||
        !mState.hookInstalled.load(std::memory_order_acquire)) {
        return false;
    }

    const std::int64_t nowMs = monotonicMillis();
    const int maskedAction = action & AMOTION_EVENT_ACTION_MASK;

    // Deduplicate identical touch events if both pl::input and AInputQueue fire within 4ms
    const std::int64_t prevEventMs = mState.lastTouchEventMs.load(std::memory_order_relaxed);
    if ((nowMs - prevEventMs) <= 4 &&
        mState.lastTouchAction.load(std::memory_order_relaxed) == maskedAction &&
        std::fabs(mState.lastRawEventX.load(std::memory_order_relaxed) - x) < 0.5f &&
        std::fabs(mState.lastRawEventY.load(std::memory_order_relaxed) - y) < 0.5f) {
        return false;
    }
    mState.lastTouchEventMs.store(nowMs, std::memory_order_relaxed);
    mState.lastTouchAction.store(maskedAction, std::memory_order_relaxed);
    mState.lastRawEventX.store(x, std::memory_order_relaxed);
    mState.lastRawEventY.store(y, std::memory_order_relaxed);

    // Opportunistically calibrate physical screen dimensions from any touch on screen
    float currentSw = mState.screenWidth.load(std::memory_order_relaxed);
    float currentSh = mState.screenHeight.load(std::memory_order_relaxed);
    if (y > 740.0f && currentSh < 1080.0f) {
        currentSh = 1080.0f;
        currentSw = std::max(currentSw, 2160.0f);
        mState.screenHeight.store(currentSh, std::memory_order_relaxed);
        mState.screenWidth.store(currentSw, std::memory_order_relaxed);
    }
    if (y > 1100.0f && currentSh < 1440.0f) {
        currentSh = 1440.0f;
        currentSw = std::max(currentSw, 2880.0f);
        mState.screenHeight.store(currentSh, std::memory_order_relaxed);
        mState.screenWidth.store(currentSw, std::memory_order_relaxed);
    }
    if (x > currentSw && x < 16384.0f) {
        currentSw = x * 1.02f;
        mState.screenWidth.store(currentSw, std::memory_order_relaxed);
    }
    if (y > currentSh && y < 16384.0f) {
        currentSh = y * 1.02f;
        mState.screenHeight.store(currentSh, std::memory_order_relaxed);
    }

    // Check if any GUI screen is currently open (rendered within the last 120 ms)
    const std::int64_t lastGuiMs = mState.lastGuiRenderMs.load(std::memory_order_acquire);
    const std::int64_t lastInvMs = mState.lastInventoryRenderMs.load(std::memory_order_acquire);
    const bool guiOpen = ((lastGuiMs > 0) && ((nowMs - lastGuiMs) <= kGuiOpenTimeoutMs)) ||
                         ((lastInvMs > 0) && ((nowMs - lastInvMs) <= kInventoryRenderTimeoutMs));

    if (!guiOpen) {
        if (mState.guiScreenOpen.exchange(false, std::memory_order_acq_rel) ||
            mState.inventoryPreviewVisible.exchange(false, std::memory_order_acq_rel)) {
            resetTouchAndOrientation(mConfig.resetOnInventoryClose);
        }
        return false;
    }

    mState.guiScreenOpen.store(true, std::memory_order_release);

    // Check if the user tapped the top-right [Cursor] settings button or inside Mobile Cursor Settings
    if (handleSettingsOverlayTouch(maskedAction, x, y)) {
        return true;
    }

    switch (maskedAction) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        if (isPointInsidePreviewBounds(x, y)) {
            mState.activePointerId.store(pointerId, std::memory_order_relaxed);
            if (!mState.trackpadMode.load(std::memory_order_relaxed) ||
                !mState.cursorInitializedInGui.load(std::memory_order_relaxed)) {
                mState.cursorX.store(x, std::memory_order_relaxed);
                mState.cursorY.store(y, std::memory_order_relaxed);
                mState.cursorInitializedInGui.store(true, std::memory_order_relaxed);
            }
            mState.lastTouchX.store(x, std::memory_order_relaxed);
            mState.lastTouchY.store(y, std::memory_order_relaxed);

            const float cx = mState.cursorX.load(std::memory_order_relaxed);
            const float cy = mState.cursorY.load(std::memory_order_relaxed);
            updateCursorFollowTargets(cx, cy);
            updateHoveredSlotFromCursor(cx, cy);
        }
        break;
    }

    case AMOTION_EVENT_ACTION_MOVE: {
        const int trackedPointer = mState.activePointerId.load(std::memory_order_relaxed);
        if (trackedPointer == -1 || pointerId == trackedPointer) {
            if (trackedPointer == -1) {
                mState.activePointerId.store(pointerId, std::memory_order_relaxed);
            }
            float cx = x;
            float cy = y;
            if (mState.trackpadMode.load(std::memory_order_relaxed)) {
                const float dx = (x - mState.lastTouchX.load(std::memory_order_relaxed)) * mConfig.trackpadSensitivity;
                const float dy = (y - mState.lastTouchY.load(std::memory_order_relaxed)) * mConfig.trackpadSensitivity;
                cx = std::clamp(mState.cursorX.load(std::memory_order_relaxed) + dx, 0.0f, currentSw);
                cy = std::clamp(mState.cursorY.load(std::memory_order_relaxed) + dy, 0.0f, currentSh);
            }
            mState.cursorX.store(cx, std::memory_order_relaxed);
            mState.cursorY.store(cy, std::memory_order_relaxed);
            mState.lastTouchX.store(x, std::memory_order_relaxed);
            mState.lastTouchY.store(y, std::memory_order_relaxed);
            updateCursorFollowTargets(cx, cy);
            updateHoveredSlotFromCursor(cx, cy);
        }
        break;
    }

    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL: {
        const int trackedPointer = mState.activePointerId.load(std::memory_order_relaxed);
        if (trackedPointer != -1 && pointerId == trackedPointer) {
            // Keep last cursorX/cursorY saved while the same GUI remains open!
            mState.activePointerId.store(-1, std::memory_order_relaxed);
        }
        break;
    }

    default:
        break;
    }

    // Never consume gameplay/inventory slot touches so vanilla item move, split, and crafting work 100% natively
    return false;
}

void PaperDollTouchMod::stepOrientationSmoothing() noexcept {
    const std::int64_t nowMs = monotonicMillis();
    mState.lastInventoryRenderMs.store(nowMs, std::memory_order_release);
    mState.lastGuiRenderMs.store(nowMs, std::memory_order_release);
    mState.guiScreenOpen.store(true, std::memory_order_release);
    mState.inventoryPreviewVisible.store(true, std::memory_order_release);
    mState.renderedPreviewFrames.fetch_add(1, std::memory_order_relaxed);

    const float targetBodyYaw = mState.targetBodyYawOffset.load(std::memory_order_relaxed);
    const float targetHeadYaw = mState.targetHeadYawOffset.load(std::memory_order_relaxed);
    const float targetPitch = mState.targetPitchOffset.load(std::memory_order_relaxed);

    float curBodyYaw = mState.currentBodyYawOffset.load(std::memory_order_relaxed);
    float curHeadYaw = mState.currentHeadYawOffset.load(std::memory_order_relaxed);
    float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);

    const float alpha = mConfig.smoothingFactor;
    curBodyYaw += (targetBodyYaw - curBodyYaw) * alpha;
    curHeadYaw += (targetHeadYaw - curHeadYaw) * alpha;
    curPitch += (targetPitch - curPitch) * alpha;

    mState.currentBodyYawOffset.store(curBodyYaw, std::memory_order_relaxed);
    mState.currentHeadYawOffset.store(curHeadYaw, std::memory_order_relaxed);
    mState.currentPitchOffset.store(curPitch, std::memory_order_relaxed);
}

void PaperDollTouchMod::applyActorPitchToComponents(const void *actorPtr) noexcept {
    if (actorPtr == nullptr || mState.activeActorRotCompFloats != nullptr)
        return;

    const float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);
    if (std::fabs(curPitch) < 0.01f)
        return;

    const auto *actorBytes = static_cast<const std::uint8_t *>(actorPtr);
    float *foundRotFloats = nullptr;

    // Pass 1: Match BuiltInActorComponents pair inside Actor (+0x18..+0xA8):
    // [AABBShapeComponent* (off - 8), ActorRotationComponent* (off)]
    for (std::size_t off = 0x18; off <= 0xA8; off += sizeof(void *)) {
        void *prevPtr = *reinterpret_cast<void *const *>(actorBytes + off - sizeof(void *));
        void *candPtr = *reinterpret_cast<void *const *>(actorBytes + off);
        const auto prevAddr = reinterpret_cast<std::uintptr_t>(prevPtr);
        const auto candAddr = reinterpret_cast<std::uintptr_t>(candPtr);

        if (isLikelyHeapPtr(prevAddr) && isLikelyHeapPtr(candAddr) && prevAddr != candAddr) {
            const auto *aabb = static_cast<const float *>(prevPtr);
            auto *rot = static_cast<float *>(candPtr);

            if (std::isfinite(aabb[0]) && std::isfinite(aabb[3]) &&
                std::isfinite(aabb[6]) && std::isfinite(aabb[7]) &&
                aabb[6] >= 0.2f && aabb[6] <= 3.0f &&
                aabb[7] >= 0.2f && aabb[7] <= 4.0f &&
                std::fabs((aabb[3] - aabb[0]) - aabb[6]) < 0.05f &&
                isValidRotationFloat(rot[0]) && isValidRotationFloat(rot[1]) &&
                isValidRotationFloat(rot[2]) && isValidRotationFloat(rot[3]) &&
                std::fabs(rot[0]) <= 90.0f && std::fabs(rot[2]) <= 90.0f) {
                foundRotFloats = rot;
                break;
            }
        }
    }

    // Pass 2: Fallback scan for ActorRotationComponent (mPitch=0, mOldPitch=0, mYaw==mOldYaw during UI pass)
    if (foundRotFloats == nullptr) {
        for (std::size_t off = 0x18; off <= 0xA8; off += sizeof(void *)) {
            void *candPtr = *reinterpret_cast<void *const *>(actorBytes + off);
            const auto candAddr = reinterpret_cast<std::uintptr_t>(candPtr);
            if (isLikelyHeapPtr(candAddr)) {
                auto *rot = static_cast<float *>(candPtr);
                if (isValidRotationFloat(rot[0]) && isValidRotationFloat(rot[1]) &&
                    isValidRotationFloat(rot[2]) && isValidRotationFloat(rot[3]) &&
                    std::fabs(rot[0]) < 0.05f && std::fabs(rot[2]) < 0.05f &&
                    std::fabs(rot[1] - rot[3]) < 0.1f && std::fabs(rot[1]) > 0.001f) {
                    foundRotFloats = rot;
                    break;
                }
            }
        }
    }

    if (foundRotFloats != nullptr) {
        mState.savedActorRotCompFloats[0] = foundRotFloats[0];
        mState.savedActorRotCompFloats[1] = foundRotFloats[1];
        mState.savedActorRotCompFloats[2] = foundRotFloats[2];
        mState.savedActorRotCompFloats[3] = foundRotFloats[3];
        mState.activeActorRotCompFloats = foundRotFloats;

        const float newPitch = std::clamp(
            foundRotFloats[0] + curPitch,
            -mConfig.maxPitchDegrees,
            mConfig.maxPitchDegrees
        );
        foundRotFloats[0] = newPitch;
        foundRotFloats[2] = newPitch;
    }
}

void PaperDollTouchMod::restoreActorPitchComponents() noexcept {
    float *rot = mState.activeActorRotCompFloats;
    if (rot != nullptr) {
        rot[0] = mState.savedActorRotCompFloats[0];
        rot[1] = mState.savedActorRotCompFloats[1];
        rot[2] = mState.savedActorRotCompFloats[2];
        rot[3] = mState.savedActorRotCompFloats[3];
        mState.activeActorRotCompFloats = nullptr;
    }
}

void PaperDollTouchMod::onLivePlayerRenderDetour(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    auto originalFn = reinterpret_cast<UICustomRendererRenderFn>(mLivePlayerRenderOriginal);
    if (originalFn == nullptr)
        return;

    if (!mState.modActive.load(std::memory_order_acquire)) {
        originalFn(self, renderContext, clientInstance, uiControl, pass);
        return;
    }

    stepOrientationSmoothing();
    mState.bodyRotGetterCalledThisPass.store(false, std::memory_order_relaxed);
    mState.headRotGetterCalledThisPass.store(false, std::memory_order_relaxed);
    ScopedInventoryPass passGuard(mState.inInventoryRenderPass);
    originalFn(self, renderContext, clientInstance, uiControl, pass);
    restoreActorPitchComponents();
}

void PaperDollTouchMod::onPaperDollRenderDetour(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    auto originalFn = reinterpret_cast<UICustomRendererRenderFn>(mPaperDollRenderOriginal);
    if (originalFn == nullptr)
        return;

    if (!mState.modActive.load(std::memory_order_acquire)) {
        originalFn(self, renderContext, clientInstance, uiControl, pass);
        return;
    }

    stepOrientationSmoothing();
    mState.bodyRotGetterCalledThisPass.store(false, std::memory_order_relaxed);
    mState.headRotGetterCalledThisPass.store(false, std::memory_order_relaxed);
    ScopedInventoryPass passGuard(mState.inInventoryRenderPass);
    originalFn(self, renderContext, clientInstance, uiControl, pass);
    restoreActorPitchComponents();
}

void PaperDollTouchMod::onGradientRenderDetour(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    auto originalFn = reinterpret_cast<UICustomRendererRenderFn>(mGradientRenderOriginal);
    if (originalFn == nullptr)
        return;

    if (mState.modActive.load(std::memory_order_acquire)) {
        const std::int64_t nowMs = monotonicMillis();
        mState.lastGuiRenderMs.store(nowMs, std::memory_order_release);
        mState.guiScreenOpen.store(true, std::memory_order_release);
    }

    originalFn(self, renderContext, clientInstance, uiControl, pass);
}

void PaperDollTouchMod::onInventoryItemRenderDetour(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    auto originalFn = reinterpret_cast<UICustomRendererRenderFn>(mInventoryItemRenderOriginal);
    if (originalFn == nullptr)
        return;

    if (mState.modActive.load(std::memory_order_acquire)) {
        const std::int64_t nowMs = monotonicMillis();
        const std::int64_t prevBucket = mState.currentSlotFrameBucketMs.load(std::memory_order_relaxed);
        if ((nowMs - prevBucket) > 12) {
            mState.currentSlotFrameBucketMs.store(nowMs, std::memory_order_relaxed);
            mState.slotsRenderedInCurrentFrame.store(1, std::memory_order_relaxed);
            mState.frameSlotCount.store(0, std::memory_order_release);
        } else {
            const int countNow = mState.slotsRenderedInCurrentFrame.fetch_add(1, std::memory_order_relaxed) + 1;
            // HUD hotbar renders <= 9 slots; any container/inventory GUI renders >= 10 item slots
            if (countNow >= 10) {
                mState.lastGuiRenderMs.store(nowMs, std::memory_order_release);
                mState.guiScreenOpen.store(true, std::memory_order_release);
            }
        }

        // Extract UIControl bounding rectangle if valid (position + size Vec2 pair inside UIControl)
        if (uiControl != nullptr) {
            const auto *ctrlBytes = static_cast<const std::uint8_t *>(uiControl);
            const float sw = std::max(1.0f, mState.screenWidth.load(std::memory_order_relaxed));
            const float sh = std::max(1.0f, mState.screenHeight.load(std::memory_order_relaxed));
            for (std::size_t off = 0x18; off <= 0x78; off += sizeof(float)) {
                const auto *f = reinterpret_cast<const float *>(ctrlBytes + off);
                if (std::isfinite(f[0]) && std::isfinite(f[1]) &&
                    std::isfinite(f[2]) && std::isfinite(f[3]) &&
                    f[0] >= 4.0f && f[0] < sw && f[1] >= 4.0f && f[1] < sh &&
                    f[2] >= 14.0f && f[2] <= 180.0f &&
                    std::fabs(f[2] - f[3]) <= 8.0f) {
                    const std::size_t idx = mState.frameSlotCount.load(std::memory_order_relaxed);
                    if (idx < RuntimeTouchState::kMaxTrackedSlots) {
                        mState.frameSlots[idx] = DetectedSlotRect{f[0], f[1], f[2], f[3]};
                        mState.frameSlotCount.store(idx + 1, std::memory_order_release);
                    }
                    break;
                }
            }
        }
    }

    originalFn(self, renderContext, clientInstance, uiControl, pass);
}

void PaperDollTouchMod::onHoverRenderDetour(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass
) noexcept {
    auto originalFn = reinterpret_cast<UICustomRendererRenderFn>(mHoverRenderOriginal);
    if (originalFn == nullptr)
        return;

    if (mState.modActive.load(std::memory_order_acquire)) {
        const std::int64_t nowMs = monotonicMillis();
        mState.lastGuiRenderMs.store(nowMs, std::memory_order_release);
        mState.guiScreenOpen.store(true, std::memory_order_release);
    }

    originalFn(self, renderContext, clientInstance, uiControl, pass);
}

void PaperDollTouchMod::renderMobileCursorOverlay(int surfaceWidth, int surfaceHeight) noexcept {
    if (surfaceWidth <= 0 || surfaceHeight <= 0)
        return;

    mState.screenWidth.store(static_cast<float>(surfaceWidth), std::memory_order_relaxed);
    mState.screenHeight.store(static_cast<float>(surfaceHeight), std::memory_order_relaxed);

    const std::int64_t nowMs = monotonicMillis();
    const std::int64_t lastGuiMs = mState.lastGuiRenderMs.load(std::memory_order_acquire);
    const std::int64_t lastInvMs = mState.lastInventoryRenderMs.load(std::memory_order_acquire);
    const bool guiOpen = ((lastGuiMs > 0) && ((nowMs - lastGuiMs) <= kGuiOpenTimeoutMs)) ||
                         ((lastInvMs > 0) && ((nowMs - lastInvMs) <= kInventoryRenderTimeoutMs));

    if (!guiOpen) {
        if (mState.guiScreenOpen.exchange(false, std::memory_order_acq_rel)) {
            resetTouchAndOrientation(mConfig.resetOnInventoryClose);
        }
        return;
    }

    if (mConfig.hideWhenMouseConnected && mState.physicalMouseActive.load(std::memory_order_relaxed)) {
        return;
    }

    // Resolve OpenGL ES 2.0 scissor-clear functions once (zero shaders, zero VBOs, zero pipeline state mutation)
    static auto fnGlEnable = reinterpret_cast<GlEnableFn>(dlsym(RTLD_DEFAULT, "glEnable"));
    static auto fnGlDisable = reinterpret_cast<GlDisableFn>(dlsym(RTLD_DEFAULT, "glDisable"));
    static auto fnGlIsEnabled = reinterpret_cast<GlIsEnabledFn>(dlsym(RTLD_DEFAULT, "glIsEnabled"));
    static auto fnGlGetIntegerv = reinterpret_cast<GlGetIntegervFn>(dlsym(RTLD_DEFAULT, "glGetIntegerv"));
    static auto fnGlGetFloatv = reinterpret_cast<GlGetFloatvFn>(dlsym(RTLD_DEFAULT, "glGetFloatv"));
    static auto fnGlScissor = reinterpret_cast<GlScissorFn>(dlsym(RTLD_DEFAULT, "glScissor"));
    static auto fnGlClearColor = reinterpret_cast<GlClearColorFn>(dlsym(RTLD_DEFAULT, "glClearColor"));
    static auto fnGlClear = reinterpret_cast<GlClearFn>(dlsym(RTLD_DEFAULT, "glClear"));

    if (!fnGlEnable || !fnGlDisable || !fnGlIsEnabled || !fnGlGetIntegerv ||
        !fnGlGetFloatv || !fnGlScissor || !fnGlClearColor || !fnGlClear) {
        return;
    }

    const unsigned char wasScissor = fnGlIsEnabled(kGlScissorTest);
    int savedBox[4] = {0, 0, surfaceWidth, surfaceHeight};
    float savedClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    fnGlGetIntegerv(kGlScissorBox, savedBox);
    fnGlGetFloatv(kGlColorClearValue, savedClear);

    fnGlEnable(kGlScissorTest);

    // Helper lambda to fill a top-left screen-space rectangle via GL_SCISSOR_TEST + glClear
    auto fillRectTopLeft = [&](float x, float y, float w, float h, float r, float g, float b, float a) noexcept {
        const int ix = std::clamp(static_cast<int>( std::lround(x)), 0, surfaceWidth);
        const int iyTop = std::clamp(static_cast<int>(std::lround(y)), 0, surfaceHeight);
        const int iw = std::clamp(static_cast<int>(std::lround(w)), 0, surfaceWidth - ix);
        const int ih = std::clamp(static_cast<int>(std::lround(h)), 0, surfaceHeight - iyTop);
        if (iw <= 0 || ih <= 0)
            return;
        const int glY = surfaceHeight - (iyTop + ih);
        fnGlScissor(ix, glY, iw, ih);
        fnGlClearColor(r, g, b, a);
        fnGlClear(kGlColorBufferBit);
    };

    auto strokeRectTopLeft = [&](float x, float y, float w, float h, float thickness, float r, float g, float b) noexcept {
        fillRectTopLeft(x, y, w, thickness, r, g, b, 1.0f);
        fillRectTopLeft(x, y + h - thickness, w, thickness, r, g, b, 1.0f);
        fillRectTopLeft(x, y, thickness, h, r, g, b, 1.0f);
        fillRectTopLeft(x + w - thickness, y, thickness, h, r, g, b, 1.0f);
    };

    // 1. Render Windows-Style Green Slot Hover (#38E54D) if hovering over a slot
    if (mState.greenSlotHoverEnabled.load(std::memory_order_relaxed) &&
        mState.hasHoveredSlot.load(std::memory_order_acquire)) {
        const float sx = mState.hoveredSlotX.load(std::memory_order_relaxed);
        const float sy = mState.hoveredSlotY.load(std::memory_order_relaxed);
        const float sw = mState.hoveredSlotW.load(std::memory_order_relaxed);
        const float sh = mState.hoveredSlotH.load(std::memory_order_relaxed);
        strokeRectTopLeft(sx, sy, sw, sh, 3.0f, 0.22f, 0.90f, 0.30f);
    }

    // 2. Render Top-Right [Cursor] Settings Button so user can adjust cursor size in-game anytime
    const float btnX = static_cast<float>(surfaceWidth) - 124.0f;
    const float btnY = 16.0f;
    fillRectTopLeft(btnX, btnY, 108.0f, 38.0f, 0.08f, 0.12f, 0.18f, 1.0f);
    strokeRectTopLeft(btnX, btnY, 108.0f, 38.0f, 2.0f, 0.22f, 0.90f, 0.30f);

    // 3. Render interactive Mobile Cursor Settings Modal if open
    if (mState.settingsMenuOpen.load(std::memory_order_acquire)) {
        const float panelW = std::min(460.0f, static_cast<float>(surfaceWidth) * 0.86f);
        const float panelH = std::min(320.0f, static_cast<float>(surfaceHeight) * 0.80f);
        const float px0 = (static_cast<float>(surfaceWidth) - panelW) * 0.5f;
        const float py0 = (static_cast<float>(surfaceHeight) - panelH) * 0.5f;

        fillRectTopLeft(px0, py0, panelW, panelH, 0.07f, 0.09f, 0.14f, 1.0f);
        strokeRectTopLeft(px0, py0, panelW, panelH, 3.0f, 0.23f, 0.51f, 0.96f);

        const float currentScale = mState.cursorScale.load(std::memory_order_relaxed);
        // Row 1: Small (0.75x) | Medium (1.00x) | Large (1.50x)
        const float btnW = (panelW - 48.0f) / 3.0f;
        const float row1Y = py0 + panelH * 0.22f;
        const float row1H = panelH * 0.18f;
        const std::array<float, 3> presetScales = {0.75f, 1.00f, 1.50f};
        for (int i = 0; i < 3; ++i) {
            const float bx = px0 + 16.0f + static_cast<float>(i) * (btnW + 8.0f);
            const bool active = std::fabs(currentScale - presetScales[i]) < 0.12f;
            fillRectTopLeft(bx, row1Y, btnW, row1H, active ? 0.15f : 0.12f, active ? 0.45f : 0.16f, active ? 0.85f : 0.24f, 1.0f);
            strokeRectTopLeft(bx, row1Y, btnW, row1H, 2.0f, active ? 0.22f : 0.35f, active ? 0.90f : 0.45f, active ? 0.30f : 0.60f);
        }

        // Row 2: Slider Track [0.75x .. 2.00x]
        const float trackX = px0 + panelW * 0.08f;
        const float trackW = panelW * 0.84f;
        const float trackY = py0 + panelH * 0.54f;
        fillRectTopLeft(trackX, trackY, trackW, 12.0f, 0.18f, 0.23f, 0.32f, 1.0f);
        const float t = std::clamp((currentScale - 0.75f) / 1.25f, 0.0f, 1.0f);
        fillRectTopLeft(trackX, trackY, trackW * t, 12.0f, 0.22f, 0.90f, 0.30f, 1.0f);
        fillRectTopLeft(trackX + trackW * t - 10.0f, trackY - 8.0f, 20.0f, 28.0f, 0.98f, 0.98f, 0.98f, 1.0f);

        // Row 3: 3 Toggles (Cursor Enabled | Green Slot Hover | PaperDoll Head Follow)
        const float row3Y = py0 + panelH * 0.74f;
        const std::array<bool, 3> states = {
            mState.cursorEnabled.load(std::memory_order_relaxed),
            mState.greenSlotHoverEnabled.load(std::memory_order_relaxed),
            mState.paperDollFollowCursor.load(std::memory_order_relaxed)
        };
        for (int i = 0; i < 3; ++i) {
            const float bx = px0 + 16.0f + static_cast<float>(i) * (btnW + 8.0f);
            fillRectTopLeft(bx, row3Y, btnW, row1H, states[i] ? 0.13f : 0.20f, states[i] ? 0.52f : 0.15f, states[i] ? 0.28f : 0.18f, 1.0f);
            strokeRectTopLeft(bx, row3Y, btnW, row1H, 2.0f, states[i] ? 0.22f : 0.45f, states[i] ? 0.90f : 0.30f, states[i] ? 0.30f : 0.30f);
        }
    }

    // 4. Render Crisp White Windows Arrow Pointer at (cursorX, cursorY)
    if (mState.cursorEnabled.load(std::memory_order_relaxed)) {
        const float cx = mState.cursorX.load(std::memory_order_relaxed);
        const float cy = mState.cursorY.load(std::memory_order_relaxed);
        const float scale = std::clamp(mState.cursorScale.load(std::memory_order_relaxed), 0.75f, 2.00f);
        const float pixelSize = std::max(1.5f, 2.2f * scale);

        for (std::size_t row = 0; row < kWindowsCursorBitmap.size(); ++row) {
            const std::string_view line = kWindowsCursorBitmap[row];
            std::size_t col = 0;
            while (col < line.size()) {
                const char ch = line[col];
                if (ch == '0') {
                    ++col;
                    continue;
                }
                std::size_t runEnd = col + 1;
                while (runEnd < line.size() && line[runEnd] == ch) {
                    ++runEnd;
                }
                const float rx = cx + static_cast<float>(col) * pixelSize;
                const float ry = cy + static_cast<float>(row) * pixelSize;
                const float rw = static_cast<float>(runEnd - col) * pixelSize;
                if (ch == '1') {
                    // Ultra-thin dark outline (#111827) so white pointer is visible on bright & dark GUIs
                    fillRectTopLeft(rx, ry, rw, pixelSize, 0.07f, 0.09f, 0.15f, 1.0f);
                } else {
                    // Pure white Windows pointer body (#FFFFFF)
                    fillRectTopLeft(rx, ry, rw, pixelSize, 1.0f, 1.0f, 1.0f, 1.0f);
                }
                col = runEnd;
            }
        }
    }

    // Restore exact OpenGL ES state
    fnGlScissor(savedBox[0], savedBox[1], savedBox[2], savedBox[3]);
    fnGlClearColor(savedClear[0], savedClear[1], savedClear[2], savedClear[3]);
    if (!wasScissor) {
        fnGlDisable(kGlScissorTest);
    }
}

unsigned int PaperDollTouchMod::onEglSwapBuffersDetour(void *display, void *surface) noexcept {
    auto orig = reinterpret_cast<EglSwapBuffersFn>(mEglSwapBuffersOriginal);
    if (!orig)
        return 0;

    if (mState.modActive.load(std::memory_order_acquire) && display != nullptr && surface != nullptr) {
        static auto fnQuerySurface = reinterpret_cast<EglQuerySurfaceFn>(dlsym(RTLD_DEFAULT, "eglQuerySurface"));
        int w = static_cast<int>(mState.screenWidth.load(std::memory_order_relaxed));
        int h = static_cast<int>(mState.screenHeight.load(std::memory_order_relaxed));
        if (fnQuerySurface != nullptr) {
            int qw = 0;
            int qh = 0;
            if (fnQuerySurface(display, surface, kEglWidth, &qw) && qw > 0)
                w = qw;
            if (fnQuerySurface(display, surface, kEglHeight, &qh) && qh > 0)
                h = qh;
        }
        renderMobileCursorOverlay(w, h);
    }

    return orig(display, surface);
}

void PaperDollTouchMod::onPaperDollRenderGuiDetour(
    void *self,
    const void *rendererName,
    McActorRenderDataPrefix *actorRenderData,
    const void *captureDesc
) noexcept {
    auto originalFn = reinterpret_cast<PaperDollRenderGuiFn>(mPaperDollRenderGuiOriginal);
    if (originalFn == nullptr)
        return;

    if (!mState.modActive.load(std::memory_order_acquire) ||
        !mState.inInventoryRenderPass.load(std::memory_order_acquire) ||
        actorRenderData == nullptr) {
        originalFn(self, rendererName, actorRenderData, captureDesc);
        return;
    }

    const float curBodyYaw = mState.currentBodyYawOffset.load(std::memory_order_relaxed);
    const float curHeadYaw = mState.currentHeadYawOffset.load(std::memory_order_relaxed);
    const float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);

    if (std::fabs(curHeadYaw) < 0.01f && std::fabs(curPitch) < 0.01f) {
        originalFn(self, rendererName, actorRenderData, captureDesc);
        return;
    }

    applyActorPitchToComponents(actorRenderData->mActor);

    const float savedPitch = actorRenderData->rotPitch;
    const float savedYaw = actorRenderData->rotYaw;
    const float savedHeadPitch = actorRenderData->headRotPitch;
    const float savedHeadYaw = actorRenderData->headRotYaw;

    if (isValidRotationFloat(savedPitch) && isValidRotationFloat(savedYaw)) {
        actorRenderData->rotPitch = std::clamp(savedPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        if (!mState.bodyRotGetterCalledThisPass.load(std::memory_order_relaxed)) {
            actorRenderData->rotYaw = savedYaw + curBodyYaw;
        }
        if (isValidRotationFloat(savedHeadPitch)) {
            actorRenderData->headRotPitch = std::clamp(savedHeadPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        }
        if (isValidRotationFloat(savedHeadYaw) && !mState.headRotGetterCalledThisPass.load(std::memory_order_relaxed)) {
            actorRenderData->headRotYaw = savedHeadYaw + curHeadYaw;
        }
    }

    originalFn(self, rendererName, actorRenderData, captureDesc);

    actorRenderData->rotPitch = savedPitch;
    actorRenderData->rotYaw = savedYaw;
    actorRenderData->headRotPitch = savedHeadPitch;
    actorRenderData->headRotYaw = savedHeadYaw;
}

void PaperDollTouchMod::onDataDrivenRenderDetour(
    void *self,
    void *baseActorRenderContext,
    McActorRenderDataPrefix *actorRenderData
) noexcept {
    auto originalFn = reinterpret_cast<DataDrivenRenderFn>(mDataDrivenRenderOriginal);
    if (originalFn == nullptr)
        return;

    if (!mState.modActive.load(std::memory_order_acquire) ||
        !mState.inInventoryRenderPass.load(std::memory_order_acquire) ||
        actorRenderData == nullptr) {
        originalFn(self, baseActorRenderContext, actorRenderData);
        return;
    }

    const float curBodyYaw = mState.currentBodyYawOffset.load(std::memory_order_relaxed);
    const float curHeadYaw = mState.currentHeadYawOffset.load(std::memory_order_relaxed);
    const float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);

    if (std::fabs(curHeadYaw) < 0.01f && std::fabs(curPitch) < 0.01f) {
        originalFn(self, baseActorRenderContext, actorRenderData);
        return;
    }

    applyActorPitchToComponents(actorRenderData->mActor);

    const float savedPitch = actorRenderData->rotPitch;
    const float savedYaw = actorRenderData->rotYaw;
    const float savedHeadPitch = actorRenderData->headRotPitch;
    const float savedHeadYaw = actorRenderData->headRotYaw;

    if (isValidRotationFloat(savedPitch) && isValidRotationFloat(savedYaw)) {
        if (std::fabs(savedPitch) < 0.05f) {
            actorRenderData->rotPitch = std::clamp(savedPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        }
        if (!mState.bodyRotGetterCalledThisPass.load(std::memory_order_relaxed)) {
            actorRenderData->rotYaw = savedYaw + curBodyYaw;
        }
        if (isValidRotationFloat(savedHeadPitch) && std::fabs(savedHeadPitch) < 0.05f) {
            actorRenderData->headRotPitch = std::clamp(savedHeadPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        }
        if (isValidRotationFloat(savedHeadYaw) && !mState.headRotGetterCalledThisPass.load(std::memory_order_relaxed)) {
            actorRenderData->headRotYaw = savedHeadYaw + curHeadYaw;
        }
    }

    originalFn(self, baseActorRenderContext, actorRenderData);

    actorRenderData->rotPitch = savedPitch;
    actorRenderData->rotYaw = savedYaw;
    actorRenderData->headRotPitch = savedHeadPitch;
    actorRenderData->headRotYaw = savedHeadYaw;
}

float PaperDollTouchMod::onMobGetInterpolatedBodyRotDetour(const void *self, float partialTicks) noexcept {
    auto originalFn = reinterpret_cast<MobGetInterpolatedRotFn>(mMobBodyRotOriginal);
    if (!originalFn)
        return 0.0f;

    const float val = originalFn(self, partialTicks);
    if (mState.modActive.load(std::memory_order_acquire) &&
        mState.inInventoryRenderPass.load(std::memory_order_acquire)) {
        mState.bodyRotGetterCalledThisPass.store(true, std::memory_order_relaxed);
        applyActorPitchToComponents(self);
        return val + mState.currentBodyYawOffset.load(std::memory_order_relaxed);
    }
    return val;
}

float PaperDollTouchMod::onMobGetInterpolatedHeadRotDetour(const void *self, float partialTicks) noexcept {
    auto originalFn = reinterpret_cast<MobGetInterpolatedRotFn>(mMobHeadRotOriginal);
    if (!originalFn)
        return 0.0f;

    const float val = originalFn(self, partialTicks);
    if (mState.modActive.load(std::memory_order_acquire) &&
        mState.inInventoryRenderPass.load(std::memory_order_acquire)) {
        mState.headRotGetterCalledThisPass.store(true, std::memory_order_relaxed);
        applyActorPitchToComponents(self);
        return val + mState.currentHeadYawOffset.load(std::memory_order_relaxed);
    }
    return val;
}

float PaperDollTouchMod::onMobGetInterpolatedBodyYawDetour(const void *self, float partialTicks) noexcept {
    auto originalFn = reinterpret_cast<MobGetInterpolatedRotFn>(mMobBodyYawOriginal);
    if (!originalFn)
        return 0.0f;

    const float val = originalFn(self, partialTicks);
    if (mState.modActive.load(std::memory_order_acquire) &&
        mState.inInventoryRenderPass.load(std::memory_order_acquire)) {
        return val + mState.currentBodyYawOffset.load(std::memory_order_relaxed);
    }
    return val;
}

} // namespace paperdoll
