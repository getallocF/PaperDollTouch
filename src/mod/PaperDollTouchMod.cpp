#include "mod/PaperDollTouchMod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <elf.h>
#include <fstream>
#include <link.h>
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

// AArch64 AAPCS64 function pointer types for Minecraft Bedrock internal methods
using HudPlayerRendererRenderFn = void (*)(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass,
    McRectangleArea *rectArea
);

using GetLocalPlayerFn = void *(*)(void *clientInstance);
using ActorGetRotationFn = const McVec2 *(*)(const void *actor);
using ActorGetYHeadRotFn = float (*)(const void *actor);
using ActorSetYHeadRotFn = void (*)(void *actor, float yaw);
using AInputQueueGetEventFn = int32_t (*)(void *queue, void **outEvent);

// Timeout (ms) since last HudPlayerRenderer::render call to consider the inventory screen open
constexpr std::int64_t kInventoryRenderTimeoutMs = 160;

// Minimum UI control dimensions to distinguish Inventory Paper Doll from top-left HUD Paper Doll
constexpr float kMinInventoryPreviewWidthUi = 28.0f;
constexpr float kMinInventoryPreviewHeightUi = 40.0f;

// Known Itanium C++ ABI mangled symbol candidates in libminecraftpe.so (ARM64)
constexpr std::array<std::string_view, 4> kHudPlayerRenderSymbolCandidates = {
    "_ZN17HudPlayerRenderer6renderER24MinecraftUIRenderContextR15IClientInstanceR9UIControliR13RectangleArea",
    "_ZN17HudPlayerRenderer6renderER24MinecraftUIRenderContextR14ClientInstanceR9UIControliR13RectangleArea",
    "_ZN17HudPlayerRenderer6renderER24MinecraftUIRenderContextR15IClientInstanceR9UIControliRN3mce13RectangleAreaE",
    "_ZN17HudPlayerRenderer6renderEP24MinecraftUIRenderContextP15IClientInstanceP9UIControliP13RectangleArea"
};

constexpr std::array<std::string_view, 4> kGetLocalPlayerSymbolCandidates = {
    "_ZNK14ClientInstance14getLocalPlayerEv",
    "_ZN14ClientInstance14getLocalPlayerEv",
    "_ZNK15IClientInstance14getLocalPlayerEv",
    "_ZN15IClientInstance14getLocalPlayerEv"
};

constexpr std::array<std::string_view, 2> kActorGetRotationSymbolCandidates = {
    "_ZNK5Actor11getRotationEv",
    "_ZN5Actor11getRotationEv"
};

// One-time read-only ELF64 .dynsym probe via dl_iterate_phdr for libminecraftpe.so
struct ElfProbeResult {
    std::uintptr_t baseAddr{0};
    std::uintptr_t hudRenderAddr{0};
    std::string hudRenderSym{};
    std::uintptr_t getLocalPlayerAddr{0};
    std::string getLocalPlayerSym{};
    std::uintptr_t actorGetRotAddr{0};
    std::string actorGetRotSym{};
    std::uintptr_t actorGetHeadRotAddr{0};
    std::uintptr_t actorSetHeadRotAddr{0};
    std::size_t scannedDynamicSymbols{0};
};

int elfPhdrCallback(struct dl_phdr_info *info, size_t /*size*/, void *data) noexcept {
    if (!info || !info->dlpi_name || !data)
        return 0;

    std::string_view modName(info->dlpi_name);
    if (modName.find("libminecraftpe.so") == std::string_view::npos)
        return 0;

    auto *result = static_cast<ElfProbeResult *>(data);
    result->baseAddr = static_cast<std::uintptr_t>(info->dlpi_addr);

    const Elf64_Dyn *dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const Elf64_Dyn *>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn)
        return 1;

    const Elf64_Sym *symtab = nullptr;
    const char *strtab = nullptr;
    std::size_t strsz = 0;
    const std::uint32_t *sysvHash = nullptr;
    const std::uint32_t *gnuHash = nullptr;

    for (const Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
        case DT_SYMTAB:
            symtab = reinterpret_cast<const Elf64_Sym *>(
                d->d_un.d_ptr > info->dlpi_addr ? d->d_un.d_ptr : (info->dlpi_addr + d->d_un.d_ptr));
            break;
        case DT_STRTAB:
            strtab = reinterpret_cast<const char *>(
                d->d_un.d_ptr > info->dlpi_addr ? d->d_un.d_ptr : (info->dlpi_addr + d->d_un.d_ptr));
            break;
        case DT_STRSZ:
            strsz = static_cast<std::size_t>(d->d_un.d_val);
            break;
        case DT_HASH:
            sysvHash = reinterpret_cast<const std::uint32_t *>(
                d->d_un.d_ptr > info->dlpi_addr ? d->d_un.d_ptr : (info->dlpi_addr + d->d_un.d_ptr));
            break;
        case DT_GNU_HASH:
            gnuHash = reinterpret_cast<const std::uint32_t *>(
                d->d_un.d_ptr > info->dlpi_addr ? d->d_un.d_ptr : (info->dlpi_addr + d->d_un.d_ptr));
            break;
        default:
            break;
        }
    }

    if (!symtab || !strtab || strsz == 0)
        return 1;

    std::size_t symCount = 0;
    if (sysvHash) {
        symCount = sysvHash[1]; // nchain
    } else if (gnuHash) {
        const std::uint32_t nbuckets = gnuHash[0];
        const std::uint32_t symoffset = gnuHash[1];
        const std::uint32_t bloomSize = gnuHash[2];
        const auto *bloom = reinterpret_cast<const std::uint64_t *>(&gnuHash[4]);
        const auto *buckets = reinterpret_cast<const std::uint32_t *>(&bloom[bloomSize]);
        const auto *chain = &buckets[nbuckets];

        std::uint32_t maxSym = symoffset;
        for (std::uint32_t b = 0; b < nbuckets; ++b) {
            if (buckets[b] > maxSym)
                maxSym = buckets[b];
        }
        if (maxSym >= symoffset) {
            while ((chain[maxSym - symoffset] & 1U) == 0U) {
                ++maxSym;
            }
            symCount = static_cast<std::size_t>(maxSym) + 1U;
        }
    }

    result->scannedDynamicSymbols = symCount;
    for (std::size_t idx = 1; idx < symCount; ++idx) {
        const Elf64_Sym &sym = symtab[idx];
        if (sym.st_value == 0 || sym.st_name == 0 || sym.st_name >= strsz)
            continue;

        std::string_view sname(strtab + sym.st_name);
        const std::uintptr_t resolvedAddr = info->dlpi_addr + sym.st_value;

        if (result->hudRenderAddr == 0 &&
            sname.find("HudPlayerRenderer") != std::string_view::npos &&
            sname.find("render") != std::string_view::npos) {
            result->hudRenderAddr = resolvedAddr;
            result->hudRenderSym = std::string(sname);
        } else if (result->getLocalPlayerAddr == 0 &&
                   (sname.find("ClientInstance") != std::string_view::npos) &&
                   (sname.find("getLocalPlayer") != std::string_view::npos)) {
            result->getLocalPlayerAddr = resolvedAddr;
            result->getLocalPlayerSym = std::string(sname);
        } else if (result->actorGetRotAddr == 0 && sname == "_ZNK5Actor11getRotationEv") {
            result->actorGetRotAddr = resolvedAddr;
            result->actorGetRotSym = std::string(sname);
        } else if (result->actorGetHeadRotAddr == 0 && sname == "_ZNK5Actor11getYHeadRotEv") {
            result->actorGetHeadRotAddr = resolvedAddr;
        } else if (result->actorSetHeadRotAddr == 0 && sname == "_ZN5Actor11setYHeadRotEf") {
            result->actorSetHeadRotAddr = resolvedAddr;
        }
    }

    return 1;
}

void detourHudPlayerRendererRender(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass,
    McRectangleArea *rectArea
) noexcept {
    PaperDollTouchMod::instance().onHudPlayerRenderDetour(
        self, renderContext, clientInstance, uiControl, pass, rectArea
    );
}

int32_t detourAInputQueueGetEvent(void *queue, void **outEvent) noexcept {
    return PaperDollTouchMod::instance().onInputQueueGetEventDetour(queue, outEvent);
}

#if PDT_HAS_PL_INPUT
// C++20 concept wrapper so compilation succeeds regardless of minor pl::input overload differences
template <typename CallbackT>
bool tryRegisterPreloaderTouchCallback(CallbackT &&cb) noexcept {
    if constexpr (requires { pl::input::registerTouchCallback(std::forward<CallbackT>(cb)); }) {
        pl::input::registerTouchCallback(std::forward<CallbackT>(cb));
        return true;
    }
    return false;
}
#endif

bool isValidRotationFloat(float val) noexcept {
    return std::isfinite(val) && std::fabs(val) <= 36000.0f;
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
    self.getLogger().info("Loading PaperDollTouch v1.0.0 for Minecraft Bedrock {} (arm64-v8a)...",
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
    const std::string &renderSymSource,
    const std::string &localPlayerSymSource,
    const std::string &rotationAccessMode
) const noexcept {
    std::error_code ec;
    const auto reportPath = getSelf().getDataDir() / "symbol_probe_26_60.json";
    std::ofstream out(reportPath, std::ios::binary | std::ios::trunc);
    if (!out)
        return;

    nlohmann::json report = {
        {"mod", "PaperDollTouch"},
        {"version", "1.0.0"},
        {"targetMinecraftVersion", mConfig.expectedMinecraftVersion},
        {"hookInstalled", hookReady},
        {"hudPlayerRendererRenderResolved", mHudPlayerRenderTarget != nullptr},
        {"hudPlayerRendererRenderSource", renderSymSource},
        {"getLocalPlayerResolved", mGetLocalPlayerFn != nullptr},
        {"getLocalPlayerSource", localPlayerSymSource},
        {"actorGetRotationResolved", mActorGetRotationFn != nullptr},
        {"rotationAccessMode", rotationAccessMode},
        {"inputHandlerReady", mPlInputCallbackRegistered || mInputQueueHookInstalled}
    };
    out << report.dump(2) << '\n';
}

bool PaperDollTouchMod::tryResolveAndInstallHooks() noexcept {
    auto &logger = getSelf().getLogger();

    std::uintptr_t renderAddr = 0;
    std::uintptr_t getLocalPlayerAddr = 0;
    std::uintptr_t actorGetRotAddr = 0;
    std::string renderSource = "unresolved";
    std::string localPlayerSource = "unresolved";

#if PDT_HAS_PL_SIG
    // 1. User-configured signature or symbol in config.json takes highest precedence
    if (!mConfig.hudPlayerRendererRenderSig.empty()) {
        renderAddr = pl::memory::resolveSignature(mConfig.hudPlayerRendererRenderSig, "libminecraftpe.so");
        if (renderAddr != 0) {
            renderSource = "config:hudPlayerRendererRenderSig";
        }
    }
    if (!mConfig.getLocalPlayerFromClientSig.empty()) {
        getLocalPlayerAddr = pl::memory::resolveSignature(mConfig.getLocalPlayerFromClientSig, "libminecraftpe.so");
        if (getLocalPlayerAddr != 0) {
            localPlayerSource = "config:getLocalPlayerFromClientSig";
        }
    }
    if (!mConfig.actorGetRotationSig.empty()) {
        actorGetRotAddr = pl::memory::resolveSignature(mConfig.actorGetRotationSig, "libminecraftpe.so");
    }

    // 2. Auto-probe known Bedrock ARM64 mangled symbol candidates via pl::memory::resolveSignature
    if (mConfig.allowSymbolAutoProbe) {
        if (renderAddr == 0) {
            for (const auto &sym : kHudPlayerRenderSymbolCandidates) {
                renderAddr = pl::memory::resolveSignature(sym, "libminecraftpe.so");
                if (renderAddr != 0) {
                    renderSource = std::string("pl::resolveSignature:") + std::string(sym);
                    break;
                }
            }
        }
        if (getLocalPlayerAddr == 0) {
            for (const auto &sym : kGetLocalPlayerSymbolCandidates) {
                getLocalPlayerAddr = pl::memory::resolveSignature(sym, "libminecraftpe.so");
                if (getLocalPlayerAddr != 0) {
                    localPlayerSource = std::string("pl::resolveSignature:") + std::string(sym);
                    break;
                }
            }
        }
        if (actorGetRotAddr == 0) {
            for (const auto &sym : kActorGetRotationSymbolCandidates) {
                actorGetRotAddr = pl::memory::resolveSignature(sym, "libminecraftpe.so");
                if (actorGetRotAddr != 0)
                    break;
            }
        }
    }
#endif

    // 3. One-time read-only ELF64 .dynsym scan via dl_iterate_phdr
    if (mConfig.allowSymbolAutoProbe && (renderAddr == 0 || getLocalPlayerAddr == 0 || actorGetRotAddr == 0)) {
        ElfProbeResult probe{};
        dl_iterate_phdr(&elfPhdrCallback, &probe);
        if (renderAddr == 0 && probe.hudRenderAddr != 0) {
            renderAddr = probe.hudRenderAddr;
            renderSource = "elf_dynsym:" + probe.hudRenderSym;
        }
        if (getLocalPlayerAddr == 0 && probe.getLocalPlayerAddr != 0) {
            getLocalPlayerAddr = probe.getLocalPlayerAddr;
            localPlayerSource = "elf_dynsym:" + probe.getLocalPlayerSym;
        }
        if (actorGetRotAddr == 0 && probe.actorGetRotAddr != 0) {
            actorGetRotAddr = probe.actorGetRotAddr;
        }
        if (probe.actorGetHeadRotAddr != 0) {
            mActorGetYHeadRotFn = reinterpret_cast<void *>(probe.actorGetHeadRotAddr);
        }
        if (probe.actorSetHeadRotAddr != 0) {
            mActorSetYHeadRotFn = reinterpret_cast<void *>(probe.actorSetHeadRotAddr);
        }
    }

    const bool hasValidStructOffsets =
        mConfig.verifiedSignaturesReady &&
        mConfig.actorRotYawOffset > 0 &&
        mConfig.actorRotPitchOffset > 0 &&
        (mConfig.actorRotYawOffset % 4 == 0) &&
        (mConfig.actorRotPitchOffset % 4 == 0);

    const bool hasRotationAccess = (actorGetRotAddr != 0) || hasValidStructOffsets;
    std::string rotationMode = "none (awaiting Actor::getRotation symbol or verified offsets)";
    if (actorGetRotAddr != 0) {
        rotationMode = "symbol:Actor::getRotation()";
    } else if (hasValidStructOffsets) {
        rotationMode = "offsets:verified_actor_float_offsets";
    }

    if (renderAddr == 0 || getLocalPlayerAddr == 0 || !hasRotationAccess) {
        logger.warn(
            "[PaperDollTouch] Hook prerequisites incomplete for Minecraft {} "
            "(HudPlayerRenderer::render={}, getLocalPlayer={}, rotationAccess={}). "
            "Staying in fail-safe mode to prevent crash. Diagnostic saved to symbol_probe_26_60.json.",
            mConfig.expectedMinecraftVersion,
            renderSource,
            localPlayerSource,
            rotationMode
        );
        writeDiagnosticProbeReport(false, renderSource, localPlayerSource, rotationMode);
        return false;
    }

#if PDT_HAS_PL_HOOK
    mHudPlayerRenderTarget = reinterpret_cast<void *>(renderAddr);
    mGetLocalPlayerFn = reinterpret_cast<void *>(getLocalPlayerAddr);
    mActorGetRotationFn = reinterpret_cast<void *>(actorGetRotAddr);

    int status = pl::memory::hook(
        mHudPlayerRenderTarget,
        reinterpret_cast<void *>(&detourHudPlayerRendererRender),
        &mHudPlayerRenderOriginal,
        pl::memory::HookPriority::Normal
    );

    if (status != 0 || mHudPlayerRenderOriginal == nullptr) {
        logger.error("[PaperDollTouch] pl::memory::hook failed for HudPlayerRenderer::render (status={})", status);
        mHudPlayerRenderTarget = nullptr;
        mHudPlayerRenderOriginal = nullptr;
        mGetLocalPlayerFn = nullptr;
        mActorGetRotationFn = nullptr;
        writeDiagnosticProbeReport(false, renderSource, localPlayerSource, rotationMode);
        return false;
    }

    mState.hookInstalled.store(true, std::memory_order_release);
    logger.info(
        "[PaperDollTouch] Installed HudPlayerRenderer::render hook ({}), getLocalPlayer ({}), rotation mode ({}).",
        renderSource, localPlayerSource, rotationMode
    );
    writeDiagnosticProbeReport(true, renderSource, localPlayerSource, rotationMode);
    return true;
#else
    writeDiagnosticProbeReport(false, renderSource, localPlayerSource, rotationMode);
    return false;
#endif
}

void PaperDollTouchMod::registerInputHandlers() noexcept {
    if (!mPlInputCallbackRegistered) {
#if PDT_HAS_PL_INPUT
        mPlInputCallbackRegistered = tryRegisterPreloaderTouchCallback(
            [](const auto &event) -> bool {
                return PaperDollTouchMod::instance().onTouchEvent(
                    static_cast<int>(event.action),
                    static_cast<int>(event.pointerId),
                    static_cast<float>(event.x),
                    static_cast<float>(event.y)
                );
            }
        );
#endif
    }

#if PDT_HAS_PL_HOOK
    if (!mPlInputCallbackRegistered && mConfig.allowNativeInputQueueFallback && !mInputQueueHookInstalled) {
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
                getSelf().getLogger().info("[PaperDollTouch] Attached non-intrusive AInputQueue_getEvent touch observer.");
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
    if (mState.hookInstalled.load(std::memory_order_acquire) && mHudPlayerRenderTarget != nullptr) {
        pl::memory::unhook(
            mHudPlayerRenderTarget,
            reinterpret_cast<void *>(&detourHudPlayerRendererRender)
        );
    }
    if (mInputQueueHookInstalled && mInputQueueGetEventTarget != nullptr) {
        pl::memory::unhook(
            mInputQueueGetEventTarget,
            reinterpret_cast<void *>(&detourAInputQueueGetEvent)
        );
        mInputQueueHookInstalled = false;
        mInputQueueGetEventTarget = nullptr;
        mInputQueueGetEventOriginal = nullptr;
    }
#endif
    mState.hookInstalled.store(false, std::memory_order_release);
    mHudPlayerRenderTarget = nullptr;
    mHudPlayerRenderOriginal = nullptr;
    mGetLocalPlayerFn = nullptr;
    mActorGetRotationFn = nullptr;
    mActorGetYHeadRotFn = nullptr;
    mActorSetYHeadRotFn = nullptr;
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
        "PaperDollTouch enabled (Renderer Hook: {}, Input Handler: {}).",
        mState.hookInstalled.load(std::memory_order_acquire) ? "ACTIVE" : "FAIL-SAFE STANDBY",
        (mPlInputCallbackRegistered || mInputQueueHookInstalled) ? "READY" : "INACTIVE"
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

    // Opportunistically calibrate physical screen dimensions without syscalls
    float currentSw = mState.screenWidth.load(std::memory_order_relaxed);
    float currentSh = mState.screenHeight.load(std::memory_order_relaxed);
    if (x > currentSw && x < 16384.0f)
        mState.screenWidth.store(x, std::memory_order_relaxed);
    if (y > currentSh && y < 16384.0f)
        mState.screenHeight.store(y, std::memory_order_relaxed);

    // Check if the inventory paper doll was rendered within the last 160 ms
    const std::int64_t nowMs = monotonicMillis();
    const std::int64_t lastRenderMs = mState.lastInventoryRenderMs.load(std::memory_order_acquire);
    const bool inventoryOpen = (lastRenderMs > 0) && ((nowMs - lastRenderMs) <= kInventoryRenderTimeoutMs);

    if (!inventoryOpen) {
        if (mState.inventoryPreviewVisible.exchange(false, std::memory_order_acq_rel)) {
            resetTouchAndOrientation(mConfig.resetOnInventoryClose);
        }
        return false;
    }

    const int maskedAction = action & AMOTION_EVENT_ACTION_MASK;
    const int trackedPointer = mState.activePointerId.load(std::memory_order_relaxed);

    switch (maskedAction) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        // Only lock pointerId if no pointer is active AND touch starts inside the character preview bounds
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

            if (std::fabs(deltaX) > 0.20f || std::fabs(deltaY) > 0.20f) {
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

void PaperDollTouchMod::onHudPlayerRenderDetour(
    void *self,
    void *renderContext,
    void *clientInstance,
    void *uiControl,
    int pass,
    McRectangleArea *rectArea
) noexcept {
    auto originalFn = reinterpret_cast<HudPlayerRendererRenderFn>(mHudPlayerRenderOriginal);
    if (originalFn == nullptr) {
        return;
    }

    if (!mState.modActive.load(std::memory_order_acquire) ||
        clientInstance == nullptr ||
        rectArea == nullptr ||
        mGetLocalPlayerFn == nullptr) {
        originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);
        return;
    }

    // Distinguish Inventory Paper Doll panel from the small top-left HUD Paper Doll
    const float rectWidth = rectArea->x1 - rectArea->x0;
    const float rectHeight = rectArea->y1 - rectArea->y0;
    const bool isInventoryPanel = (rectWidth >= kMinInventoryPreviewWidthUi) &&
                                  (rectHeight >= kMinInventoryPreviewHeightUi) &&
                                  (rectArea->x0 > 12.0f);

    if (!isInventoryPanel) {
        originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);
        return;
    }

    mState.lastInventoryRenderMs.store(monotonicMillis(), std::memory_order_release);
    mState.inventoryPreviewVisible.store(true, std::memory_order_release);
    mState.renderedPreviewFrames.fetch_add(1, std::memory_order_relaxed);

    // Exponential smoothing on Render Thread (zero extra threads, zero allocations)
    const float targetYaw = mState.targetYawOffset.load(std::memory_order_relaxed);
    const float targetPitch = mState.targetPitchOffset.load(std::memory_order_relaxed);
    float curYaw = mState.currentYawOffset.load(std::memory_order_relaxed);
    float curPitch = mState.currentPitchOffset.load(std::memory_order_relaxed);

    const float alpha = mConfig.smoothingFactor;
    curYaw += (targetYaw - curYaw) * alpha;
    curPitch += (targetPitch - curPitch) * alpha;

    mState.currentYawOffset.store(curYaw, std::memory_order_relaxed);
    mState.currentPitchOffset.store(curPitch, std::memory_order_relaxed);

    if (std::fabs(curYaw) < 0.01f && std::fabs(curPitch) < 0.01f) {
        originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);
        return;
    }

    auto getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(mGetLocalPlayerFn);
    void *localPlayer = getLocalPlayer(clientInstance);
    if (localPlayer == nullptr) {
        originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);
        return;
    }

    // Path A: Offset-free rotation override via Actor::getRotation() const (returns Vec2& inside ActorRotationComponent)
    if (mActorGetRotationFn != nullptr) {
        auto getRotation = reinterpret_cast<ActorGetRotationFn>(mActorGetRotationFn);
        const McVec2 *rotConstPtr = getRotation(localPlayer);
        if (rotConstPtr != nullptr &&
            isValidRotationFloat(rotConstPtr->x) &&
            isValidRotationFloat(rotConstPtr->y)) {
            auto *rotMutable = const_cast<McVec2 *>(rotConstPtr);
            const float savedPitch = rotMutable->x;
            const float savedYaw = rotMutable->y;

            rotMutable->x = std::clamp(savedPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
            rotMutable->y = savedYaw + curYaw;

            float savedHeadYaw = 0.0f;
            bool hasHeadRotFn = (mActorGetYHeadRotFn != nullptr && mActorSetYHeadRotFn != nullptr);
            if (hasHeadRotFn) {
                auto getHead = reinterpret_cast<ActorGetYHeadRotFn>(mActorGetYHeadRotFn);
                auto setHead = reinterpret_cast<ActorSetYHeadRotFn>(mActorSetYHeadRotFn);
                savedHeadYaw = getHead(localPlayer);
                if (isValidRotationFloat(savedHeadYaw)) {
                    setHead(localPlayer, savedHeadYaw + curYaw);
                } else {
                    hasHeadRotFn = false;
                }
            }

            originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);

            rotMutable->x = savedPitch;
            rotMutable->y = savedYaw;
            if (hasHeadRotFn) {
                auto setHead = reinterpret_cast<ActorSetYHeadRotFn>(mActorSetYHeadRotFn);
                setHead(localPlayer, savedHeadYaw);
            }
            return;
        }
    }

    // Path B: Verified byte offsets on LocalPlayer* (with strict alignment & finite-float guards)
    if (mConfig.verifiedSignaturesReady &&
        mConfig.actorRotYawOffset > 0 &&
        mConfig.actorRotPitchOffset > 0 &&
        (mConfig.actorRotYawOffset % 4 == 0) &&
        (mConfig.actorRotPitchOffset % 4 == 0)) {
        auto *basePtr = static_cast<std::uint8_t *>(localPlayer);
        auto *actorYawPtr = reinterpret_cast<float *>(basePtr + mConfig.actorRotYawOffset);
        auto *actorPitchPtr = reinterpret_cast<float *>(basePtr + mConfig.actorRotPitchOffset);

        if (!isValidRotationFloat(*actorYawPtr) || !isValidRotationFloat(*actorPitchPtr)) {
            originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);
            return;
        }

        const float savedActorYaw = *actorYawPtr;
        const float savedActorPitch = *actorPitchPtr;

        float *bodyYawPtr = nullptr;
        float *headYawPtr = nullptr;
        float savedBodyYaw = 0.0f;
        float savedHeadYaw = 0.0f;

        if (mConfig.mobBodyYawOffset > 0 && (mConfig.mobBodyYawOffset % 4 == 0)) {
            auto *candidate = reinterpret_cast<float *>(basePtr + mConfig.mobBodyYawOffset);
            if (isValidRotationFloat(*candidate)) {
                bodyYawPtr = candidate;
                savedBodyYaw = *bodyYawPtr;
            }
        }
        if (mConfig.mobHeadYawOffset > 0 && (mConfig.mobHeadYawOffset % 4 == 0)) {
            auto *candidate = reinterpret_cast<float *>(basePtr + mConfig.mobHeadYawOffset);
            if (isValidRotationFloat(*candidate)) {
                headYawPtr = candidate;
                savedHeadYaw = *headYawPtr;
            }
        }

        *actorYawPtr = savedActorYaw + curYaw;
        *actorPitchPtr = std::clamp(savedActorPitch + curPitch, -mConfig.maxPitchDegrees, mConfig.maxPitchDegrees);
        if (bodyYawPtr)
            *bodyYawPtr = savedBodyYaw + curYaw;
        if (headYawPtr)
            *headYawPtr = savedHeadYaw + curYaw;

        originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);

        // Immediately restore world orientation so player camera in world is untouched
        *actorYawPtr = savedActorYaw;
        *actorPitchPtr = savedActorPitch;
        if (bodyYawPtr)
            *bodyYawPtr = savedBodyYaw;
        if (headYawPtr)
            *headYawPtr = savedHeadYaw;
        return;
    }

    originalFn(self, renderContext, clientInstance, uiControl, pass, rectArea);
}

} // namespace paperdoll
