#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace paperdoll {

struct ModConfig {
    int version = 3;
    bool enabled = true;
    float sensitivityYaw = 1.0f;
    float sensitivityPitch = 1.0f;
    bool invertVertical = false;
    float maxPitchDegrees = 45.0f;
    bool resetOnInventoryClose = true;
    float smoothingFactor = 0.45f;

    // Normalized touch bounds [0.0, 1.0] (full inventory screen active in Cursor-Follow v3)
    float boundsMinX = 0.0f;
    float boundsMinY = 0.0f;
    float boundsMaxX = 1.0f;
    float boundsMaxY = 1.0f;

    // Target Minecraft Bedrock version and runtime RTTI vtable / symbol resolution settings
    std::string expectedMinecraftVersion = "26.60";
    bool allowSymbolAutoProbe = true;
    bool allowNativeInputQueueFallback = true;
    bool verifiedSignaturesReady = false;

    // Optional byte signature or mangled symbol overrides for libminecraftpe.so (26.60 arm64-v8a)
    std::string hudPlayerRendererRenderSig = "";
    std::string getLocalPlayerFromClientSig = "";
    std::string actorGetRotationSig = "";

    // Optional verified LocalPlayer/Actor float offsets (in bytes, 4-byte aligned)
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
        "Configuration for PaperDollTouch inventory cursor-follow character look on Minecraft Bedrock 26.60 ARM64.";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Config Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Enabled", .description = "Enable cursor-follow touch tracking on the inventory paper doll preview."};
        if (name == "sensitivityYaw")
            return {.title = "Horizontal Look Sensitivity", .description = "Multiplier for horizontal head & body cursor-follow angle."};
        if (name == "sensitivityPitch")
            return {.title = "Vertical Pitch Sensitivity", .description = "Multiplier for vertical head pitch cursor-follow angle."};
        if (name == "invertVertical")
            return {.title = "Invert Vertical Look", .description = "Invert vertical look direction for pitch control."};
        if (name == "maxPitchDegrees")
            return {.title = "Max Pitch Degrees", .description = "Maximum vertical pitch clamp angle in degrees (0 to 85)."};
        if (name == "resetOnInventoryClose")
            return {.title = "Reset Orientation on Close", .description = "Reset preview yaw/pitch offsets when closing inventory."};
        if (name == "smoothingFactor")
            return {.title = "Smoothing Factor", .description = "Exponential interpolation factor per frame (0.05 = smooth, 1.0 = instant)."};
        if (name == "boundsMinX")
            return {.title = "Preview Bounds Min X", .description = "Normalized left boundary [0.0-1.0] of the active touch area."};
        if (name == "boundsMinY")
            return {.title = "Preview Bounds Min Y", .description = "Normalized top boundary [0.0-1.0] of the active touch area."};
        if (name == "boundsMaxX")
            return {.title = "Preview Bounds Max X", .description = "Normalized right boundary [0.0-1.0] of the active touch area."};
        if (name == "boundsMaxY")
            return {.title = "Preview Bounds Max Y", .description = "Normalized bottom boundary [0.0-1.0] of the active touch area."};
        if (name == "expectedMinecraftVersion")
            return {.title = "Expected Minecraft Version", .description = "Target Minecraft Bedrock version string (26.60)."};
        if (name == "allowSymbolAutoProbe")
            return {.title = "Allow RTTI Vtable & Symbol Auto-Probe", .description = "Automatically resolve LivePlayerRenderer, PaperDollRenderer, DataDrivenRenderer, and Mob vtables in libminecraftpe.so."};
        if (name == "allowNativeInputQueueFallback")
            return {.title = "Allow AInputQueue Touch Hook", .description = "Hook Android AInputQueue_getEvent non-destructively alongside Preloader Input API."};
        if (name == "verifiedSignaturesReady")
            return {.title = "Verified Signatures Ready", .description = "Enable custom byte-signature and struct-offset overrides if populated."};
        if (name == "hudPlayerRendererRenderSig")
            return {.title = "LivePlayerRenderer::render Signature Override", .description = "Optional custom signature override for LivePlayerRenderer::render in libminecraftpe.so."};
        if (name == "getLocalPlayerFromClientSig")
            return {.title = "ClientInstance::getLocalPlayer Signature Override", .description = "Optional custom signature override for ClientInstance::getLocalPlayer in libminecraftpe.so."};
        if (name == "actorGetRotationSig")
            return {.title = "Actor::getRotation Signature Override", .description = "Optional custom signature override for Actor::getRotation in libminecraftpe.so."};
        if (name == "actorRotYawOffset")
            return {.title = "Actor Yaw Offset", .description = "Optional verified byte offset of yaw float in LocalPlayer/Actor."};
        if (name == "actorRotPitchOffset")
            return {.title = "Actor Pitch Offset", .description = "Optional verified byte offset of pitch float in LocalPlayer/Actor."};
        if (name == "mobBodyYawOffset")
            return {.title = "Mob Body Yaw Offset", .description = "Optional verified byte offset of body yaw float in LocalPlayer/Mob."};
        if (name == "mobHeadYawOffset")
            return {.title = "Mob Head Yaw Offset", .description = "Optional verified byte offset of head yaw float in LocalPlayer/Mob."};
        return {};
    }
};
