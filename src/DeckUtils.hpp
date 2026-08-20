#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * @namespace Deck
 * @brief Card capture helpers with no game or GPU dependencies.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Rarity rolls apply only to unique actors when RarityRolls is enabled.
 *
 * ### :material-cards-playing-outline: Rarity selection
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef step fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef out fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *     A[ActorRarityProfile]:::input --> B[RarityFromActor]:::step
 *     C[actor tier index]:::input --> D[RarityFromTier]:::step
 *     B --> E[higher rank wins]:::step
 *     D --> E
 *     F[NextDeckRarityRoll - DeterministicPhase sample]:::input --> G[ApplyRarityRoll]:::step
 *     E --> G
 *     G --> H[final Rarity]:::out
 *     H --> I[TreatmentTierForRarity]:::step
 *     I --> J[tier that paints the card]:::out
 * ```
 */
namespace Deck
{
/**
 * @enum Rarity
 * @brief Ordered rarity ranks from 0 through 4.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Integer comparisons and clamps depend on this order and band count.
 */
enum class Rarity : std::uint8_t
{
    Common,
    Uncommon,
    Rare,
    Epic,
    Legendary
};

/**
 * @enum RarityArchetype
 * @brief Actor families used by the rarity score.
 * @author Alex (<https://github.com/lextpf>)
 */
enum class RarityArchetype : std::uint8_t
{
    Humanoid,
    Beast,
    Undead,
    Daedra,
    Dragon
};

/**
 * @struct ActorRarityProfile
 * @brief Stable actor facts for intrinsic rarity.
 * @author Alex (<https://github.com/lextpf>)
 */
struct ActorRarityProfile
{
    /// Levels below 1 count as 1.
    int level = 1;
    RarityArchetype archetype = RarityArchetype::Humanoid;
    bool essential = false;  ///< Adds one rarity rank
};

/**
 * @struct PortraitCrop
 * @brief Source rectangle in pixels, with a top-left origin and positive y downward.
 * @author Alex (<https://github.com/lextpf>)
 */
struct PortraitCrop
{
    float x = .0f;
    float y = .0f;
    float width = .0f;
    float height = .0f;
};

/**
 * @fn float CardLayoutScale(float width, float height) noexcept
 * @brief Fit the 750x1050 reference layout uniformly into the target.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param width Target width, in pixels.
 * @param height Target height, in pixels.
 * @return Min(width / 750, height / 1050), without an upper clamp. Non-finite or
 *         non-positive dimensions return the unscaled factor 1.
 */
[[nodiscard]] float CardLayoutScale(float width, float height) noexcept;

/**
 * @fn bool CopyRgbaToBgra(const std::uint8_t* source, std::size_t sourceRowPitch, int width, int
 *     height, std::uint8_t* destination, std::size_t destinationSize) noexcept
 * @brief Copy padded RGBA8 rows to packed BGRA8 without changing alpha.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Buffers must not overlap. Writes width * height * 4 bytes; extra capacity stays untouched.
 *
 * @param source Mapped RGBA8 bytes, with straight alpha. Must not be null.
 * @param sourceRowPitch Source stride in bytes; at least width * 4.
 * @param width Positive width in pixels.
 * @param height Positive height in pixels.
 * @param destination Non-null buffer for packed BGRA8 bytes.
 * @param destinationSize Capacity in bytes; at least width * height * 4.
 * @return False without writes on invalid arguments or byte-count overflow.
 */
[[nodiscard]] bool CopyRgbaToBgra(const std::uint8_t* source,
                                  std::size_t sourceRowPitch,
                                  int width,
                                  int height,
                                  std::uint8_t* destination,
                                  std::size_t destinationSize) noexcept;

/**
 * @fn Rarity RarityFromTier(int tierIndex, int tierCount) noexcept
 * @brief Map a tier index to five rarity bands.
 * @author Alex (<https://github.com/lextpf>)
 *
 * For clamped index $i$ and ladder length $n$:
 *
 * $$b = \min\left(\left\lfloor \frac{5\,i}{n - 1} \right\rfloor,\; 4\right)$$
 *
 * @param tierIndex Zero-based index, clamped to [0, tierCount - 1].
 * @param tierCount Ladder length; counts below 2 return Common.
 * @return Common at the first tier, Legendary at the last when tierCount > 1.
 */
[[nodiscard]] Rarity RarityFromTier(int tierIndex, int tierCount) noexcept;

/**
 * @fn Rarity RarityFromActor(const ActorRarityProfile& profile) noexcept
 * @brief Score intrinsic rarity from stable actor facts.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Each level milestone at 20, 35, and 50 adds one rank. Essential status adds one.
 * The final rank cannot exceed Legendary; a level-50 Undead actor reaches that rank.
 *
 * | Archetype | Adjustment             |
 * |-----------|------------------------|
 * | Humanoid  | None                   |
 * | Daedra    | +1 at level 25         |
 * | Undead    | +1 at level 30         |
 * | Beast     | +1 at level 40         |
 * | Dragon    | Legendary at any level |
 *
 * @param profile Levels below 1 count as 1.
 * @return Intrinsic rarity before the roll.
 */
[[nodiscard]] Rarity RarityFromActor(const ActorRarityProfile& profile) noexcept;

/**
 * @fn Rarity ApplyRarityRoll(Rarity intrinsicRarity, float roll) noexcept
 * @brief Raise rarity when the rolled rank exceeds the intrinsic rank.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Uniform rolls give Common through Legendary odds of 45/25/15/10/5 percent.
 * These are rolled-rank odds; the intrinsic floor changes the returned distribution.
 *
 * @param intrinsicRarity Clamped to Common through Legendary.
 * @param roll Uniform sample in [0, 1), clamped if outside the range.
 * @return Higher rank; non-finite rolls retain the clamped intrinsic rank.
 */
[[nodiscard]] Rarity ApplyRarityRoll(Rarity intrinsicRarity, float roll) noexcept;

/**
 * @fn int TreatmentTierForRarity(Rarity rarity, int actorTierIndex, int tierCount) noexcept
 * @brief Choose a treatment tier within the resolved rarity band.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clamps the actor tier into the matching band. If the band is empty, uses
 *
 * $$t = \text{round}\!\left(\frac{r}{4}\,(n - 1)\right)$$
 *
 * Where $r$ is the rarity rank 0..4 and $n$ is the ladder length.
 *
 * @param rarity Resolved card rarity.
 * @param actorTierIndex Zero-based actor tier.
 * @param tierCount Ladder length; counts below 2 return 0.
 * @return Tier index in [0, tierCount - 1] when tierCount > 1.
 */
[[nodiscard]] int TreatmentTierForRarity(Rarity rarity, int actorTierIndex, int tierCount) noexcept;

/**
 * @fn std::string_view RarityName(Rarity rarity) noexcept
 * @brief Get the rarity display name.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return View of a static literal; invalid values return "Common".
 */
[[nodiscard]] std::string_view RarityName(Rarity rarity) noexcept;

/**
 * @fn PortraitCrop ComputePortraitCrop(float sourceWidth, float sourceHeight, float headX, float
 *     headY, float feetX, float feetY, float desiredAspect) noexcept
 * @brief Fit an aspect-preserving portrait crop around projected head and feet.
 * @author Alex (<https://github.com/lextpf>)
 *
 * All coordinates use source pixels, with positive y downward. Pad each side by
 * 12.5 percent of the longer span:
 *
 * $$s = \max\bigl(|x_f - x_h|,\; |y_f - y_h|\bigr), \qquad p = 0.125\,s$$
 *
 * $$w_0 = |x_f - x_h| + 2p, \qquad h_0 = |y_f - y_h| + 2p$$
 *
 * @verbatim
 * source image: top-left origin; +x right, +y down.
 *
 * +--------------------------------------------------+
 * |        +----------------------------+            |  final aspect crop
 * |        |      +--------------+      |            |
 * |        |      |      H       |      |            |  padded head-feet box
 * |        |      |              |      |            |
 * |        |      |      F       |      |            |
 * |        |      +--------------+      |            |
 * |        +----------------------------+            |
 * +--------------------------------------------------+
 *
 * H is the head point; F is the feet point.
 * near an edge, move the crop inside the source without changing its aspect.
 * @endverbatim
 *
 * Expand one axis to the requested aspect, fit uniformly inside the source, then
 * clamp the centered position. Near an edge, the subject can be off-center.
 * Non-finite projections or spans at most 0.001 pixels use the largest centered crop.
 *
 * @param sourceWidth Finite positive source width in pixels.
 * @param sourceHeight Finite positive source height in pixels.
 * @param headX Projected head x.
 * @param headY Projected head y.
 * @param feetX Projected feet x.
 * @param feetY Projected feet y.
 * @param desiredAspect Width / height; non-finite or non-positive values use the source aspect.
 * @return Crop wholly inside the source; invalid source dimensions return an empty rectangle.
 */
[[nodiscard]] PortraitCrop ComputePortraitCrop(float sourceWidth,
                                               float sourceHeight,
                                               float headX,
                                               float headY,
                                               float feetX,
                                               float feetY,
                                               float desiredAspect) noexcept;

/**
 * @fn float DeterministicPhase(std::uint32_t formID) noexcept
 * @brief Derive a repeatable phase from an integer seed.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The upper 24 hash bits give multiples of 2^-24. Distribution affects rarity odds:
 * NextDeckRarityRoll mixes a clock and serial into the seed before calling this helper.
 *
 * @param formID Seed; zero yields zero.
 * @return Phase in [0, 1).
 */
[[nodiscard]] float DeterministicPhase(std::uint32_t formID) noexcept;

/**
 * @fn std::string SanitizeFilenameStem(std::string_view value)
 * @brief Make a Windows-safe ASCII filename stem from an actor name.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Unsafe or non-ASCII byte runs become one underscore; trailing spaces and dots are removed.
 * Prefixes reserved device names with an underscore, ignoring case and extensions.
 * The reserved set is CON, PRN, AUX, NUL, CLOCK$, CONIN$, CONOUT$, COM1..9, and LPT1..9.
 * The 220-byte cap reserves space for the caller's FormID, timestamp, suffix, and extension.
 *
 * @param value Actor name in UTF-8.
 * @return Stem, or "Actor" when empty after sanitization.
 */
[[nodiscard]] std::string SanitizeFilenameStem(std::string_view value);

/**
 * @fn float FitBadgeStrip(int count, float icon, float gap, float available) noexcept
 * @brief Shrink badge sizes and gaps together to fit the width budget.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param count Number of badges; non-positive counts return 1.
 * @param icon Icon edge, in the same unit as available.
 * @param gap Space between adjacent badges, in the same unit.
 * @param available Width budget; non-finite or non-positive values return 1.
 * @return Factor in [0, 1]; invalid total strip widths return 1.
 */
[[nodiscard]] float FitBadgeStrip(int count, float icon, float gap, float available) noexcept;
}  // namespace Deck
