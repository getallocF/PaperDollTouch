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

// Timeout (ms) since last LivePlayerRenderer / PaperDollRenderer render call to consider Inventory open
constexpr std::int64_t kInventoryRenderTimeoutMs = 220;

bool isValidRotationFloat(float val) noexcept {
    return std::isfinite(val) && std::fabs(val) <= 36000.0f;
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
    // Automatically migrate narrow v1 hit-box bounds to v2 full-inventory character bounds
    if (mConfig.version < 2 ||
        (mConfig.boundsMinX >= 0.20f && mConfig.boundsMaxX <= 0.46f)) {
        mConfig.version = 2;
        mConfig.boundsMinX = 0.04f;
        mConfig.boundsMinY = 0.06f;
        mConfig.boundsMaxX = 0.68f;
        mConfig.boundsMaxY = 0.82f;
        if (mConfig.sensitivityYaw < 0.50f)
            mConfig.sensitivityYaw = 0.55f;
    }

    mConfig.sensitivityYaw = std::clamp(mConfig.sensitivityYaw, 0.05f, 5.0f);
    mConfig.sensitivityPitch = std::clamp(mConfig.sensitivityPitch, 0.0f, 5.0f);
    mConfig.maxPitchDegrees = std::clamp(mConfig.maxPitchDegrees, 0.0f, 85.0f);
    mConfig.smoothingFactor = std::clamp(mConfig.smoothingFactor, 0.05f, 1.0f);

    mConfig.boundsMinX = std::clamp(mConfig.boundsMinX, 0.0f, 0.95f);
    mConfig.boundsMinY = std::clamp(mConfig.boundsMinY, 0.0f, 0.95f);
    mConfig.boundsMaxX = std::clamp(mConfig.boundsMaxX, mConfig.boundsMinX + 0.05f, 1.0f);
    mConfig.boundsMaxY = std::clamp(mConfig.boundsMaxY, mConfig.boundsMinY + 0.05f, 1.0f);

    mState.activeBoundsMinX.store(mConfig.boundsMinX, std::memory_order_relaxed);
    mState.activeBoundsMinY.store(mConfig.boundsMinY, std::memory_order_relaxed);
    mState.activeBoundsMaxX.store(mConfig.boundsMaxX, std::memory_order_relaxed);
    mState.activeBoundsMaxY.store(mConfig.boundsMaxY, std::memory_order_relaxed);
}

bool PaperDollTouchMod::load() {
    auto &self = getSelf();
    self.getLogger().info("Loading PaperDollTouch v1.1.0 (RTTI Vtable Engine) for Minecraft Bedrock {} (arm64-v8a)...",
                          mConfig.expectedMinecraftVersion);

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
        self.getLogger().warn("Using default PaperDollTouch configuration values.");
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
        {"version", "1.1.0"},
        {"targetMinecraftVersion", mConfig.expectedMinecraftVersion},
        {"hookInstalled", hookReady},
        {"livePlayerRendererResolved", mLivePlayerRenderTarget != nullptr},
        {"livePlayerRendererSource", liveRenderSource},
        {"paperDollRendererResolved", mPaperDollRenderTarget != nullptr},
        {"paperDollRendererSource", paperDollSource},
        {"dataDrivenRendererResolved", mDataDrivenRenderTarget != nullptr},
        {"dataDrivenRendererSource", dataDrivenSource},
        {"mobInterpolatedRotResolved", mMobBodyRotTarget != nullptr},
        {"mobInterpolatedRotSource", mobInterpSource},
        {"plInputRegistered", mPlInputCallbackRegistered},
        {"inputQueueHookInstalled", mInputQueueHookInstalled}
    };
    out << report.dump(2) << '\n';
}

bool PaperDollTouchMod::tryResolveAndInstallHooks() noexcept {
    auto &logger = getSelf().getLogger();

    std::uintptr_t liveRenderAddr = 0;
    std::uintptr_t paperDollRenderAddr = 0;
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
        // 1. Resolve LivePlayerRenderer::render and PaperDollRenderer::render via RTTI Vtable
        // Default slot in MinecraftUICustomRenderer is 17; scan 17 first, then fallback 14..22
        constexpr std::array<std::size_t, 9> kUiRenderSlotOrder = {17, 16, 18, 15, 19, 14, 20, 21, 22};
        for (std::size_t slot : kUiRenderSlotOrder) {
            const std::uintptr_t candLive = pl::memory::resolveVtableFunction("18LivePlayerRenderer", slot);
            const std::uintptr_t candHud = pl::memory::resolveVtableFunction("17HudPlayerRenderer", slot);
            const std::uintptr_t candPaper = pl::memory::resolveVtableFunction("17PaperDollRenderer", slot);
            const std::uintptr_t candBase = pl::memory::resolveVtableFunction("25MinecraftUICustomRenderer", slot);

            if (candLive != 0 && candHud != 0 && candLive != candHud && candLive != candBase) {
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

        // Fallback if 17HudPlayerRenderer RTTI was optimized out
        if (liveRenderAddr == 0) {
            liveRenderAddr = pl::memory::resolveVtableFunction("18LivePlayerRenderer", 17);
            if (liveRenderAddr != 0)
                liveSource = "vtable:18LivePlayerRenderer#17";
        }
        if (paperDollRenderAddr == 0) {
            paperDollRenderAddr = pl::memory::resolveVtableFunction("17PaperDollRenderer", 17);
            if (paperDollRenderAddr != 0)
                paperDollSource = "vtable:17PaperDollRenderer#17";
        }

        // 2. Resolve PaperDollBaseActorRenderContext::renderGui (slot 6)
        paperDollRenderGuiAddr = pl::memory::resolveVtableFunction("31PaperDollBaseActorRenderContext", 6);

        // 3. Resolve DataDrivenRenderer::render(BaseActorRenderContext&, ActorRenderData&) (slot 3)
        constexpr std::array<std::size_t, 5> kDataDrivenSlotOrder = {3, 2, 4, 5, 6};
        for (std::size_t slot : kDataDrivenSlotOrder) {
            const std::uintptr_t candDd = pl::memory::resolveVtableFunction("18DataDrivenRenderer", slot);
            const std::uintptr_t candBase = pl::memory::resolveVtableFunction("13ActorRenderer", slot);
            if (candDd != 0 && candDd != candBase) {
                dataDrivenRenderAddr = candDd;
                dataDrivenSource = "vtable:18DataDrivenRenderer#" + std::to_string(slot);
                break;
            }
        }
        if (dataDrivenRenderAddr == 0) {
            dataDrivenRenderAddr = pl::memory::resolveVtableFunction("18DataDrivenRenderer", 3);
            if (dataDrivenRenderAddr != 0)
                dataDrivenSource = "vtable:18DataDrivenRenderer#3";
        }

        // 4. Resolve Mob::getInterpolatedBodyRot (slot 14), getInterpolatedHeadRot (slot 15), getInterpolatedBodyYaw (slot 16)
        // Verify against 5Actor where slots 12,13 are identical and 14,15,16 are overridden by 3Mob
        bool foundMobSlots = false;
        for (std::size_t baseSlot : {14ULL, 13ULL, 15ULL, 12ULL, 16ULL, 17ULL}) {
            const std::uintptr_t m0 = pl::memory::resolveVtableFunction("3Mob", baseSlot);
            const std::uintptr_t m1 = pl::memory::resolveVtableFunction("3Mob", baseSlot + 1);
            const std::uintptr_t m2 = pl::memory::resolveVtableFunction("3Mob", baseSlot + 2);
            const std::uintptr_t a0 = pl::memory::resolveVtableFunction("5Actor", baseSlot);
            const std::uintptr_t a1 = pl::memory::resolveVtableFunction("5Actor", baseSlot + 1);
            const std::uintptr_t a2 = pl::memory::resolveVtableFunction("5Actor", baseSlot + 2);
            const std::uintptr_t prevM = baseSlot > 0 ? pl::memory::resolveVtableFunction("3Mob", baseSlot - 1) : 0;
            const std::uintptr_t prevA = baseSlot > 0 ? pl::memory::resolveVtableFunction("5Actor", baseSlot - 1) : 0;

            if (m0 != 0 && m1 != 0 && m2 != 0 &&
                a0 != 0 && a1 != 0 && a2 != 0 &&
                m0 != a0 && m1 != a1 && m2 != a2 &&
                (prevM == prevA)) {
                mobBodyRotAddr = m0;
                mobHeadRotAddr = m1;
                mobBodyYawAddr = m2;
                mobInterpSource = "vtable:3Mob#" + std::to_string(baseSlot) + ".." + std::to_string(baseSlot + 2);
                foundMobSlots = true;
                break;
            }
        }
        if (!foundMobSlots) {
            mobBodyRotAddr = pl::memory::resolveVtableFunction("3Mob", 14);
            mobHeadRotAddr = pl::memory::resolveVtableFunction("3Mob", 15);
            mobBodyYawAddr = pl::memory::resolveVtableFunction("3Mob", 16);
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
            logger.info("[PaperDollTouch] Hooked LivePlayerRenderer::render ({})", liveSource);
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
            logger.info("[PaperDollTouch] Hooked PaperDollRenderer::render ({})", paperDollSource);
        } else {
            mPaperDollRenderTarget = nullptr;
            mPaperDollRenderOriginal = nullptr;
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
            logger.info("[PaperDollTouch] Hooked DataDrivenRenderer::render ({})", dataDrivenSource);
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
        logger.info("[PaperDollTouch] Hooked Mob interpolated rotation getters ({})", mobInterpSource);
    }

    if (anyRendererHooked) {
        mState.hookInstalled.store(true, std::memory_order_release);
        writeDiagnosticProbeReport(true, liveSource, paperDollSource, dataDrivenSource, mobInterpSource);
        return true;
    }
#endif

    logger.warn("[PaperDollTouch] Could not resolve Inventory renderer vtables (live={}, paperDoll={}).",
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
    // Always install AInputQueue_getEvent observer as well (with deduplication in onTouchEvent)
    // because Minecraft Bedrock Android reads native touch events directly from AInputQueue.
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
                getSelf().getLogger().info("[PaperDollTouch] Attached native AInputQueue_getEvent touch observer.");
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
        self.getLogger().info("PaperDollTouch is disabled in config.json.");
        return true;
    }

    resetTouchAndOrientation(true);
    registerInputHandlers();
    tryResolveAndInstallHooks();

    mState.modActive.store(true, std::memory_order_release);
    self.getLogger().info(
        "PaperDollTouch v1.1.0 enabled (Renderer Hooks: {}, Input Handlers: plInput={}, AInputQueue={}).",
        mState.hookInstalled.load(std::memory_order_acquire) ? "ACTIVE" : "STANDBY",
        mPlInputCallbackRegistered,
        mInputQueueHookInstalled
    );
    return true;
}

bool PaperDollTouchMod::disable() {
    mState.modActive.store(false, std::memory_order_release);
    resetTouchAndOrientation(true);
    removeHooks();
    getSelf().getLogger().info("PaperDollTouch disabled and orientation state restored.");
    return true;
}

bool PaperDollTouchMod::unload() {
    disable();
    mConfigFile.reset();
    return true;
}

void PaperDollTouchMod::resetTouchAndOrientation(bool clearAngles) noexcept {
    mState.activePointerId.store(-1, std::memory_order_relaxed);
    mState.lastTouchX.store(0.0f, std::memory_order_relaxed);
    mState.lastTouchY.store(0.0f, std::memory_order_relaxed);
    if (clearAngles) {
        mState.targetYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.targetPitchOffset.store(0.0f, std::memory_order_relaxed);
        mState.currentYawOffset.store(0.0f, std::memory_order_relaxed);
        mState.currentPitchOffset.store(0.0f, std::memory_order_relaxed);
    }
}

bool PaperDollTouchMod::isPointInsidePreviewBounds(float x, float y) const noexcept {
    const float sw = std::max(1.0f, mState.screenWidth.load(std::memory_order_relaxed));
    const float sh = std::max(1.0f, mState.screenHeight.load(std::memory_order_relaxed));

    const float nx = x / sw;
    const float ny = y / sh;

    const float minX = mState.activeBoundsMinX.load(std::memory_order_relaxed);
    const float minY = mState.activeBoundsMinY.load(std::memory_order_relaxed);
    const float maxX = mState.activeBoundsMaxX.load(std::memory_order_relaxed);
    const float maxY = mState.activeBoundsMaxY.load(std::memory_order_relaxed);

    return (nx >= minX && nx <= maxX && ny >= minY && ny <= maxY);
}

int32_t PaperDollTouchMod::onInputQueueGetEventDetour(void *queue, void **outEvent) noexcept {
    auto orig = reinterpret_cast<AInputQueueGetEventFn>(mInputQueueGetEventOriginal);
    if (!orig)
        return -1;

    const int32_t res = orig(queue, outEvent);
    if (res >= 0 && outEvent != nullptr && *outEvent != nullptr) {
        const auto *ev = static_cast<const AInputEvent *>(*outEvent);
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
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

    // Opportunistically calibrate physical screen dimensions without syscalls
    float currentSw = mState.screenWidth.load(std::memory_order_relaxed);
    float currentSh = mState.screenHeight.load(std::memory_order_relaxed);
    if (x > currentSw && x < 16384.0f)
        mState.screenWidth.store(x, std::memory_order_relaxed);
    if (y > currentSh && y < 16384.0f)
        mState.screenHeight.store(y, std::memory_order_relaxed);

    // Check if the inventory paper doll was rendered within the last 220 ms
    const std::int64_t lastRenderMs = mState.lastInventoryRenderMs.load(std::memory_order_acquire);
    const bool inventoryOpen = (lastRenderMs > 0) && ((nowMs - lastRenderMs) <= kInventoryRenderTimeoutMs);

    if (!inventoryOpen) {
        if (mState.inventoryPreviewVisible.exchange(false, std::memory_order_acq_rel)) {
            resetTouchAndOrientation(mConfig.resetOnInventoryClose);
        }
        return false;
    }

    const int trackedPointer = mState.activePointerId.load(std::memory_order_relaxed);

    switch (maskedAction) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        // Lock pointerId if no pointer is active AND touch starts inside the character preview bounds
        if (trackedPointer == -1 && isPointInsidePreviewBounds(x, y)) {
            mState.activePointerId.store(pointerId, std::memory_order_relaxed);
            mState.lastTouchX.store(x, std::memory_order_relaxed);
            mState.lastTouchY.store(y, std::memory_order_relaxed);
        }
        break;
    }

    case AMOTION_EVENT_ACTION_MOVE: {
        if (trackedPointer != -1 && pointerId == trackedPointer) {
            const float prevX = mState.lastTouchX.load(std::memory_order_relaxed);
            const float prevY = mState.lastTouchY.load(std::memory_order_relaxed);

            const float deltaX = x - prevX;
            const float deltaY = y - prevY;

            mState.lastTouchX.store(x, std::memory_order_relaxed);
            mState.lastTouchY.store(y, std::memory_order_relaxed);

            if (std::fabs(deltaX) > 0.15f || std::fabs(deltaY) > 0.15f) {
                float yaw = mState.targetYawOffset.load(std::memory_order_relaxed);
                float pitch = mState.targetPitchOffset.load(std::memory_order_relaxed);

                yaw -= deltaX * mConfig.sensitivityYaw;
                yaw = std::remainder(yaw, 360.0f);

                const float pitchSign = mConfig.invertVertical ? -1.0f : 1.0f;
                pitch += (deltaY * pitchSign) * mConfig.sensitivityPitch;
                pitch = std::clamp(pitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);

                mState.targetYawOffset.store(yaw, std::memory_order_relaxed);
                mState.targetPitchOffset.store(pitch, std::memory_order_relaxed);
            }
        }
        break;
    }

    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL: {
        if (trackedPointer != -1 && pointerId == trackedPointer) {
            mState.activePointerId.store(-1, std::memory_order_relaxed);
        }
        break;
    }

    default:
        break;
    }

    // Never consume events so Minecraft inventory slots, armor, and crafting UI work seamlessly
    return false;
}

void PaperDollTouchMod::stepOrientationSmoothing() noexcept {
    mState.lastInventoryRenderMs.store(monotonicMillis(), std::memory_order_release);
    mState.inventoryPreviewVisible.store(true, std::memory_order_release);
    mState.renderedPreviewFrames.fetch_add(1, std::memory_order_relaxed);

    const float targetYaw = mState.targetYawOffset.load(std::memory_order_relaxed);
    const float targetPitch = mState.targetPitchOffset.load(std::memory_order_relaxed);
    float curYaw = mState.currentYawOffset.load(std::memory_order_relaxed);
    float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);

    const float alpha = mConfig.smoothingFactor;
    curYaw += (targetYaw - curYaw) * alpha;
    curPitch += (targetPitch - curPitch) * alpha;

    mState.currentYawOffset.store(curYaw, std::memory_order_relaxed);
    mState.currentPitchOffset.store(curPitch, std::memory_order_relaxed);
}

void PaperDollTouchMod::applyPaperDollRendererFields(void *paperDollRenderer) noexcept {
    if (paperDollRenderer == nullptr)
        return;

    const float curYaw = mState.currentYawOffset.load(std::memory_order_relaxed);
    const float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);
    if (std::fabs(curYaw) < 0.01f && std::fabs(curPitch) < 0.01f)
        return;

    // PaperDollRenderer POD field layout (64-bit ARM64):
    // +0x0C: int mRotationStyle (CustomY = 3)
    // +0x20: float mRotation
    // +0x24: float mTargetRotation
    // +0x28: float mCameraTiltInDeg
    auto *base = static_cast<std::uint8_t *>(paperDollRenderer);
    auto *rotStyle = reinterpret_cast<int *>(base + 0x0C);
    auto *rot = reinterpret_cast<float *>(base + 0x20);
    auto *targetRot = reinterpret_cast<float *>(base + 0x24);
    auto *tilt = reinterpret_cast<float *>(base + 0x28);

    if (*rotStyle >= 0 && *rotStyle <= 6 &&
        isValidRotationFloat(*rot) &&
        isValidRotationFloat(*targetRot) &&
        isValidRotationFloat(*tilt)) {
        *rotStyle = 3; // CustomY
        *rot += curYaw;
        *targetRot = *rot;
        *tilt = std::clamp(*tilt + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
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
    ScopedInventoryPass passGuard(mState.inInventoryRenderPass);
    originalFn(self, renderContext, clientInstance, uiControl, pass);
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
    applyPaperDollRendererFields(self);
    ScopedInventoryPass passGuard(mState.inInventoryRenderPass);
    originalFn(self, renderContext, clientInstance, uiControl, pass);
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

    const float curYaw = mState.currentYawOffset.load(std::memory_order_relaxed);
    const float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);

    if (std::fabs(curYaw) < 0.01f && std::fabs(curPitch) < 0.01f) {
        originalFn(self, rendererName, actorRenderData, captureDesc);
        return;
    }

    const float savedPitch = actorRenderData->rotPitch;
    const float savedYaw = actorRenderData->rotYaw;
    const float savedHeadPitch = actorRenderData->headRotPitch;
    const float savedHeadYaw = actorRenderData->headRotYaw;

    if (isValidRotationFloat(savedPitch) && isValidRotationFloat(savedYaw)) {
        actorRenderData->rotPitch = std::clamp(savedPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        actorRenderData->rotYaw = savedYaw + curYaw;
        if (isValidRotationFloat(savedHeadPitch))
            actorRenderData->headRotPitch = std::clamp(savedHeadPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        if (isValidRotationFloat(savedHeadYaw))
            actorRenderData->headRotYaw = savedHeadYaw + curYaw;
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

    // Only apply rotation override during LivePlayerRenderer::render or PaperDollRenderer::render
    if (!mState.modActive.load(std::memory_order_acquire) ||
        !mState.inInventoryRenderPass.load(std::memory_order_acquire) ||
        actorRenderData == nullptr) {
        originalFn(self, baseActorRenderContext, actorRenderData);
        return;
    }

    const float curYaw = mState.currentYawOffset.load(std::memory_order_relaxed);
    const float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);

    if (std::fabs(curYaw) < 0.01f && std::fabs(curPitch) < 0.01f) {
        originalFn(self, baseActorRenderContext, actorRenderData);
        return;
    }

    const float savedPitch = actorRenderData->rotPitch;
    const float savedYaw = actorRenderData->rotYaw;
    const float savedHeadPitch = actorRenderData->headRotPitch;
    const float savedHeadYaw = actorRenderData->headRotYaw;

    // Also locate ActorRotationComponent (float mPitch, mYaw, mOldPitch, mOldYaw) inside mActor's BuiltInActorComponents
    // so Molang queries (query.head_x_rotation, query.head_y_rotation, query.body_y_rotation) rotate in tandem.
    float *actorRotCompFloats = nullptr;
    float savedCompFloats[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    if (actorRenderData->mActor != nullptr &&
        isValidRotationFloat(savedPitch) &&
        isValidRotationFloat(savedYaw)) {
        auto *actorBytes = static_cast<std::uint8_t *>(actorRenderData->mActor);
        const std::uintptr_t actorAddr = reinterpret_cast<std::uintptr_t>(actorBytes);
        // BuiltInActorComponents is embedded inside Actor within +0x18..+0x98
        for (std::size_t off = 0x18; off <= 0x98; off += sizeof(void *)) {
            void *candPtr = *reinterpret_cast<void **>(actorBytes + off);
            const std::uintptr_t candAddr = reinterpret_cast<std::uintptr_t>(candPtr);
            if (candAddr > 0x10000ULL && (candAddr & 0x3ULL) == 0ULL &&
                std::llabs(static_cast<long long>(candAddr) - static_cast<long long>(actorAddr)) < 0x40000000LL) {
                auto *f = static_cast<float *>(candPtr);
                if (std::isfinite(f[0]) && std::isfinite(f[1]) &&
                    std::fabs(f[0] - savedPitch) < 0.05f &&
                    std::fabs(f[1] - savedYaw) < 0.05f &&
                    isValidRotationFloat(f[2]) && isValidRotationFloat(f[3])) {
                    actorRotCompFloats = f;
                    savedCompFloats[0] = f[0];
                    savedCompFloats[1] = f[1];
                    savedCompFloats[2] = f[2];
                    savedCompFloats[3] = f[3];
                    const float newCompPitch = std::clamp(savedCompFloats[0] + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
                    const float newCompYaw = savedCompFloats[1] + curYaw;
                    f[0] = newCompPitch;
                    f[1] = newCompYaw;
                    f[2] = newCompPitch;
                    f[3] = newCompYaw;
                    break;
                }
            }
        }
    }

    if (isValidRotationFloat(savedPitch) && isValidRotationFloat(savedYaw)) {
        actorRenderData->rotPitch = std::clamp(savedPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        actorRenderData->rotYaw = savedYaw + curYaw;
        if (isValidRotationFloat(savedHeadPitch))
            actorRenderData->headRotPitch = std::clamp(savedHeadPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        if (isValidRotationFloat(savedHeadYaw))
            actorRenderData->headRotYaw = savedHeadYaw + curYaw;
    }

    originalFn(self, baseActorRenderContext, actorRenderData);

    actorRenderData->rotPitch = savedPitch;
    actorRenderData->rotYaw = savedYaw;
    actorRenderData->headRotPitch = savedHeadPitch;
    actorRenderData->headRotYaw = savedHeadYaw;

    if (actorRotCompFloats != nullptr) {
        actorRotCompFloats[0] = savedCompFloats[0];
        actorRotCompFloats[1] = savedCompFloats[1];
        actorRotCompFloats[2] = savedCompFloats[2];
        actorRotCompFloats[3] = savedCompFloats[3];
    }
}

float PaperDollTouchMod::onMobGetInterpolatedBodyRotDetour(const void *self, float partialTicks) noexcept {
    auto originalFn = reinterpret_cast<MobGetInterpolatedRotFn>(mMobBodyRotOriginal);
    if (!originalFn)
        return 0.0f;

    const float val = originalFn(self, partialTicks);
    if (mState.modActive.load(std::memory_order_acquire) &&
        mState.inInventoryRenderPass.load(std::memory_order_acquire)) {
        return val + mState.currentYawOffset.load(std::memory_order_relaxed);
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
        return val + mState.currentYawOffset.load(std::memory_order_relaxed);
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
        return val + mState.currentYawOffset.load(std::memory_order_relaxed);
    }
    return val;
}

} // namespace paperdoll
