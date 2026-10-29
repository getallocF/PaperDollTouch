#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace paperdoll {

struct ModConfig {
    int version = 1;
    bool enabled = true;
    float sensitivityYaw = 0.45f;
    float sensitivityPitch = 0.30f;
    bool invertVertical = false;
    float maxPitchDegrees = 45.0f;
    bool resetOnInventoryClose = true;
    float smoothingFactor = 0.35f;

    // Normalized touch hit-box bounds [0.0, 1.0] for inventory character preview panel
    float boundsMinX = 0.24f;
    float boundsMinY = 0.14f;
    float boundsMaxX = 0.44f;
    float boundsMaxY = 0.56f;

    // Target Minecraft Bedrock version and runtime symbol/signature resolution settings
    std::string expectedMinecraftVersion = "26.60";
    bool allowSymbolAutoProbe = true;
    bool allowNativeInputQueueFallback = true;
    bool verifiedSignaturesReady = false;

    // Optional byte signature or mangled symbol overrides for libminecraftpe.so (26.60 arm64-v8a)
    std::string hudPlayerRendererRenderSig = "";
    std::string getLocalPlayerFromClientSig = "";
    std::string actorGetRotationSig = "";

    // Optional verified LocalPlayer/Actor float offsets (in bytes, 4-byte aligned)
    // Only used when Actor::getRotation() symbol is unavailable and verifiedSignaturesReady=true
    int actorRotYawOffset = 0;
    int actorRotPitchOffset = 0;
    int mobBodyYawOffset = 0;
    int mobHeadYawOffset = 0;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace paperdoll

template <> struct pl::config::Schema<paperdoll::ModConfig> {
    static constexpr std::string_view title = "PaperDollTouch Config";
    static constexpr std::string_view description =
        "Configuration for PaperDollTouch inventory character touch rotation on Minecraft Bedrock 26.60 ARM64.";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Config Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enabled", .description = "Enable touch-drag rotation on the inventory paper doll preview."};
        if (name == "sensitivityYaw")
            return {.title = "Horizontal Yaw Sensitivity", .description = "Degrees of horizontal rotation per pixel dragged."};
        if (name == "sensitivityPitch")
            return {.title = "Vertical Pitch Sensitivity", .description = "Degrees of vertical head pitch per pixel dragged."};
        if (name == "invertVertical")
            return {.title = "Invert Vertical Drag", .description = "Invert vertical drag direction for pitch control."};
        if (name == "maxPitchDegrees")
            return {.title = "Max Pitch Degrees", .description = "Maximum vertical pitch clamp angle in degrees (0 to 85)."};
        if (name == "resetOnInventoryClose")
            return {.title = "Reset Orientation on Close", .description = "Reset preview yaw/pitch offsets when closing inventory."};
        if (name == "smoothingFactor")
            return {.title = "Smoothing Factor", .description = "Exponential interpolation factor per frame (0.05 = smooth, 1.0 = instant)."};
        if (name == "boundsMinX")
            return {.title = "Preview Bounds Min X", .description = "Normalized left boundary [0.0-1.0] of the inventory paper doll area."};
        if (name == "boundsMinY")
            return {.title = "Preview Bounds Min Y", .description = "Normalized top boundary [0.0-1.0] of the inventory paper doll area."};
        if (name == "boundsMaxX")
            return {.title = "Preview Bounds Max X", .description = "Normalized right boundary [0.0-1.0] of the inventory paper doll area."};
        if (name == "boundsMaxY")
            return {.title = "Preview Bounds Max Y", .description = "Normalized bottom boundary [0.0-1.0] of the inventory paper doll area."};
        if (name == "expectedMinecraftVersion")
            return {.title = "Expected Minecraft Version", .description = "Target Minecraft Bedrock version string (26.60)."};
        if (name == "allowSymbolAutoProbe")
            return {.title = "Allow ELF Symbol Auto-Probe", .description = "Automatically inspect libminecraftpe.so dynamic symbols at enable-time for HudPlayerRenderer::render and Actor::getRotation."};
        if (name == "allowNativeInputQueueFallback")
            return {.title = "Allow AInputQueue Fallback", .description = "Hook Android AInputQueue_getEvent non-destructively if Preloader Input API callback is unavailable."};
        if (name == "verifiedSignaturesReady")
            return {.title = "Verified Signatures Ready", .description = "Enable byte-signature and struct-offset hooking when custom 26.60 signatures/offsets are populated."};
        if (name == "hudPlayerRendererRenderSig")
            return {.title = "HudPlayerRenderer::render Signature", .description = "Mangled symbol or AArch64 byte signature for HudPlayerRenderer::render in libminecraftpe.so."};
        if (name == "getLocalPlayerFromClientSig")
            return {.title = "ClientInstance::getLocalPlayer Signature", .description = "Mangled symbol or AArch64 byte signature for ClientInstance::getLocalPlayer in libminecraftpe.so."};
        if (name == "actorGetRotationSig")
            return {.title = "Actor::getRotation Signature", .description = "Mangled symbol or AArch64 byte signature for Actor::getRotation in libminecraftpe.so."};
        if (name == "actorRotYawOffset")
            return {.title = "Actor Yaw Offset", .description = "Verified byte offset of yaw float in LocalPlayer/Actor (0 if using Actor::getRotation)."};
        if (name == "actorRotPitchOffset")
            return {.title = "Actor Pitch Offset", .description = "Verified byte offset of pitch float in LocalPlayer/Actor (0 if using Actor::getRotation)."};
        if (name == "mobBodyYawOffset")
            return {.title = "Mob Body Yaw Offset", .description = "Verified byte offset of body yaw float in LocalPlayer/Mob (optional)."};
        if (name == "mobHeadYawOffset")
            return {.title = "Mob Head Yaw Offset", .description = "Verified byte offset of head yaw float in LocalPlayer/Mob (optional)."};
        return {};
    }
};
