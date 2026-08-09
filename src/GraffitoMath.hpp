#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

/**
 * @namespace Graffito::Math
 * @brief Pure geometry helpers for perspective-projected world text.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Runtime-independent, stateless geometry; safe from either thread. Units are world units,
 * radians, seconds and source pixels with downward +Y unless a parameter states otherwise.
 */
namespace Graffito::Math
{
inline constexpr double PI = 3.1415926535897932384626433832795;
inline constexpr double TWO_PI = PI * 2.0;

/**
 * @struct Vec2
 * @brief Plain two-dimensional math value.
 * @author Alex (<https://github.com/lextpf>)
 */
struct Vec2
{
    double x = 0.0;
    double y = 0.0;
};

/**
 * @struct Vec3
 * @brief Plain three-dimensional math value.
 * @author Alex (<https://github.com/lextpf>)
 */
struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/**
 * @fn Vec3 operator+(const Vec3& lhs, const Vec3& rhs)
 * @brief Add vectors component by component.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return The component sums.
 */
inline Vec3 operator+(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

/**
 * @fn Vec3 operator-(const Vec3& lhs, const Vec3& rhs)
 * @brief Subtract vectors component by component.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return The left components minus the right components.
 */
inline Vec3 operator-(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

/**
 * @fn Vec3 operator*(const Vec3& value, double scale)
 * @brief Scale each vector component by the same factor.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return The scaled vector.
 */
inline Vec3 operator*(const Vec3& value, double scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

/**
 * @fn double Dot(const Vec3& lhs, const Vec3& rhs)
 * @brief Measure the scalar product of two vectors.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return The sum of their component products.
 */
inline double Dot(const Vec3& lhs, const Vec3& rhs)
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

/**
 * @fn Vec3 Cross(const Vec3& lhs, const Vec3& rhs)
 * @brief Form the right-handed cross product.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return A vector perpendicular to both inputs; zero for parallel inputs.
 */
inline Vec3 Cross(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

/**
 * @fn double LengthSquared(const Vec3& value)
 * @brief Measure vector magnitude without a square root.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return The sum of squared components.
 */
inline double LengthSquared(const Vec3& value)
{
    return Dot(value, value);
}

/**
 * @fn bool Normalize(const Vec3& value, Vec3& out, double epsilon = 1e-12)
 * @brief Scale a vector to unit length.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param value   Vector to normalize.
 * @param out     Receives the unit vector. It is only meaningful when the
 *                function returns true, and it is set to zero when the length
 *                test fails.
 * @param epsilon Smallest accepted length. The test compares the squared length
 *                against epsilon squared.
 * @return True when the result is a finite unit vector; false for a non-finite
 *         input or for a length at or below epsilon.
 */
inline bool Normalize(const Vec3& value, Vec3& out, double epsilon = 1e-12)
{
    const double lenSq = LengthSquared(value);
    if (!std::isfinite(lenSq) || lenSq <= epsilon * epsilon)
    {
        out = {};
        return false;
    }

    const double invLen = 1.0 / std::sqrt(lenSq);
    out = value * invLen;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

/**
 * @fn bool IsFinite(const Vec3& value)
 * @brief Check every vector component for a finite value.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True when all three components are finite.
 */
inline bool IsFinite(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/**
 * @fn Vec3 BlendMotionVelocity(const Vec3& previousVelocity, const Vec3& previousSample, const
 *     Vec3& currentSample, double sampleDeltaTime, double response)
 * @brief Blend a newly measured world-space velocity into the previous estimate.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param previousVelocity  Prior estimate. A non-finite value is treated as zero.
 * @param previousSample    Earlier world position.
 * @param currentSample     Later world position.
 * @param sampleDeltaTime   Seconds between the two samples. A non-finite value, or one at or below
 * 1e-6, returns the prior estimate.
 * @param response          Blend weight for the new measurement, clamped to [0, 1].
 * @return The blended velocity, in world units per second.
 */
inline Vec3 BlendMotionVelocity(const Vec3& previousVelocity,
                                const Vec3& previousSample,
                                const Vec3& currentSample,
                                double sampleDeltaTime,
                                double response)
{
    if (!IsFinite(previousSample) || !IsFinite(currentSample) || !std::isfinite(sampleDeltaTime) ||
        sampleDeltaTime <= 1e-6)
    {
        return IsFinite(previousVelocity) ? previousVelocity : Vec3{};
    }

    const Vec3 measured = (currentSample - previousSample) * (1.0 / sampleDeltaTime);
    if (!IsFinite(measured))
    {
        return IsFinite(previousVelocity) ? previousVelocity : Vec3{};
    }

    const Vec3 finitePrevious = IsFinite(previousVelocity) ? previousVelocity : Vec3{};
    response = std::clamp(response, 0.0, 1.0);
    return finitePrevious * (1.0 - response) + measured * response;
}

/**
 * @fn Vec3 PredictMotionPosition(const Vec3& sample, const Vec3& velocity, double sampleAge, double
 *     sampleInterval, double maxHorizon = .050, double maxDisplacement = 32.0)
 * @brief Predict a sampled world position just far enough to bridge render frames.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Cap prediction at 1.5 observed intervals so stalled snapshots stay near the last pose.
 *
 * @param sample          Last known world position.
 * @param velocity        Current velocity estimate, in world units per second.
 * @param sampleAge       Seconds since @p sample was taken. It is raised to 0.
 * @param sampleInterval  Observed seconds between samples, clamped to [0.001, `maxHorizon`].
 * @param maxHorizon      Upper bound on the extrapolation time, in seconds. The default is 50 ms.
 * it must be 0.001 or more; a smaller positive bound inverts the limits of the sample interval
 * clamp below, which is undefined behavior.
 * @param maxDisplacement Upper bound on the extrapolated offset length, in world units. The default
 * is 32.
 * @return The extrapolated position. Any non-finite input, or a non-positive bound, returns
 * `sample` unchanged; a non-finite `sample` returns the zero vector.
 */
inline Vec3 PredictMotionPosition(const Vec3& sample,
                                  const Vec3& velocity,
                                  double sampleAge,
                                  double sampleInterval,
                                  double maxHorizon = .050,
                                  double maxDisplacement = 32.0)
{
    if (!IsFinite(sample) || !IsFinite(velocity) || !std::isfinite(sampleAge) ||
        !std::isfinite(sampleInterval) || !std::isfinite(maxHorizon) ||
        !std::isfinite(maxDisplacement) || maxHorizon <= 0.0 || maxDisplacement <= 0.0)
    {
        return IsFinite(sample) ? sample : Vec3{};
    }

    sampleAge = std::max(0.0, sampleAge);
    sampleInterval = std::clamp(sampleInterval, .001, maxHorizon);
    const double horizon = std::min(sampleAge, std::min(maxHorizon, sampleInterval * 1.5));
    Vec3 offset = velocity * horizon;
    const double offsetLengthSquared = LengthSquared(offset);
    const double maxLengthSquared = maxDisplacement * maxDisplacement;
    if (!std::isfinite(offsetLengthSquared))
    {
        return sample;
    }
    if (offsetLengthSquared > maxLengthSquared)
    {
        offset = offset * (maxDisplacement / std::sqrt(offsetLengthSquared));
    }
    return sample + offset;
}

/**
 * @fn double RaySphereHitDistance(const Vec3& rayOrigin, const Vec3& rayDirection, const Vec3&
 *     sphereCenter, double sphereRadius, double maxDistance =
 *     std::numeric_limits<double>::infinity())
 * @brief Return the first non-negative distance at which a ray enters a sphere.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Normalize direction internally; an origin inside the sphere returns zero.
 *
 * @param rayOrigin    Ray start point. A non-finite value returns infinity.
 * @param rayDirection Ray direction, of any length. A zero-length or non-finite direction returns
 * infinity.
 * @param sphereCenter Sphere center. A non-finite value returns infinity.
 * @param sphereRadius Sphere radius. A value at or below zero returns infinity; it does not
 * degenerate to a point test.
 * @param maxDistance  Largest accepted entry distance, in world units. A negative or NaN value
 * returns infinity. The default accepts any distance.
 * @return Entry distance along the normalized direction, or infinity for no hit.
 */
inline double RaySphereHitDistance(const Vec3& rayOrigin,
                                   const Vec3& rayDirection,
                                   const Vec3& sphereCenter,
                                   double sphereRadius,
                                   double maxDistance = std::numeric_limits<double>::infinity())
{
    constexpr double NO_HIT = std::numeric_limits<double>::infinity();
    if (!std::isfinite(rayOrigin.x) || !std::isfinite(rayOrigin.y) || !std::isfinite(rayOrigin.z) ||
        !std::isfinite(sphereCenter.x) || !std::isfinite(sphereCenter.y) ||
        !std::isfinite(sphereCenter.z) || !std::isfinite(sphereRadius) || sphereRadius <= 0.0 ||
        std::isnan(maxDistance) || maxDistance < 0.0)
    {
        return NO_HIT;
    }

    Vec3 direction{};
    if (!Normalize(rayDirection, direction))
    {
        return NO_HIT;
    }

    const Vec3 toCenter = sphereCenter - rayOrigin;
    const double centerDistance = Dot(toCenter, direction);
    const double toCenterSquared = LengthSquared(toCenter);
    if (!std::isfinite(centerDistance) || !std::isfinite(toCenterSquared))
    {
        return NO_HIT;
    }

    const double radiusSquared = sphereRadius * sphereRadius;
    const double perpendicularSquared =
        std::max(0.0, toCenterSquared - centerDistance * centerDistance);
    if (!std::isfinite(radiusSquared) || perpendicularSquared > radiusSquared)
    {
        return NO_HIT;
    }

    const double halfChord = std::sqrt(std::max(0.0, radiusSquared - perpendicularSquared));
    if (centerDistance + halfChord < 0.0)
    {
        return NO_HIT;
    }

    const double entryDistance = std::max(0.0, centerDistance - halfChord);
    return entryDistance <= maxDistance ? entryDistance : NO_HIT;
}

/**
 * @struct UprightBasis
 * @brief Orthonormal right-handed page axes for upright actor text.
 * @author Alex (<https://github.com/lextpf>)
 */
struct UprightBasis
{
    Vec3 forward{};  ///< Front normal: the side from which the text is readable.
    Vec3 right{};    ///< Page-right when viewed from the actor's front.
    Vec3 up{};       ///< World up, always (0, 0, 1).
};

/**
 * @fn UprightBasis BuildUprightBasis(double yawRadians)
 * @brief Build the stable yaw-only basis used by actor-bound Graffito.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Yaw-zero forward is +Y: (sin(yaw), cos(yaw), 0). Page-right = worldUp x forward
 * keeps lettering readable from the front. Forward == Cross(right, up).
 *
 * @param yawRadians Actor yaw, in radians. It is not scrubbed: a non-finite yaw gives a non-finite
 * basis.
 * @return The orthonormal right-handed page basis.
 */
inline UprightBasis BuildUprightBasis(double yawRadians)
{
    const Vec3 worldUp{0.0, 0.0, 1.0};
    const Vec3 forward{std::sin(yawRadians), std::cos(yawRadians), 0.0};
    return {forward, Cross(worldUp, forward), worldUp};
}

/**
 * @fn double YawFacingPoint(const Vec3& anchor, const Vec3& target, double fallbackYaw)
 * @brief Resolve Skyrim yaw whose readable normal points from an anchor toward a target.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param anchor       Plane position.
 * @param target       Point the readable normal must face.
 * @param fallbackYaw  Returned when the horizontal separation is non-finite or at or below 1e-5
 * world units. A non-finite fallback itself yields 0.
 * @return The yaw, in radians. A resolved yaw is from -pi through pi. A returned fallback is passed
 * through as given and is not wrapped.
 */
inline double YawFacingPoint(const Vec3& anchor, const Vec3& target, double fallbackYaw)
{
    const double dx = target.x - anchor.x;
    const double dy = target.y - anchor.y;
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) <= 1e-5)
    {
        return std::isfinite(fallbackYaw) ? fallbackYaw : 0.0;
    }
    return std::atan2(dx, dy);
}

/**
 * @fn double WrapAngle(double angle)
 * @brief Wrap an angle to the turn centred on zero.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Non-finite angles return zero.
 *
 * @param angle  Angle to wrap, in radians.
 * @return       The wrapped angle, from -pi through pi.
 */
inline double WrapAngle(double angle)
{
    if (!std::isfinite(angle))
    {
        return 0.0;
    }
    return std::remainder(angle, TWO_PI);
}

/**
 * @fn double ShortestAngleDelta(double fromRadians, double toRadians)
 * @brief Signed shortest turn between two angles.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param fromRadians Start angle, in radians.
 * @param toRadians   End angle, in radians.
 * @return The turn from the start angle to the end angle, in radians, from -pi
 *         through pi. A non-finite input returns 0.
 */
inline double ShortestAngleDelta(double fromRadians, double toRadians)
{
    return WrapAngle(toRadians - fromRadians);
}

/**
 * @fn double ExponentialAlpha(double deltaTime, double settleTime, double epsilon = 0.01)
 * @brief Frame-rate-independent exponential approach factor.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Delta t is frame time, T is settle time and epsilon is the remaining error fraction.
 * Compounding the factor for T seconds leaves epsilon of the initial error.
 *
 * $$ \alpha = 1 - \epsilon^{\,\Delta t / T} $$
 *
 * @param deltaTime  Frame time, in seconds. A non-finite or non-positive value returns 0, so
 * nothing moves this frame.
 * @param settleTime Time T, in seconds. A non-finite or non-positive value returns 1, so the value
 * snaps to its target.
 * @param epsilon    Error fraction still left at the settle time, clamped to [1e-9, 1 - 1e-9]. The
 * default leaves 1 percent.
 * @return The blend factor, from 0 through 1.
 */
inline double ExponentialAlpha(double deltaTime, double settleTime, double epsilon = 0.01)
{
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0)
    {
        return 0.0;
    }
    if (!std::isfinite(settleTime) || settleTime <= 0.0)
    {
        return 1.0;
    }
    epsilon = std::clamp(epsilon, 1e-9, 1.0 - 1e-9);
    return std::clamp(1.0 - std::pow(epsilon, deltaTime / settleTime), 0.0, 1.0);
}

/**
 * @fn double SmoothAngle(double currentRadians, double targetRadians, double deltaTime, double
 *     settleTime, double epsilon = 0.01)
 * @brief Smooth toward a target angle along the shortest turn.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param currentRadians Present angle. A non-finite value adopts the target.
 * @param targetRadians  Wanted angle. A non-finite value returns the wrapped
 *                       present angle without smoothing.
 * @param deltaTime      Frame time, in seconds.
 * @param settleTime     Settle time, in seconds; see ExponentialAlpha.
 * @param epsilon        Error fraction still left at the settle time.
 * @return The new angle, in radians, from -pi through pi.
 */
inline double SmoothAngle(double currentRadians,
                          double targetRadians,
                          double deltaTime,
                          double settleTime,
                          double epsilon = 0.01)
{
    if (!std::isfinite(currentRadians))
    {
        currentRadians = targetRadians;
    }
    if (!std::isfinite(targetRadians))
    {
        return WrapAngle(currentRadians);
    }
    const double alpha = ExponentialAlpha(deltaTime, settleTime, epsilon);
    return WrapAngle(currentRadians + ShortestAngleDelta(currentRadians, targetRadians) * alpha);
}

/**
 * @fn double SmoothStep(double value)
 * @brief Cubic ease with zero slope at both ends.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clamp input to [0, 1]. Infinities saturate; NaN propagates.
 *
 * @param value  Input value.
 * @return       The eased value from zero through one, or NaN when `value` is NaN.
 */
inline double SmoothStep(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

/**
 * @struct FacingMaterial
 * @brief View-dependent ink treatment for a two-sided inscription.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Apply opacity once, either on the CPU or in the shader. Desaturation and brightness
 * affect all vertices, including fill, outline and shadow.
 */
struct FacingMaterial
{
    double opacity = 0.0;       ///< Alpha multiplier for all projected ink, from 0 through 1.
    double desaturation = 0.0;  ///< Blend of RGB toward luminance, from 0 through 1.
    double brightness = 1.0;    ///< RGB multiplier applied after the desaturation.
};

// Fixed ink treatment of the two non-front cases. The reverse side keeps a trace
// of hue, while the edge seam is fully desaturated and darker still.
inline constexpr double BACK_BLEED_DESATURATION = 0.82;
inline constexpr double BACK_BLEED_BRIGHTNESS = 0.72;
inline constexpr double EDGE_SEAM_DESATURATION = 1.0;
inline constexpr double EDGE_SEAM_BRIGHTNESS = 0.55;

/**
 * @fn FacingMaterial EvaluateFacingMaterial(double frontDot, double fadeDegrees, double
 *     backBleedAlpha, double edgeSeamAlpha)
 * @brief Resolve front ink, an edge-on seam, and mirrored backside bleed.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Weight material colors by opacity to avoid a hue jump at the seam.
 *
 * @param frontDot       Dot product of the plane normal with the unit direction from the plane
 * toward the viewer. A positive value selects the readable side, a negative value the reverse side,
 * and exactly zero forces the pure edge seam. A non-finite value returns a fully transparent
 * material.
 * @param fadeDegrees    Angular transition band on either side of edge-on, in degrees, clamped to
 * at most 90. A non-finite or non-positive value removes the band, but a frontDot of exactly zero
 * still gives the pure seam.
 * @param backBleedAlpha Opacity of the reverse-side ink, clamped to [0, 1].
 * @param edgeSeamAlpha  Opacity at the pure edge seam, clamped to [0, 1].
 * @return The blended opacity, desaturation, and brightness. When the blended opacity is at or
 * below 1e-12 the blend has no weight, so the two treatment fields keep their defaults of 0
 * desaturation and 1 brightness.
 */
inline FacingMaterial EvaluateFacingMaterial(double frontDot,
                                             double fadeDegrees,
                                             double backBleedAlpha,
                                             double edgeSeamAlpha)
{
    if (!std::isfinite(frontDot))
    {
        return {};
    }

    frontDot = std::clamp(frontDot, -1.0, 1.0);
    backBleedAlpha = std::isfinite(backBleedAlpha) ? std::clamp(backBleedAlpha, 0.0, 1.0) : 0.0;
    edgeSeamAlpha = std::isfinite(edgeSeamAlpha) ? std::clamp(edgeSeamAlpha, 0.0, 1.0) : 0.0;

    double sideProgress = 1.0;
    if (std::isfinite(fadeDegrees) && fadeDegrees > 0.0)
    {
        const double fadeRadians = std::clamp(fadeDegrees, 0.0, 90.0) * PI / 180.0;
        const double angleFromEdge = std::asin(std::abs(frontDot));
        sideProgress = fadeRadians > 1e-12 ? SmoothStep(angleFromEdge / fadeRadians) : 1.0;
    }

    if (frontDot == 0.0)
    {
        sideProgress = 0.0;
    }

    const FacingMaterial side =
        frontDot > 0.0
            ? FacingMaterial{1.0, 0.0, 1.0}
            : FacingMaterial{backBleedAlpha, BACK_BLEED_DESATURATION, BACK_BLEED_BRIGHTNESS};
    const FacingMaterial edge{edgeSeamAlpha, EDGE_SEAM_DESATURATION, EDGE_SEAM_BRIGHTNESS};
    const double sideContribution = side.opacity * sideProgress;
    const double edgeContribution = edge.opacity * (1.0 - sideProgress);

    FacingMaterial out{};
    out.opacity = sideContribution + edgeContribution;
    if (out.opacity <= 1e-12)
    {
        return out;
    }
    out.desaturation =
        (side.desaturation * sideContribution + edge.desaturation * edgeContribution) / out.opacity;
    out.brightness =
        (side.brightness * sideContribution + edge.brightness * edgeContribution) / out.opacity;
    return out;
}

/**
 * @struct PlanePose
 * @brief Position and orthonormal page basis for an arbitrary text plane.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Axes must be orthonormal and right-handed: normal == Cross(right, up). +normal is
 * the readable side. This is unchecked; CylinderPointFromSourceOffset recomputes normal.
 */
struct PlanePose
{
    Vec3 origin{};  ///< World point that source offset (0, 0) maps to.
    Vec3 normal{};  ///< Front normal: the side from which the text is readable.
    Vec3 right{};   ///< Page-right axis, toward increasing source X.
    Vec3 up{};      ///< Page-up axis, toward decreasing source Y.
};

/**
 * @fn PlanePose OffsetPlanePose(const PlanePose& pose, double normalDistance)
 * @brief Translate a plane along its own normal without changing its basis.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Positive distance moves toward the readable side. Non-finite distance becomes zero.
 *
 * @param pose            Source plane pose.
 * @param normalDistance  Signed distance along the plane normal.
 * @return                The translated pose.
 */
inline PlanePose OffsetPlanePose(const PlanePose& pose, double normalDistance)
{
    const double distance = std::isfinite(normalDistance) ? normalDistance : 0.0;
    PlanePose out = pose;
    out.origin = pose.origin + pose.normal * distance;
    return out;
}

/**
 * @struct FolioWeights
 * @brief Continuous view weights for the full front and the compact marker.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Valid directions satisfy front + spine + back == 1 and front * back == 0.
 * Non-finite directions or planar length <= 1e-6 return all-zero weights.
 */
struct FolioWeights
{
    double front = 0.0;  ///< Weight of the full front inscription.
    double spine = 0.0;  ///< Weight of the compact side facet.
    double back = 0.0;   ///< Weight of the rear facet.
    /// +1 is page-right; -1 is page-left. Zero weights retain +1.
    int sideSign = 1;
};

/**
 * @brief Depth multipliers of the configured relief spacing, far to near.
 *
 * Offset along the normal by minus this multiple of relief spacing; index 0 is farthest.
 */
inline constexpr std::array<double, 3> FOLIO_RELIEF_STEPS{3.0, 2.0, 1.0};

/**
 * @struct FolioFacetMetrics
 * @brief Source-space dimensions and head clearance for the compact marker.
 * @author Alex (<https://github.com/lextpf>)
 */
struct FolioFacetMetrics
{
    double reliefSpacing = 0.0;  ///< Gap between adjacent relief planes, in source pixels.
    double sideWidth = 48.0;     ///< Width of the side facet, in source pixels.
    double height = 56.0;        ///< Height of the marker, in source pixels.
    double rearWidth = 66.0;     ///< Width of the rear facet, in source pixels.
    double markerLift = 9.0;     ///< Clearance above the head anchor, in source pixels.

    /**
     * @fn double TotalReliefDepth() const
     * @brief Measure the full relief stack behind the front face.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return Distance to the farthest relief plane, in source pixels.
     */
    double TotalReliefDepth() const { return reliefSpacing * FOLIO_RELIEF_STEPS.front(); }

    /**
     * @fn double RankSize(double width) const
     * @brief Rank glyph size for one facet, in source pixels.
     * @author Alex (<https://github.com/lextpf>)
     *
     * Size = clamp(.82 * min(width, marker height), 42, 96) source pixels.
     *
     * @param width Facet width, in source pixels.
     * @return The rank glyph size, in source pixels.
     */
    double RankSize(double width) const
    {
        return std::clamp(std::min(width, height) * .82, 42.0, 96.0);
    }
};

/**
 * @fn FolioFacetMetrics ComputeFolioFacetMetrics(double nameFontSize, double reliefSpacingPixels)
 * @brief Derive the compact marker's metrics from the name typography.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Relief spacing stays in source pixels. Side width is at least 1.5 times relief depth,
 * subject to its clamp; typography does not enlarge the spacing.
 *
 * @param nameFontSize        Name row font size, in source pixels. A non-finite or negative value
 * is treated as 0.
 * @param reliefSpacingPixels Wanted gap between adjacent relief planes, in source pixels, clamped
 * to [0, 28]. A non-finite value is treated as 0, which disables the relief.
 * @return Metrics in source pixels. Height is clamped to [56, 120], side width to [48, 108], rear
 * width to [66, 140], and marker lift to [8, 20], so every field stays positive even for a zero
 * font size.
 */
inline FolioFacetMetrics ComputeFolioFacetMetrics(double nameFontSize, double reliefSpacingPixels)
{
    const double fontSize = std::isfinite(nameFontSize) ? std::max(0.0, nameFontSize) : 0.0;
    const double spacing =
        std::isfinite(reliefSpacingPixels) ? std::max(0.0, reliefSpacingPixels) : 0.0;

    FolioFacetMetrics out{};
    out.reliefSpacing = std::clamp(spacing, 0.0, 28.0);
    out.height = std::clamp(fontSize * .55, 56.0, 120.0);
    out.sideWidth = std::clamp(std::max(out.TotalReliefDepth() * 1.5, fontSize * .42), 48.0, 108.0);
    out.rearWidth = std::clamp(out.height * 1.18, 66.0, 140.0);
    out.markerLift = std::clamp(out.height * .16, 8.0, 20.0);
    return out;
}

// Crossfade band edges, as an azimuth in degrees from the readable front normal,
// measured in the page plane. The front inscription yields to the compact side
// facet across the first band, and the side facet yields to the rear facet
// across the second. The second band must start at or after the first band ends,
// or the FolioWeights partition no longer sums to 1.
inline constexpr double FOLIO_FRONT_TO_SPINE_START_DEGREES = 45.0;
inline constexpr double FOLIO_FRONT_TO_SPINE_END_DEGREES = 55.0;
inline constexpr double FOLIO_SPINE_TO_BACK_START_DEGREES = 110.0;
inline constexpr double FOLIO_SPINE_TO_BACK_END_DEGREES = 130.0;

/**
 * @fn FolioWeights EvaluateFolioWeights(double frontDot, double rightDot)
 * @brief Blend the full front, the compact side facet, and the rear facet.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Normalize the direction in the page plane to exclude camera elevation. Side activation
 * begins at 45 degrees, where its sign is stable without hysteresis.
 *
 * @verbatim
 * azimuth from the readable front normal, in degrees
 *
 * 0          45        55             110       130        180
 * |--front----|~~fade~~~|----spine-----|~~fade~~~|---back----|
 * @endverbatim
 *
 * @param frontDot Component along the plane normal of the direction from the plane toward the
 * viewer. It need not be normalized.
 * @param rightDot Component of that same direction along the page-right axis.
 * @return The three weights and the selected side.
 */
inline FolioWeights EvaluateFolioWeights(double frontDot, double rightDot)
{
    if (!std::isfinite(frontDot) || !std::isfinite(rightDot))
    {
        return {};
    }

    const double planarLength = std::hypot(frontDot, rightDot);
    if (!std::isfinite(planarLength) || planarLength <= 1e-6)
    {
        return {};
    }

    const double normalizedFront = frontDot / planarLength;
    const double normalizedRight = rightDot / planarLength;
    const double angleDegrees = std::abs(std::atan2(normalizedRight, normalizedFront)) * 180.0 / PI;
    const double frontToSpine =
        SmoothStep((angleDegrees - FOLIO_FRONT_TO_SPINE_START_DEGREES) /
                   (FOLIO_FRONT_TO_SPINE_END_DEGREES - FOLIO_FRONT_TO_SPINE_START_DEGREES));
    const double spineToBack =
        SmoothStep((angleDegrees - FOLIO_SPINE_TO_BACK_START_DEGREES) /
                   (FOLIO_SPINE_TO_BACK_END_DEGREES - FOLIO_SPINE_TO_BACK_START_DEGREES));

    return {1.0 - frontToSpine,
            frontToSpine * (1.0 - spineToBack),
            spineToBack,
            normalizedRight < 0.0 ? -1 : 1};
}

/**
 * @fn Vec3 WorldPointFromSourceOffset(const PlanePose& pose, const Vec2& sourceOffset, double
 *     worldUnitsPerPixel)
 * @brief Map a source-space offset onto an arbitrary plane pose.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Source +Y maps toward -pose.up. Non-finite offsets or scale become zero.
 *
 * @param pose                Plane pose.
 * @param sourceOffset        Source-space offset, in pixels.
 * @param worldUnitsPerPixel  Plane scale in world units per source pixel.
 * @return The mapped world point.
 */
inline Vec3 WorldPointFromSourceOffset(const PlanePose& pose,
                                       const Vec2& sourceOffset,
                                       double worldUnitsPerPixel)
{
    const double x = std::isfinite(sourceOffset.x) ? sourceOffset.x : 0.0;
    const double y = std::isfinite(sourceOffset.y) ? sourceOffset.y : 0.0;
    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;
    return pose.origin + pose.right * (x * scale) - pose.up * (y * scale);
}

/**
 * @fn Vec3 CylinderPointFromSourceOffset(const PlanePose& pose, const Vec2& sourceOffset, double
 *     worldUnitsPerPixel, double apexOffsetXPixels, double radiansPerPixel, double surfaceRadius)
 * @brief Map a source-space offset onto a cylinder wrapped about the pose up axis.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Zero or non-finite curvature/radius takes the exact flat path. Non-finite scalar inputs
 * become zero. The apex column stays fixed for every source Y. Use each surface radius
 * with the shared curvature to keep concentric layers aligned.
 *
 * In the formula, O is origin, R/U are page axes, N = R x U, s is worldUnitsPerPixel,
 * a is apexOffsetXPixels, k is radiansPerPixel and r is surfaceRadius.
 * The axis passes through O + R(a s) - N r along U; wings recede along -N.
 *
 * R * sin(theta) controls physical width. The small-angle flat limit requires r == s / k;
 * other radii scale inscription width by r*k/s.
 *
 * $$
 * P(x,y) = O + R\,\bigl(a\,s + r\sin\theta\bigr)
 *            + N\,r\,\bigl(\cos\theta - 1\bigr) - U\,(y\,s),
 * \qquad \theta = (x - a)\,k
 * $$
 *
 * @verbatim
 * Cross-section viewed along U (theta > 0):
 *
 *                         +N
 *                          ^
 *                          A    P
 *                          |   /
 *                        r |  / r
 *                          | /
 *                          +----------------> +R
 *                          C
 *
 * A: apex, x = a, theta = 0
 * C: cylinder axis, O + R(a s) - N r
 * P: wrapped point, theta = (x - a) k
 *
 * Both A and P are r units from C. Positive theta turns toward +R.
 * @endverbatim
 *
 * @p apexOffsetXPixels locates the arc apex relative to the pose's source anchor, and @p
 * radiansPerPixel is the constant curvature; a zero curvature, or a zero radius, reproduces
 * WorldPointFromSourceOffset exactly. Every scalar argument is treated as 0 when it is not finite,
 * so a non-finite curvature or radius takes that same flat path. The apex column is the fixed point
 * of the wrap: at the apex the cylinder and the flat plane give the same world point, for every
 * source Y.
 * @p surfaceRadius is this surface's own radius, so concentric layers can share a single axis. the
 * angle always comes from the caller's curvature and is never re-derived from arc length at this
 * radius; preserving arc length instead would slide a layer sideways against the front it belongs
 * to.
 * @param pose                Plane pose.
 * @param sourceOffset Source-space offset, in pixels.
 * @param worldUnitsPerPixel  Plane scale in world units per source pixel.
 * @param apexOffsetXPixels   Horizontal arc-apex offset from the source anchor.
 * @param radiansPerPixel     Cylindrical curvature.
 * @param surfaceRadius       Radius of this concentric surface, in world units.
 * @return                    The mapped world point.
 */
inline Vec3 CylinderPointFromSourceOffset(const PlanePose& pose,
                                          const Vec2& sourceOffset,
                                          double worldUnitsPerPixel,
                                          double apexOffsetXPixels,
                                          double radiansPerPixel,
                                          double surfaceRadius)
{
    const double kappa = std::isfinite(radiansPerPixel) ? radiansPerPixel : 0.0;
    const double radius = std::isfinite(surfaceRadius) ? surfaceRadius : 0.0;
    if (kappa == 0.0 || radius == 0.0)
    {
        return WorldPointFromSourceOffset(pose, sourceOffset, worldUnitsPerPixel);
    }

    const double x = std::isfinite(sourceOffset.x) ? sourceOffset.x : 0.0;
    const double y = std::isfinite(sourceOffset.y) ? sourceOffset.y : 0.0;
    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;
    const double apex = std::isfinite(apexOffsetXPixels) ? apexOffsetXPixels : 0.0;
    const double theta = (x - apex) * kappa;
    const Vec3 normal = Cross(pose.right, pose.up);
    return pose.origin + pose.right * (apex * scale + radius * std::sin(theta)) +
           normal * (radius * (std::cos(theta) - 1.0)) - pose.up * (y * scale);
}

/**
 * @fn PlanePose BuildFolioBackPose(const PlanePose& frontPose, double sourcePivotXPixels, double
 *     worldUnitsPerPixel)
 * @brief Build a rear-facing pose that preserves the front plate's footprint.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Reflect about sourcePivotXPixels: rear (x,y) maps to front (2*pivot-x,y).
 *
 * @p sourcePivotXPixels is the reflection axis relative to the front pose's source anchor. for
 * every source point `(x,y)`, the returned pose maps it to the same world point as the front pose
 * maps `(2*pivot-x,y)`.
 * @param frontPose          Front plate pose. Its basis invariant is preserved.
 * @param sourcePivotXPixels Reflection axis, in source pixels relative to the front source anchor.
 * A non-finite value becomes 0.
 * @param worldUnitsPerPixel Physical scale of one source pixel, in world units. A non-finite value
 * becomes 0.
 * @return The rear-facing pose, sharing the front pose's up axis.
 */
inline PlanePose BuildFolioBackPose(const PlanePose& frontPose,
                                    double sourcePivotXPixels,
                                    double worldUnitsPerPixel)
{
    const double pivot = std::isfinite(sourcePivotXPixels) ? sourcePivotXPixels : 0.0;
    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;

    PlanePose out{};
    out.origin = frontPose.origin + frontPose.right * (2.0 * pivot * scale);
    out.normal = frontPose.normal * -1.0;
    out.right = frontPose.right * -1.0;
    out.up = frontPose.up;
    return out;
}

/**
 * @fn PlanePose BuildFolioSpinePose(const PlanePose& frontPose, int sideSign, const Vec2&
 *     frontEdgeOffsetPixels, double worldUnitsPerPixel)
 * @brief Build the compact side-indicator plane at a selected edge of the front.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Anchor at the selected front edge. The plane is perpendicular to the front, with its
 * readable normal pointing outward. Source +X runs front-to-rear on the right side and
 * rear-to-front on the left to preserve readable orientation.
 *
 * @p frontEdgeOffsetPixels is the edge point relative to the front source anchor, normally the
 * selected horizontal bound and the plate's vertical center. The returned origin is the spine
 * layout's own source anchor.
 * @param frontPose             Front plate pose.
 * @param sideSign              Selected side. A negative value is page-left; 0 or positive is
 * page-right.
 * @param frontEdgeOffsetPixels Edge point, in source pixels relative to the front source anchor.
 * @param worldUnitsPerPixel    Physical scale of one source pixel, in world units. A non-finite
 * value becomes 0.
 * @return The side-indicator pose, pinned to the edge point.
 */
inline PlanePose BuildFolioSpinePose(const PlanePose& frontPose,
                                     int sideSign,
                                     const Vec2& frontEdgeOffsetPixels,
                                     double worldUnitsPerPixel)
{
    const double side = sideSign < 0 ? -1.0 : 1.0;

    PlanePose out{};
    out.origin = WorldPointFromSourceOffset(frontPose, frontEdgeOffsetPixels, worldUnitsPerPixel);
    out.normal = frontPose.right * side;
    out.right = frontPose.normal * -side;
    out.up = frontPose.up;
    return out;
}

/**
 * @fn PlanePose BuildFolioFacetPose(const PlanePose& frontPose, int sideSign, const Vec2&
 *     frontEdgeOffsetPixels, double worldUnitsPerPixel, double depthWorldUnits)
 * @brief Center a side-facet plane across the depth behind the front face.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Retain the BuildFolioSpinePose basis and center the anchor halfway through relief depth.
 *
 * @param frontPose             Front plate pose.
 * @param sideSign              Selected side; see BuildFolioSpinePose.
 * @param frontEdgeOffsetPixels Edge point, in source pixels relative to the front source anchor.
 * @param worldUnitsPerPixel    Physical scale of one source pixel, in world units. A non-finite
 * value becomes 0.
 * @param depthWorldUnits       Front-to-back span to straddle, in world units. A non-finite or
 * negative value becomes 0, which reproduces BuildFolioSpinePose exactly.
 * @return The centered side-facet pose.
 */
inline PlanePose BuildFolioFacetPose(const PlanePose& frontPose,
                                     int sideSign,
                                     const Vec2& frontEdgeOffsetPixels,
                                     double worldUnitsPerPixel,
                                     double depthWorldUnits)
{
    PlanePose out =
        BuildFolioSpinePose(frontPose, sideSign, frontEdgeOffsetPixels, worldUnitsPerPixel);
    const double depth = std::isfinite(depthWorldUnits) ? std::max(0.0, depthWorldUnits) : 0.0;
    out.origin = out.origin - frontPose.normal * (depth * 0.5);
    return out;
}

/**
 * @fn PlanePose BuildFallenEpitaphPose(double yawRadians, double progress, const Vec3&
 *     uprightOrigin, const Vec3& groundHinge, const Vec2& sourceHinge, double worldUnitsPerPixel)
 * @brief Hinge upright text onto the ground while pinning one source-space point.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Progress 0 returns the exact upright pose. At 1, normal points world-up, page top points
 * back toward the actor, and sourceHinge reaches groundHinge without sliding.
 *
 * Clamp progress to [0, 1], then smoothstep-ease angle and translation. Progress .25 gives
 * about 14.1 degrees. Non-finite progress becomes zero.
 *
 * @p groundHinge without sliding.
 * @p progress is clamped to [0, 1] and smoothstep-eased before it drives both the 0 to 90 degree
 * hinge angle and the hinge translation, so mid-range values are not linear in angle: 0.25 gives
 * about 14.1 degrees, not 22.5. A non-finite value is treated as 0.
 * @param yawRadians         Actor yaw, in radians; see BuildUprightBasis. It is not scrubbed.
 * @param progress           Hinge progress, from 0 upright through 1 landed.
 * @param uprightOrigin      World point that source offset (0, 0) maps to at progress 0. It is not
 * scrubbed.
 * @param groundHinge        World point the source hinge lands on at progress 1. It is not
 * scrubbed.
 * @param sourceHinge        Pinned point, in source pixels relative to the source anchor. It is not
 * scrubbed.
 * @param worldUnitsPerPixel Physical scale of one source pixel, in world units. A non-finite value
 * becomes 0.
 * @return The hinged pose. For a finite yaw its basis stays orthonormal and right-handed, so it
 * obeys the PlanePose convention at every progress.
 */
inline PlanePose BuildFallenEpitaphPose(double yawRadians,
                                        double progress,
                                        const Vec3& uprightOrigin,
                                        const Vec3& groundHinge,
                                        const Vec2& sourceHinge,
                                        double worldUnitsPerPixel)
{
    const UprightBasis upright = BuildUprightBasis(yawRadians);
    const double eased = SmoothStep(std::isfinite(progress) ? progress : 0.0);
    const double angle = eased * PI * 0.5;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);

    PlanePose out{};
    out.normal = upright.forward * cosine + upright.up * sine;
    out.right = upright.right;
    out.up = upright.up * cosine - upright.forward * sine;

    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;
    const Vec3 uprightHinge =
        uprightOrigin + out.right * (sourceHinge.x * scale) - upright.up * (sourceHinge.y * scale);
    const Vec3 movingHinge = uprightHinge * (1.0 - eased) + groundHinge * eased;
    out.origin =
        movingHinge - out.right * (sourceHinge.x * scale) + out.up * (sourceHinge.y * scale);
    return out;
}

/**
 * @fn double RangeFade(double distance, double fadeStart, double fadeEnd)
 * @brief Smooth distance fade between a near and a far bound.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Fade from 1 at the near bound to 0 at the far bound using smoothstep.
 * Non-finite distance returns zero.
 *
 * @param distance  Distance to the plate, in world units. It is raised to 0.
 * @param fadeStart Near bound, in world units. It is raised to 0. A non-finite value becomes 0.
 * @param fadeEnd   Far bound, in world units. It is raised to 0. A non-finite value adopts the near
 * bound.
 * @return The fade factor, from 0 through 1. When the far bound is not above the near bound the
 * band collapses to a hard cut at the far bound: 1 at or below it, 0 above it.
 */
inline double RangeFade(double distance, double fadeStart, double fadeEnd)
{
    if (!std::isfinite(distance))
    {
        return 0.0;
    }
    distance = std::max(0.0, distance);
    fadeStart = std::max(0.0, std::isfinite(fadeStart) ? fadeStart : 0.0);
    fadeEnd = std::max(0.0, std::isfinite(fadeEnd) ? fadeEnd : fadeStart);
    if (fadeEnd <= fadeStart)
    {
        return distance <= fadeEnd ? 1.0 : 0.0;
    }
    return 1.0 - SmoothStep((distance - fadeStart) / (fadeEnd - fadeStart));
}

/**
 * @struct Homography
 * @brief Row-major 3-by-3 projective map.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Transform accepts either sign of W. BuildProjection requires positive W at control
 * points to keep adjacent chords oriented consistently.
 *
 * $$
 * \begin{bmatrix} X \\ Y \\ W
 * \end{bmatrix}
 * = H \begin{bmatrix} x \\ y \\ 1 \end{bmatrix},
 * \qquad (u,v) =
 * \left(\frac{X}{W},\frac{Y}{W}\right),
 * \qquad |W| > \epsilon.
 * $$
 */
struct Homography
{
    /// Row-major numerator rows followed by denominator; default is identity.
    std::array<double, 9> m{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    /**
     * @fn bool Transform(const Vec2& point, Vec2& out, double epsilon = 1e-12) const
     * @brief Map a point and apply the perspective divide.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @param point   Source point.
     * @param out     Receives the mapped point. It is set to zero when the
     *                denominator test fails, and holds a non-finite value when
     *                the divide itself overflows.
     * @param epsilon Smallest accepted absolute denominator.
     * @return True when the mapped point is finite; false for a non-finite or
     *         near-zero denominator, or for a non-finite quotient.
     */
    bool Transform(const Vec2& point, Vec2& out, double epsilon = 1e-12) const
    {
        const double w = m[6] * point.x + m[7] * point.y + m[8];
        if (!std::isfinite(w) || std::abs(w) <= epsilon)
        {
            out = {};
            return false;
        }
        out.x = (m[0] * point.x + m[1] * point.y + m[2]) / w;
        out.y = (m[3] * point.x + m[4] * point.y + m[5]) / w;
        return std::isfinite(out.x) && std::isfinite(out.y);
    }

    /**
     * @fn double Denominator(const Vec2& point) const
     * @brief Sample the denominator row at a point, without the divide.
     * @author Alex (<https://github.com/lextpf>)
     *
     * BuildProjection requires positive W at each control point to reject folds.
     *
     * @param point Source point.
     * @return The denominator, which is the homogeneous W of the mapped point.
     */
    double Denominator(const Vec2& point) const { return m[6] * point.x + m[7] * point.y + m[8]; }
};

/**
 * @struct AffinePlane
 * @brief Affine scalar field over two-dimensional coordinates.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The field is `value = xSlope*x + ySlope*y + constant`.
 */
struct AffinePlane
{
    double xSlope = 0.0;
    double ySlope = 0.0;
    double constant = 0.0;

    /**
     * @fn double Sample(double x, double y) const
     * @brief Evaluate the affine field at a two-dimensional point.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return The scalar value without clamping.
     */
    double Sample(double x, double y) const { return xSlope * x + ySlope * y + constant; }
};

namespace detail
{
using Matrix3 = std::array<double, 9>;

/**
 * @fn Matrix3 Multiply(const Matrix3& lhs, const Matrix3& rhs)
 * @brief Multiply two row-major 3-by-3 matrices.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param lhs  Left matrix.
 * @param rhs Right matrix.
 * @return     The product `lhs * rhs`.
 */
inline Matrix3 Multiply(const Matrix3& lhs, const Matrix3& rhs)
{
    Matrix3 out{};
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t col = 0; col < 3; ++col)
        {
            for (std::size_t k = 0; k < 3; ++k)
            {
                out[row * 3 + col] += lhs[row * 3 + k] * rhs[k * 3 + col];
            }
        }
    }
    return out;
}

/**
 * @brief Coefficient matrix of a square system with the right-hand side appended.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The row length is one greater than the row count: N coefficient columns, then the constant.
 *
 * @tparam N Number of unknowns.
 */
template <std::size_t N>
using AugmentedMatrix = double[N][N + 1];

/**
 * @fn bool SolveLinear(AugmentedMatrix<N>& augmented, std::array<double, N>& out, double epsilon)
 * @brief Solve a square system by Gauss-Jordan elimination with scaled pivoting.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Score pivots by each row maximum to handle mixed pixel and unit scales.
 *
 * @tparam N        Number of unknowns.
 * @param augmented The N by N+1 augmented matrix. It is overwritten in place, including on a failed
 * solve.
 * @param out       Receives the N solution values. It is only meaningful when the function returns
 * true.
 * @param epsilon   Degeneracy threshold. A row scale, a scaled pivot score, or a pivot relative to
 * its row scale at or below it fails.
 * @return True when every unknown resolved to a finite value.
 */
template <std::size_t N>
inline bool SolveLinear(AugmentedMatrix<N>& augmented, std::array<double, N>& out, double epsilon)
{
    std::array<double, N> rowScale{};
    for (std::size_t row = 0; row < N; ++row)
    {
        for (std::size_t col = 0; col < N; ++col)
        {
            rowScale[row] = std::max(rowScale[row], std::abs(augmented[row][col]));
        }
        if (!std::isfinite(rowScale[row]) || rowScale[row] <= epsilon)
        {
            return false;
        }
    }

    for (std::size_t col = 0; col < N; ++col)
    {
        std::size_t pivot = col;
        double pivotScore = 0.0;
        for (std::size_t row = col; row < N; ++row)
        {
            const double score = std::abs(augmented[row][col]) / rowScale[row];
            if (score > pivotScore)
            {
                pivotScore = score;
                pivot = row;
            }
        }
        if (!std::isfinite(pivotScore) || pivotScore <= epsilon)
        {
            return false;
        }
        if (pivot != col)
        {
            for (std::size_t k = 0; k <= N; ++k)
            {
                std::swap(augmented[col][k], augmented[pivot][k]);
            }
            std::swap(rowScale[col], rowScale[pivot]);
        }

        const double divisor = augmented[col][col];
        if (!std::isfinite(divisor) || std::abs(divisor) <= epsilon * rowScale[col])
        {
            return false;
        }
        for (std::size_t k = col; k <= N; ++k)
        {
            augmented[col][k] /= divisor;
        }

        for (std::size_t row = 0; row < N; ++row)
        {
            if (row == col)
            {
                continue;
            }
            const double factor = augmented[row][col];
            if (factor == 0.0)
            {
                continue;
            }
            for (std::size_t k = col; k <= N; ++k)
            {
                augmented[row][k] -= factor * augmented[col][k];
            }
        }
    }

    for (std::size_t row = 0; row < N; ++row)
    {
        out[row] = augmented[row][N];
        if (!std::isfinite(out[row]))
        {
            return false;
        }
    }
    return true;
}

/**
 * @fn bool Normalization(const std::array<Vec2, 4>& points, std::array<Vec2, 4>& normalized,
 *     Matrix3& transform, Matrix3& inverse, double epsilon)
 * @brief Hartley normalization of four points: centre them, then scale them.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Center points and scale their mean centroid distance to sqrt(2) to condition the solve.
 *
 * @param points     Four input points.
 * @param normalized Receives the centred and scaled points.
 * @param transform  Receives the map from input space to normalized space.
 * @param inverse    Receives the exact inverse of that map.
 * @param epsilon    Smallest accepted mean distance from the centroid.
 * @return True on success; false for a non-finite point or a point set that is effectively one
 * point. All three out parameters are then left unchanged.
 */
inline bool Normalization(const std::array<Vec2, 4>& points,
                          std::array<Vec2, 4>& normalized,
                          Matrix3& transform,
                          Matrix3& inverse,
                          double epsilon)
{
    Vec2 center{};
    for (const auto& point : points)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y))
        {
            return false;
        }
        center.x += point.x;
        center.y += point.y;
    }
    center.x *= 0.25;
    center.y *= 0.25;

    double meanDistance = 0.0;
    for (const auto& point : points)
    {
        meanDistance += std::hypot(point.x - center.x, point.y - center.y);
    }
    meanDistance *= 0.25;
    if (!std::isfinite(meanDistance) || meanDistance <= epsilon)
    {
        return false;
    }

    const double scale = std::sqrt(2.0) / meanDistance;
    transform = {scale, 0.0, -scale * center.x, 0.0, scale, -scale * center.y, 0.0, 0.0, 1.0};
    inverse = {1.0 / scale, 0.0, center.x, 0.0, 1.0 / scale, center.y, 0.0, 0.0, 1.0};
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        normalized[i] = {(points[i].x - center.x) * scale, (points[i].y - center.y) * scale};
    }
    return true;
}
}  // namespace detail

/**
 * @fn bool SolveHomography(const std::array<Vec2, 4>& source, const std::array<Vec2, 4>&
 *     destination, Homography& out, double epsilon = 1e-10)
 * @brief Solve the unique projective map between two non-degenerate four-point sets.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Use normalized coordinates and scaled pivots for pixel and small screen-space quads.
 *
 * @param source      Four source points, in correspondence order.
 * @param destination Four destination points, in the same order.
 * @param out         Receives the solved map. It is cleared first.
 * @param epsilon     Degeneracy threshold for normalization, elimination, and the denominator test.
 * @return True when the solve succeeded and every correspondence reprojects to within 1e-7 times
 * the largest absolute destination coordinate, where that coordinate is treated as at least 1.
 * @post The returned matrix is scaled so its largest absolute entry is exactly 1, and signed so the
 * denominator at the source-set centroid is positive. BuildProjection depends on both halves of
 * that gauge: it rejects any chord whose denominator is not positive at its own control points,
 * rescales every later chord to match its neighbour's denominator at a shared point, then requires
 * the two to still agree at two shared corners within a tolerance that never tightens below 1e-4
 * absolute. A change to either convention makes the wrapped solve reject its chords and fall back
 * to the flat single-quad plate, without failing this function's own validation.
 */
inline bool SolveHomography(const std::array<Vec2, 4>& source,
                            const std::array<Vec2, 4>& destination,
                            Homography& out,
                            double epsilon = 1e-10)
{
    out = {};
    std::array<Vec2, 4> src{};
    std::array<Vec2, 4> dst{};
    detail::Matrix3 srcTransform{};
    detail::Matrix3 srcInverse{};
    detail::Matrix3 dstTransform{};
    detail::Matrix3 dstInverse{};
    if (!detail::Normalization(source, src, srcTransform, srcInverse, epsilon) ||
        !detail::Normalization(destination, dst, dstTransform, dstInverse, epsilon))
    {
        return false;
    }

    double equations[8][9]{};
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        const double x = src[i].x;
        const double y = src[i].y;
        const double u = dst[i].x;
        const double v = dst[i].y;
        const std::size_t row = i * 2;
        equations[row][0] = x;
        equations[row][1] = y;
        equations[row][2] = 1.0;
        equations[row][6] = -u * x;
        equations[row][7] = -u * y;
        equations[row][8] = u;

        equations[row + 1][3] = x;
        equations[row + 1][4] = y;
        equations[row + 1][5] = 1.0;
        equations[row + 1][6] = -v * x;
        equations[row + 1][7] = -v * y;
        equations[row + 1][8] = v;
    }

    std::array<double, 8> solution{};
    if (!detail::SolveLinear(equations, solution, epsilon))
    {
        return false;
    }

    const detail::Matrix3 normalized{solution[0],
                                     solution[1],
                                     solution[2],
                                     solution[3],
                                     solution[4],
                                     solution[5],
                                     solution[6],
                                     solution[7],
                                     1.0};
    out.m = detail::Multiply(dstInverse, detail::Multiply(normalized, srcTransform));

    double largest = 0.0;
    for (double value : out.m)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        largest = std::max(largest, std::abs(value));
    }
    if (largest <= epsilon)
    {
        return false;
    }
    for (double& value : out.m)
    {
        value /= largest;
    }

    Vec2 sourceCenter{};
    for (const auto& point : source)
    {
        sourceCenter.x += point.x * 0.25;
        sourceCenter.y += point.y * 0.25;
    }
    double centerDenominator = out.Denominator(sourceCenter);
    if (!std::isfinite(centerDenominator) || std::abs(centerDenominator) <= epsilon)
    {
        return false;
    }
    if (centerDenominator < 0.0)
    {
        for (double& value : out.m)
        {
            value = -value;
        }
        centerDenominator = -centerDenominator;
    }

    double destinationScale = 1.0;
    for (const auto& point : destination)
    {
        destinationScale = std::max({destinationScale, std::abs(point.x), std::abs(point.y)});
    }
    const double tolerance = 1e-7 * destinationScale;
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        Vec2 projected{};
        if (!out.Transform(source[i], projected, epsilon) ||
            std::hypot(projected.x - destination[i].x, projected.y - destination[i].y) > tolerance)
        {
            return false;
        }
    }
    return true;
}

/**
 * @fn bool SolveAffinePlane(const std::array<Vec3, 4>& samples, AffinePlane& out, double epsilon =
 *     1e-10, double residualTolerance = 1e-4)
 * @brief Fit an affine scalar plane through four samples, or reject the set.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Solve the largest normalized triangle exactly, then reject if any of the four residuals
 * exceeds the limit. This rejects non-planar strips rather than fitting least squares.
 *
 * @param samples           Four samples as `(x, y, value)`.
 * @param out               Receives the fitted plane. It is cleared first.
 * @param epsilon           Degeneracy threshold for normalization, triangle area, and elimination.
 * @param residualTolerance Relative tolerance. The absolute limit is `residualTolerance * max(1,
 * largest |value|)`.
 * @return True when the triangle is well conditioned and every residual is within the limit; false
 * otherwise.
 */
inline bool SolveAffinePlane(const std::array<Vec3, 4>& samples,
                             AffinePlane& out,
                             double epsilon = 1e-10,
                             double residualTolerance = 1e-4)
{
    out = {};
    std::array<Vec2, 4> points{};
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        if (!std::isfinite(samples[i].x) || !std::isfinite(samples[i].y) ||
            !std::isfinite(samples[i].z))
        {
            return false;
        }
        points[i] = {samples[i].x, samples[i].y};
    }

    std::array<Vec2, 4> normalized{};
    detail::Matrix3 transform{};
    detail::Matrix3 inverse{};
    if (!detail::Normalization(points, normalized, transform, inverse, epsilon))
    {
        return false;
    }

    // Pick the largest normalized-screen triangle. It gives the best
    // conditioned exact plane; the remaining sample is used for validation.
    std::array<std::size_t, 3> best{0, 1, 2};
    double bestArea = 0.0;
    for (std::size_t a = 0; a < 2; ++a)
    {
        for (std::size_t b = a + 1; b < 3; ++b)
        {
            for (std::size_t c = b + 1; c < 4; ++c)
            {
                const double area = std::abs(
                    (normalized[b].x - normalized[a].x) * (normalized[c].y - normalized[a].y) -
                    (normalized[b].y - normalized[a].y) * (normalized[c].x - normalized[a].x));
                if (area > bestArea)
                {
                    bestArea = area;
                    best = {a, b, c};
                }
            }
        }
    }
    if (!std::isfinite(bestArea) || bestArea <= epsilon)
    {
        return false;
    }

    double equations[3][4]{};
    for (std::size_t row = 0; row < best.size(); ++row)
    {
        const std::size_t i = best[row];
        equations[row][0] = normalized[i].x;
        equations[row][1] = normalized[i].y;
        equations[row][2] = 1.0;
        equations[row][3] = samples[i].z;
    }
    std::array<double, 3> normalizedPlane{};
    if (!detail::SolveLinear(equations, normalizedPlane, epsilon))
    {
        return false;
    }

    const double scale = transform[0];
    const double centerX = -transform[2] / scale;
    const double centerY = -transform[5] / scale;
    out.xSlope = normalizedPlane[0] * scale;
    out.ySlope = normalizedPlane[1] * scale;
    out.constant = normalizedPlane[2] - out.xSlope * centerX - out.ySlope * centerY;

    double valueScale = 1.0;
    for (const auto& sample : samples)
    {
        valueScale = std::max(valueScale, std::abs(sample.z));
    }
    for (const auto& sample : samples)
    {
        const double residual = std::abs(out.Sample(sample.x, sample.y) - sample.z);
        if (!std::isfinite(residual) || residual > residualTolerance * valueScale)
        {
            return false;
        }
    }
    return std::isfinite(out.xSlope) && std::isfinite(out.ySlope) && std::isfinite(out.constant);
}

/**
 * @fn bool SolveAffinePlaneLeastSquares(const Vec3* samples, std::size_t count, AffinePlane& out,
 *     double& maxResidual, double epsilon = 1e-10)
 * @brief Best-fit affine depth field over an arbitrary number of samples.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Minimize squared error over all samples and report the worst residual. Callers decide
 * acceptance; BuildProjection discards that residual, so wrapped depth has no accuracy bound.
 *
 * @param samples     Pointer to @p count samples as `(x, y, value)`.
 * @param count       Number of samples. Fewer than 3 returns false.
 * @param out         Receives the fitted plane. It is cleared first.
 * @param maxResidual Receives the largest absolute residual over the samples. It is set to 0 before
 * the solve.
 * @param epsilon     Degeneracy threshold for the normal-equation elimination.
 * @return True when the solve succeeded; false on a null pointer, too few samples, a non-finite
 * sample, a singular system, or a non-finite residual.
 */
inline bool SolveAffinePlaneLeastSquares(const Vec3* samples,
                                         std::size_t count,
                                         AffinePlane& out,
                                         double& maxResidual,
                                         double epsilon = 1e-10)
{
    out = {};
    maxResidual = 0.0;
    if (samples == nullptr || count < 3)
    {
        return false;
    }

    double sumX = 0.0, sumY = 0.0, sumZ = 0.0, sumXX = 0.0, sumXY = 0.0, sumYY = 0.0, sumXZ = 0.0,
           sumYZ = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const Vec3& sample = samples[i];
        if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || !std::isfinite(sample.z))
        {
            return false;
        }
        sumX += sample.x;
        sumY += sample.y;
        sumZ += sample.z;
        sumXX += sample.x * sample.x;
        sumXY += sample.x * sample.y;
        sumYY += sample.y * sample.y;
        sumXZ += sample.x * sample.z;
        sumYZ += sample.y * sample.z;
    }

    double equations[3][4] = {{sumXX, sumXY, sumX, sumXZ},
                              {sumXY, sumYY, sumY, sumYZ},
                              {sumX, sumY, static_cast<double>(count), sumZ}};
    std::array<double, 3> solution{};
    if (!detail::SolveLinear(equations, solution, epsilon))
    {
        return false;
    }
    if (!std::isfinite(solution[0]) || !std::isfinite(solution[1]) || !std::isfinite(solution[2]))
    {
        return false;
    }

    out.xSlope = solution[0];
    out.ySlope = solution[1];
    out.constant = solution[2];
    for (std::size_t i = 0; i < count; ++i)
    {
        const double residual = std::abs(out.Sample(samples[i].x, samples[i].y) - samples[i].z);
        if (!std::isfinite(residual))
        {
            return false;
        }
        maxResidual = std::max(maxResidual, residual);
    }
    return true;
}
}  // namespace Graffito::Math
