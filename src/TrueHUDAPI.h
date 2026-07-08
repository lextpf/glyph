#pragma once

/**
 * TrueHUD API -- vendored interface header.
 *
 * Condensed from Ershin's TrueHUD (https://github.com/ersh1/TrueHUD,
 * src/TrueHUDAPI.h, MIT license).  glyph only *calls*
 * `RequestPluginAPI` and `IVTrueHUD3::HasInfoBar`; every other virtual is
 * declared solely to keep the vtable layout identical to the original
 * header, so their parameter types matter only for slot count, never for
 * calls.  Do not reorder or remove entries.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <Windows.h>

namespace TRUEHUD_API
{
constexpr const auto TrueHUDPluginName = "TrueHUD";

/// Available interface versions.
enum class InterfaceVersion : uint8_t
{
    V1,
    V2,
    V3,
    V4
};

/// Error types returned by the TrueHUD API.
enum class APIResult : uint8_t
{
    OK,
    NotOwner,
    MustKeep,
    AlreadyGiven,
    AlreadyTaken,
    WidgetFailedToLoad,
    BadThread,
};

/// Widget removal behavior.
enum class WidgetRemovalMode : uint8_t
{
    Immediate,
    Normal,
    Delayed,
};

/// Bar color types (V2 color overrides -- unused by glyph).
enum class BarColorType : uint8_t
{
    FlashColor,
    BarColor,
    PhantomColor,
    BackgroundColor,
    PenaltyColor,
};

class WidgetBase;

using SpecialResourceCallback = std::function<float(RE::Actor* a_actor)>;
using APIResultCallback = std::function<void(APIResult)>;

/// TrueHUD's modder interface, version 1.
class IVTrueHUD1
{
public:
    [[nodiscard]] virtual unsigned long GetTrueHUDThreadId() const noexcept = 0;
    [[nodiscard]] virtual APIResult RequestTargetControl(
        SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
    [[nodiscard]] virtual APIResult RequestSpecialResourceBarsControl(
        SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
    virtual APIResult SetTarget(SKSE::PluginHandle a_myPluginHandle,
                                RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual APIResult SetSoftTarget(SKSE::PluginHandle a_myPluginHandle,
                                    RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual void AddActorInfoBar(RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual void RemoveActorInfoBar(RE::ActorHandle a_actorHandle,
                                    WidgetRemovalMode a_removalMode) noexcept = 0;
    virtual void AddBoss(RE::ActorHandle a_actorHandle) noexcept = 0;
    virtual void RemoveBoss(RE::ActorHandle a_actorHandle,
                            WidgetRemovalMode a_removalMode) noexcept = 0;
    virtual void FlashActorValue(RE::ActorHandle a_actorHandle,
                                 RE::ActorValue a_actorValue,
                                 bool a_bLong) noexcept = 0;
    virtual APIResult FlashActorSpecialBar(SKSE::PluginHandle a_myPluginHandle,
                                           RE::ActorHandle a_actorHandle,
                                           bool a_bLong) noexcept = 0;
    virtual APIResult RegisterSpecialResourceFunctions(
        SKSE::PluginHandle a_myPluginHandle,
        SpecialResourceCallback&& a_getCurrentSpecialResource,
        SpecialResourceCallback&& a_getMaxSpecialResource,
        bool a_bSpecialMode,
        bool a_bDisplaySpecialForPlayer = true) noexcept = 0;
    virtual void LoadCustomWidgets(SKSE::PluginHandle a_myPluginHandle,
                                   std::string_view a_filePath,
                                   APIResultCallback&& a_successCallback) noexcept = 0;
    virtual void RegisterNewWidgetType(SKSE::PluginHandle a_myPluginHandle,
                                       uint32_t a_widgetType) noexcept = 0;
    virtual void AddWidget(SKSE::PluginHandle a_myPluginHandle,
                           uint32_t a_widgetType,
                           uint32_t a_widgetID,
                           std::string_view a_symbolIdentifier,
                           std::shared_ptr<WidgetBase> a_widget) noexcept = 0;
    virtual void RemoveWidget(SKSE::PluginHandle a_myPluginHandle,
                              uint32_t a_widgetType,
                              uint32_t a_widgetID,
                              WidgetRemovalMode a_removalMode) noexcept = 0;
    virtual SKSE::PluginHandle GetTargetControlOwner() const noexcept = 0;
    virtual SKSE::PluginHandle GetPlayerWidgetBarColorsControlOwner() const noexcept = 0;
    virtual SKSE::PluginHandle GetSpecialResourceBarControlOwner() const noexcept = 0;
    virtual APIResult ReleaseTargetControl(SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
    virtual APIResult ReleaseSpecialResourceBarControl(
        SKSE::PluginHandle a_myPluginHandle) noexcept = 0;
};

/// Version 2: per-bar color overrides (unused by glyph; vtable slots only).
class IVTrueHUD2 : public IVTrueHUD1
{
public:
    virtual APIResult OverrideBarColor(RE::ActorHandle a_actorHandle,
                                       RE::ActorValue a_actorValue,
                                       BarColorType a_colorType,
                                       uint32_t a_color) noexcept = 0;
    virtual APIResult OverrideSpecialBarColor(RE::ActorHandle a_actorHandle,
                                              BarColorType a_colorType,
                                              uint32_t a_color) noexcept = 0;
    virtual APIResult RevertBarColor(RE::ActorHandle a_actorHandle,
                                     RE::ActorValue a_actorValue,
                                     BarColorType a_colorType) noexcept = 0;
    virtual APIResult RevertSpecialBarColor(RE::ActorHandle a_actorHandle,
                                            BarColorType a_colorType) noexcept = 0;
};

/// Version 3: debug drawing (unused) + HasInfoBar (the query glyph needs).
class IVTrueHUD3 : public IVTrueHUD2
{
public:
    virtual void DrawLine(const RE::NiPoint3& a_start,
                          const RE::NiPoint3& a_end,
                          float a_duration = 0.f,
                          uint32_t a_color = 0xFF0000FF,
                          float a_thickness = 1.f) noexcept = 0;
    virtual void DrawPoint(const RE::NiPoint3& a_position,
                           float a_size,
                           float a_duration = 0.f,
                           uint32_t a_color = 0xFF0000FF) noexcept = 0;
    virtual void DrawArrow(const RE::NiPoint3& a_start,
                           const RE::NiPoint3& a_end,
                           float a_size = 10.f,
                           float a_duration = 0.f,
                           uint32_t a_color = 0xFF0000FF,
                           float a_thickness = 1.f) noexcept = 0;
    virtual void DrawBox(const RE::NiPoint3& a_center,
                         const RE::NiPoint3& a_extent,
                         const RE::NiQuaternion& a_rotation,
                         float a_duration = 0.f,
                         uint32_t a_color = 0xFF0000FF,
                         float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCircle(const RE::NiPoint3& a_center,
                            const RE::NiPoint3& a_x,
                            const RE::NiPoint3& a_y,
                            float a_radius,
                            uint32_t a_segments,
                            float a_duration = 0.f,
                            uint32_t a_color = 0xFF0000FF,
                            float a_thickness = 1.f) noexcept = 0;
    virtual void DrawHalfCircle(const RE::NiPoint3& a_center,
                                const RE::NiPoint3& a_x,
                                const RE::NiPoint3& a_y,
                                float a_radius,
                                uint32_t a_segments,
                                float a_duration = 0.f,
                                uint32_t a_color = 0xFF0000FF,
                                float a_thickness = 1.f) noexcept = 0;
    virtual void DrawSphere(const RE::NiPoint3& a_origin,
                            float a_radius,
                            uint32_t a_segments = 16,
                            float a_duration = 0.f,
                            uint32_t a_color = 0xFF0000FF,
                            float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCylinder(const RE::NiPoint3& a_start,
                              const RE::NiPoint3& a_end,
                              float a_radius,
                              uint32_t a_segments,
                              float a_duration = 0.f,
                              uint32_t a_color = 0xFF0000FF,
                              float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCone(const RE::NiPoint3& a_origin,
                          const RE::NiPoint3& a_direction,
                          float a_length,
                          float a_angleWidth,
                          float a_angleHeight,
                          uint32_t a_segments,
                          float a_duration = 0.f,
                          uint32_t a_color = 0xFF0000FF,
                          float a_thickness = 1.f) noexcept = 0;
    virtual void DrawCapsule(const RE::NiPoint3& a_origin,
                             float a_halfHeight,
                             float a_radius,
                             const RE::NiQuaternion& a_rotation,
                             float a_duration = 0.f,
                             uint32_t a_color = 0xFF0000FF,
                             float a_thickness = 1.f) noexcept = 0;

    /// True when TrueHUD currently displays an info bar for the actor.
    /// `a_bFloatingOnly` restricts the answer to bars floating over the
    /// actor's head (the ones a nameplate would collide with).
    [[nodiscard]] virtual bool HasInfoBar(RE::ActorHandle a_actorHandle,
                                          bool a_bFloatingOnly = false) const noexcept = 0;
};

using _RequestPluginAPI = void* (*)(const InterfaceVersion interfaceVersion);

/// Request the TrueHUD API interface.
/// Returns nullptr when TrueHUD is absent or too old for the version asked.
[[nodiscard]] inline void* RequestPluginAPI(
    const InterfaceVersion a_interfaceVersion = InterfaceVersion::V3)
{
    const auto pluginHandle = GetModuleHandleA("TrueHUD.dll");
    if (!pluginHandle)
    {
        return nullptr;
    }
    const auto requestAPIFunction = reinterpret_cast<_RequestPluginAPI>(
        GetProcAddress(pluginHandle, "RequestPluginAPI"));
    if (requestAPIFunction)
    {
        return requestAPIFunction(a_interfaceVersion);
    }
    return nullptr;
}
}  // namespace TRUEHUD_API
