#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace paperdoll {

struct ModConfig {
    int version = 4;
    bool enabled = true;

    // Mobile Cursor — Windows-Style GUI System settings
    bool cursorEnabled = true;
    std::string cursorSizePreset = "medium"; // "small" (0.75x), "medium" (1.00x), "large" (1.50x)
    float cursorScale = 1.00f;               // Continuous slider [0.75, 2.00], default 1.00
    bool greenSlotHoverEnabled = true;       // Windows-style #38E54D green slot hover border
    bool paperDollFollowCursor = true;       // PaperDoll head & body follow cursor in GUI
    bool hideWhenMouseConnected = true;      // Suppress custom cursor when physical mouse is active
    std::string touchInputMode = "direct";   // "direct" or "trackpad"
    float trackpadSensitivity = 1.25f;

    // PaperDoll head + body cursor-follow tuning
    float sensitivityYaw = 1.0f;
    float sensitivityPitch = 1.0f;
    bool invertVertical = false;
    float maxPitchDegrees = 45.0f;
    bool resetOnInventoryClose = true;
    float smoothingFactor = 0.45f;

    // Normalized touch bounds [0.0, 1.0]
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
    static constexpr std::string_view title = "Minecraft Mobile Cursor & PaperDollTouch Config";
    static constexpr std::string_view description =
        "Configuration for Minecraft Mobile Cursor (Windows-Style GUI Pointer, Green Slot Hover, Mobile Cursor Settings, and PaperDoll Head Follow) on Minecraft Bedrock 26.60 ARM64.";

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version")
            return {.title = "Config Version", .readOnly = true};
        if (name == "enabled")
            return {.title = "Mod Master Enable", .description = "Enable the Mobile Cursor & PaperDollTouch mod."};
        if (name == "cursorEnabled")
            return {.title = "Enable Mobile Cursor", .description = "Show Windows-style white arrow cursor when any GUI screen is open."};
        if (name == "cursorSizePreset")
            return {.title = "Cursor Size Preset", .description = "Preset cursor size: small (0.75x), medium (1.00x), or large (1.50x)."};
        if (name == "cursorScale")
            return {.title = "Cursor Scale Slider", .description = "Continuous cursor scale factor from 0.75 to 2.00 (default 1.00)."};
        if (name == "greenSlotHoverEnabled")
            return {.title = "Enable Green Slot Hover", .description = "Highlight hovered inventory and container slots with Windows-style green border."};
        if (name == "paperDollFollowCursor")
            return {.title = "PaperDoll Head Follows Cursor", .description = "Rotate PaperDoll head (yaw/pitch) and body toward the cursor when visible."};
        if (name == "hideWhenMouseConnected")
            return {.title = "Hide When Physical Mouse Used", .description = "Automatically hide custom mobile cursor if a hardware mouse is sending events."};
        if (name == "touchInputMode")
            return {.title = "Touch Input Mode", .description = "Touch cursor movement mode: direct or trackpad."};
        if (name == "trackpadSensitivity")
            return {.title = "Trackpad Sensitivity", .description = "Pointer speed multiplier when touchInputMode is trackpad."};
        if (name == "sensitivityYaw")
            return {.title = "Horizontal Look Sensitivity", .description = "Multiplier for horizontal head & body cursor-follow angle."};
        if (name == "sensitivityPitch")
            return {.title = "Vertical Pitch Sensitivity", .description = "Multiplier for vertical head pitch cursor-follow angle."};
        if (name == "invertVertical")
            return {.title = "Invert Vertical Look", .description = "Invert vertical look direction for pitch control."};
        if (name == "maxPitchDegrees")
            return {.title = "Max Pitch Degrees", .description = "Maximum vertical pitch clamp angle in degrees (0 to 85)."};
        if (name == "resetOnInventoryClose")
            return {.title = "Reset Orientation on Close", .description = "Reset preview yaw/pitch offsets and hover state when closing GUI."};
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
            return {.title = "Allow RTTI Vtable & Symbol Auto-Probe", .description = "Automatically resolve UI renderers, slot renderers, and Mob/Actor vtables in libminecraftpe.so."};
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
