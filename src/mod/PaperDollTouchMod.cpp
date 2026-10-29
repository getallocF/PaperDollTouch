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
constexpr std::int64_t kInventoryRenderTimeoutMs = 250;

// Normalized screen position of the Paper Doll eye/chest center in Inventory UI
constexpr float kPaperDollAnchorX = 0.28f;
constexpr float kPaperDollAnchorY = 0.34f;

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
    // Automatically migrate v1/v2 turntable configs to v3 full-screen Cursor-Follow mode
    if (mConfig.version < 3) {
        mConfig.version = 3;
        mConfig.boundsMinX = 0.0f;
        mConfig.boundsMinY = 0.0f;
        mConfig.boundsMaxX = 1.0f;
        mConfig.boundsMaxY = 1.0f;
        mConfig.sensitivityYaw = 1.0f;
        mConfig.sensitivityPitch = 1.0f;
        mConfig.smoothingFactor = 0.45f;
    }

    mConfig.sensitivityYaw = std::clamp(mConfig.sensitivityYaw, 0.1f, 5.0f);
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
    self.getLogger().info("Loading PaperDollTouch v1.3.0 (MC Windows Head+Body Cursor-Follow) for Minecraft Bedrock {} (arm64-v8a)...",
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
        {"version", "1.3.0"},
        {"mode", "cursor_follow_head_and_body"},
        {"targetMinecraftVersion", mConfig.expectedMinecraftVersion},
        {"hookInstalled", hookReady},
        {"livePlayerRendererResolved", mLivePlayerRenderTarget != nullptr},
        {"livePlayerRendererSource", liveRenderSource},
        {"paperDollRendererResolved", mPaperDollRenderTarget != nullptr},
        {"paperDollRendererSource", paperDollSource},
        {"dataDrivenRendererResolved", mDataDrivenRenderTarget != nullptr},
        {"dataDrivenRendererSource", dataDrivenSource},
        {"mobInterpolatedRotResolved", mMobBodyRotTarget != nullptr},
        {"mobInterpolatedHeadRotResolved", mMobHeadRotTarget != nullptr},
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
        auto resolveMcVtable = [](std::string_view typeInfoName, std::size_t slot) noexcept -> std::uintptr_t {
            return pl::memory::resolveVtableFunction(typeInfoName, slot, "libminecraftpe.so");
        };

        // 1. Resolve LivePlayerRenderer::render and PaperDollRenderer::render via RTTI Vtable
        // Default slot in MinecraftUICustomRenderer is 17; scan 17 first, then fallback 14..22
        constexpr std::array<std::size_t, 9> kUiRenderSlotOrder = {17, 16, 18, 15, 19, 14, 20, 21, 22};
        for (std::size_t slot : kUiRenderSlotOrder) {
            const std::uintptr_t candLive = resolveMcVtable("18LivePlayerRenderer", slot);
            const std::uintptr_t candHud = resolveMcVtable("17HudPlayerRenderer", slot);
            const std::uintptr_t candPaper = resolveMcVtable("17PaperDollRenderer", slot);
            const std::uintptr_t candBase = resolveMcVtable("25MinecraftUICustomRenderer", slot);

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
            liveRenderAddr = resolveMcVtable("18LivePlayerRenderer", 17);
            if (liveRenderAddr != 0)
                liveSource = "vtable:18LivePlayerRenderer#17";
        }
        if (paperDollRenderAddr == 0) {
            paperDollRenderAddr = resolveMcVtable("17PaperDollRenderer", 17);
            if (paperDollRenderAddr != 0)
                paperDollSource = "vtable:17PaperDollRenderer#17";
        }

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
        // In Bedrock 26.60, 3Mob overrides getInterpolatedBodyRot (slot 14, m0 != a0) and getInterpolatedBodyYaw (slot 16, m2 != a2),
        // while getInterpolatedHeadRot (slot 15, m1 == a1) is implemented directly on 5Actor using ActorHeadRotationComponent!
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
        // Fallback if 3Mob also overrides getInterpolatedHeadRot (require 2 preceding slots to match 5Actor so slot 16 cannot false-match)
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
        logger.info("[PaperDollTouch] Hooked Mob/Actor interpolated body+head rotation getters ({})", mobInterpSource);
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
        "PaperDollTouch v1.3.0 (MC Windows Head+Body Cursor-Follow) enabled (Renderer Hooks: {}, Input Handlers: plInput={}, AInputQueue={}).",
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
    const float sw = std::max(1.0f, mState.screenWidth.load(std::memory_order_relaxed));
    const float sh = std::max(1.0f, mState.screenHeight.load(std::memory_order_relaxed));

    const float nx = std::clamp(x / sw, 0.0f, 1.0f);
    const float ny = std::clamp(y / sh, 0.0f, 1.0f);

    // Offset from the Paper Doll's eye/chest center in Inventory UI
    const float dx = nx - kPaperDollAnchorX;
    const float dy = ny - kPaperDollAnchorY;

    // Authentic Minecraft Windows (InventoryScreen) atan cursor-look curve:
    // atan(delta * scale) provides responsive tracking near the doll and natural saturation at screen edges
    const float atanX = std::atan(dx * 3.8f);
    const float atanY = std::atan(dy * 3.8f);

    // Body turns partially (~26 deg) while Head turns fully (~62 deg, so ~36 deg neck yaw relative to chest)
    // toward the touch point, creating authentic Minecraft Windows head + torso cursor tracking!
    const float bodyYaw = -atanX * (26.0f * mConfig.sensitivityYaw);
    const float headYaw = -atanX * (62.0f * mConfig.sensitivityYaw);

    const float pitchSign = mConfig.invertVertical ? -1.0f : 1.0f;
    const float rawPitch = (atanY * pitchSign) * (38.0f * mConfig.sensitivityPitch);
    const float pitch = std::clamp(rawPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);

    mState.targetBodyYawOffset.store(bodyYaw, std::memory_order_relaxed);
    mState.targetHeadYawOffset.store(headYaw, std::memory_order_relaxed);
    mState.targetPitchOffset.store(pitch, std::memory_order_relaxed);
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
        mState.screenWidth.store(x * 1.02f, std::memory_order_relaxed);
    }
    if (y > currentSh && y < 16384.0f) {
        mState.screenHeight.store(y * 1.02f, std::memory_order_relaxed);
    }

    // Check if the inventory paper doll was rendered within the last 250 ms
    const std::int64_t lastRenderMs = mState.lastInventoryRenderMs.load(std::memory_order_acquire);
    const bool inventoryOpen = (lastRenderMs > 0) && ((nowMs - lastRenderMs) <= kInventoryRenderTimeoutMs);

    if (!inventoryOpen) {
        if (mState.inventoryPreviewVisible.exchange(false, std::memory_order_acq_rel)) {
            resetTouchAndOrientation(mConfig.resetOnInventoryClose);
        }
        return false;
    }

    switch (maskedAction) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        if (isPointInsidePreviewBounds(x, y)) {
            mState.activePointerId.store(pointerId, std::memory_order_relaxed);
            mState.lastTouchX.store(x, std::memory_order_relaxed);
            mState.lastTouchY.store(y, std::memory_order_relaxed);
            // Immediately point the Paper Doll head + body toward the touched location like MC Windows
            updateCursorFollowTargets(x, y);
        }
        break;
    }

    case AMOTION_EVENT_ACTION_MOVE: {
        const int trackedPointer = mState.activePointerId.load(std::memory_order_relaxed);
        if (trackedPointer == -1 || pointerId == trackedPointer) {
            if (trackedPointer == -1) {
                mState.activePointerId.store(pointerId, std::memory_order_relaxed);
            }
            mState.lastTouchX.store(x, std::memory_order_relaxed);
            mState.lastTouchY.store(y, std::memory_order_relaxed);
            // Continuously track finger position across the entire Inventory screen like a cursor
            updateCursorFollowTargets(x, y);
        }
        break;
    }

    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL: {
        const int trackedPointer = mState.activePointerId.load(std::memory_order_relaxed);
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
    // AABBShapeComponent has mMin (f[0..2]), mMax (f[3..5]), mWidth (f[6] in 0.2..3.0), mHeight (f[7] in 0.2..4.0)
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

    // Only apply cursor-follow look during LivePlayerRenderer::render or PaperDollRenderer::render
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
