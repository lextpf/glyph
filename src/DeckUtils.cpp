#include "DeckUtils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace Deck
{
namespace
{
// Equal padding on both axes keeps the margin proportional for any projected slope.
constexpr float PORTRAIT_PADDING = .125f;

// Reserves space within 255 bytes for "-FFFFFFFF-YYYYMMDD-HHMMSS-999.png".
constexpr std::size_t MAX_FILENAME_STEM_LENGTH = 220;

/**
 * @fn bool IsFinitePositive(float value) noexcept
 * @brief Reject non-finite and non-positive dimensions.
 * @author Alex (<https://github.com/lextpf>)
 */
bool IsFinitePositive(float value) noexcept
{
    return std::isfinite(value) && value > .0f;
}

/**
 * @fn PortraitCrop CenteredAspectCrop(float sourceWidth, float sourceHeight, float desiredAspect)
 *     noexcept
 * @brief Fit the largest centered crop with the requested aspect.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Tolerate float rounding so the source aspect returns the whole image.
 */
PortraitCrop CenteredAspectCrop(float sourceWidth, float sourceHeight, float desiredAspect) noexcept
{
    const double width = sourceWidth;
    const double height = sourceHeight;
    const double aspect = desiredAspect;
    const double sourceAspect = width / height;

    if (std::abs(sourceAspect - aspect) <=
        std::numeric_limits<float>::epsilon() * std::max(sourceAspect, aspect))
    {
        return {.0f, .0f, sourceWidth, sourceHeight};
    }

    double cropWidth = width;
    double cropHeight = height;
    if (sourceAspect > aspect)
    {
        cropWidth = height * aspect;
    }
    else
    {
        cropHeight = width / aspect;
    }

    if (!std::isfinite(cropWidth) || !std::isfinite(cropHeight) || cropWidth <= .0 ||
        cropHeight <= .0)
    {
        return {.0f, .0f, sourceWidth, sourceHeight};
    }

    return {static_cast<float>((width - cropWidth) * .5),
            static_cast<float>((height - cropHeight) * .5),
            static_cast<float>(cropWidth),
            static_cast<float>(cropHeight)};
}

/**
 * @fn bool IsUnsafeFilenameByte(unsigned char value) noexcept
 * @brief Identify bytes that cannot remain in the ASCII filename stem.
 * @author Alex (<https://github.com/lextpf>)
 */
bool IsUnsafeFilenameByte(unsigned char value) noexcept
{
    if (value < 32 || value >= 127)
    {
        return true;
    }

    switch (value)
    {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            return true;
        default:
            return false;
    }
}

/**
 * @fn char AsciiUpper(char value) noexcept
 * @brief Uppercase an ASCII byte without locale-dependent conversion.
 * @author Alex (<https://github.com/lextpf>)
 */
char AsciiUpper(char value) noexcept
{
    if (value >= 'a' && value <= 'z')
    {
        return static_cast<char>(value - ('a' - 'A'));
    }
    return value;
}

/**
 * @fn bool IsReservedDeviceName(std::string_view value) noexcept
 * @brief Recognize Windows device names before a filename extension.
 * @author Alex (<https://github.com/lextpf>)
 */
bool IsReservedDeviceName(std::string_view value) noexcept
{
    // Device names remain reserved with extensions.
    const std::size_t dot = value.find('.');
    std::size_t length = dot == std::string_view::npos ? value.size() : dot;
    while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '.'))
    {
        --length;
    }

    std::string name;
    name.reserve(length);
    for (std::size_t i = 0; i < length; ++i)
    {
        name.push_back(AsciiUpper(value[i]));
    }

    if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" || name == "CLOCK$" ||
        name == "CONIN$" || name == "CONOUT$")
    {
        return true;
    }

    if (name.size() == 4 && name[3] >= '1' && name[3] <= '9')
    {
        return name.compare(0, 3, "COM") == 0 || name.compare(0, 3, "LPT") == 0;
    }
    return false;
}

/**
 * @fn void TrimFilenameEnding(std::string& value)
 * @brief Remove trailing spaces and periods from a filename stem.
 * @author Alex (<https://github.com/lextpf>)
 */
void TrimFilenameEnding(std::string& value)
{
    while (!value.empty() && (value.back() == ' ' || value.back() == '.'))
    {
        value.pop_back();
    }
}
}  // namespace

Rarity RarityFromTier(int tierIndex, int tierCount) noexcept
{
    if (tierCount <= 1)
    {
        return Rarity::Common;
    }

    const int clampedIndex = std::clamp(tierIndex, 0, tierCount - 1);
    // Clamp the upper endpoint from band 5 to Legendary (band 4).
    const auto scaled = static_cast<std::int64_t>(clampedIndex) * 5;
    const int band = std::min(static_cast<int>(scaled / (tierCount - 1)), 4);
    return static_cast<Rarity>(band);
}

Rarity RarityFromActor(const ActorRarityProfile& profile) noexcept
{
    const int level = std::max(1, profile.level);
    int rank = 0;
    rank += level >= 20 ? 1 : 0;
    rank += level >= 35 ? 1 : 0;
    rank += level >= 50 ? 1 : 0;

    switch (profile.archetype)
    {
        case RarityArchetype::Dragon:
            rank = 4;
            break;
        case RarityArchetype::Undead:
            rank += level >= 30 ? 1 : 0;
            break;
        case RarityArchetype::Daedra:
            rank += level >= 25 ? 1 : 0;
            break;
        case RarityArchetype::Beast:
            rank += level >= 40 ? 1 : 0;
            break;
        case RarityArchetype::Humanoid:
            break;
    }
    rank += profile.essential ? 1 : 0;
    return static_cast<Rarity>(std::clamp(rank, 0, 4));
}

Rarity ApplyRarityRoll(Rarity intrinsicRarity, float roll) noexcept
{
    const int floorRank = std::clamp(static_cast<int>(intrinsicRarity), 0, 4);
    if (!std::isfinite(roll))
    {
        return static_cast<Rarity>(floorRank);
    }
    roll = std::clamp(roll, .0f, 1.0f);
    const int rolledRank = roll < .45f   ? 0
                           : roll < .70f ? 1
                           : roll < .85f ? 2
                           : roll < .95f ? 3
                                         : 4;
    return static_cast<Rarity>(std::max(floorRank, rolledRank));
}

int TreatmentTierForRarity(Rarity rarity, int actorTierIndex, int tierCount) noexcept
{
    if (tierCount <= 1)
    {
        return 0;
    }

    const int rarityRank = std::clamp(static_cast<int>(rarity), 0, 4);
    int first = -1;
    int last = -1;
    for (int i = 0; i < tierCount; ++i)
    {
        if (RarityFromTier(i, tierCount) == static_cast<Rarity>(rarityRank))
        {
            if (first < 0)
            {
                first = i;
            }
            last = i;
        }
    }
    if (first < 0)
    {
        // Short ladders leave empty bands; use the proportional tier.
        const float normalized = static_cast<float>(rarityRank) / 4.0f;
        return std::clamp(
            static_cast<int>(std::lround(normalized * (tierCount - 1))), 0, tierCount - 1);
    }
    return std::clamp(actorTierIndex, first, last);
}

float CardLayoutScale(float width, float height) noexcept
{
    if (!IsFinitePositive(width) || !IsFinitePositive(height))
    {
        return 1.0f;
    }
    return std::min(width / 750.0f, height / 1050.0f);
}

bool CopyRgbaToBgra(const std::uint8_t* source,
                    std::size_t sourceRowPitch,
                    int width,
                    int height,
                    std::uint8_t* destination,
                    std::size_t destinationSize) noexcept
{
    if (!source || !destination || width <= 0 || height <= 0 ||
        static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / 4)
    {
        return false;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    const std::size_t rowCount = static_cast<std::size_t>(height);
    // Check packed size and padded source offsets separately for overflow.
    if (sourceRowPitch < rowBytes ||
        rowCount > std::numeric_limits<std::size_t>::max() / rowBytes ||
        (rowCount - 1) > std::numeric_limits<std::size_t>::max() / sourceRowPitch)
    {
        return false;
    }
    const std::size_t requiredSize = rowBytes * rowCount;
    if (destinationSize < requiredSize)
    {
        return false;
    }

    for (std::size_t y = 0; y < rowCount; ++y)
    {
        const auto* sourceRow = source + y * sourceRowPitch;
        auto* destinationRow = destination + y * rowBytes;
        for (int x = 0; x < width; ++x)
        {
            const std::size_t pixel = static_cast<std::size_t>(x) * 4;
            destinationRow[pixel + 0] = sourceRow[pixel + 2];
            destinationRow[pixel + 1] = sourceRow[pixel + 1];
            destinationRow[pixel + 2] = sourceRow[pixel + 0];
            destinationRow[pixel + 3] = sourceRow[pixel + 3];
        }
    }
    return true;
}

std::string_view RarityName(Rarity rarity) noexcept
{
    switch (rarity)
    {
        case Rarity::Common:
            return "Common";
        case Rarity::Uncommon:
            return "Uncommon";
        case Rarity::Rare:
            return "Rare";
        case Rarity::Epic:
            return "Epic";
        case Rarity::Legendary:
            return "Legendary";
        default:
            return "Common";
    }
}

PortraitCrop ComputePortraitCrop(float sourceWidth,
                                 float sourceHeight,
                                 float headX,
                                 float headY,
                                 float feetX,
                                 float feetY,
                                 float desiredAspect) noexcept
{
    if (!IsFinitePositive(sourceWidth) || !IsFinitePositive(sourceHeight))
    {
        return {};
    }

    if (!IsFinitePositive(desiredAspect))
    {
        desiredAspect = sourceWidth / sourceHeight;
    }

    const PortraitCrop fallback = CenteredAspectCrop(sourceWidth, sourceHeight, desiredAspect);
    if (!std::isfinite(headX) || !std::isfinite(headY) || !std::isfinite(feetX) ||
        !std::isfinite(feetY))
    {
        return fallback;
    }

    const double deltaX = std::abs(static_cast<double>(feetX) - headX);
    const double deltaY = std::abs(static_cast<double>(feetY) - headY);
    const double span = std::max(deltaX, deltaY);
    if (!std::isfinite(span) || span <= .001)
    {
        return fallback;
    }

    const double padding = span * PORTRAIT_PADDING;
    const double subjectWidth = deltaX + padding * 2.0;
    const double subjectHeight = deltaY + padding * 2.0;
    const double aspect = desiredAspect;

    double cropWidth = subjectWidth;
    double cropHeight = subjectHeight;
    if (cropWidth / cropHeight > aspect)
    {
        cropHeight = cropWidth / aspect;
    }
    else
    {
        cropWidth = cropHeight * aspect;
    }

    const double fitScale = std::min({1.0,
                                      static_cast<double>(sourceWidth) / cropWidth,
                                      static_cast<double>(sourceHeight) / cropHeight});
    cropWidth *= fitScale;
    cropHeight *= fitScale;

    if (!std::isfinite(cropWidth) || !std::isfinite(cropHeight) || cropWidth <= .0 ||
        cropHeight <= .0)
    {
        return fallback;
    }

    const double centerX = (static_cast<double>(headX) + feetX) * .5;
    const double centerY = (static_cast<double>(headY) + feetY) * .5;
    const double maxX = std::max(0.0, static_cast<double>(sourceWidth) - cropWidth);
    const double maxY = std::max(0.0, static_cast<double>(sourceHeight) - cropHeight);
    const double cropX = std::clamp(centerX - cropWidth * .5, 0.0, maxX);
    const double cropY = std::clamp(centerY - cropHeight * .5, 0.0, maxY);

    return {static_cast<float>(cropX),
            static_cast<float>(cropY),
            static_cast<float>(cropWidth),
            static_cast<float>(cropHeight)};
}

float DeterministicPhase(std::uint32_t formID) noexcept
{
    // Decorrelate adjacent FormIDs; the upper 24 bits convert exactly to float.
    std::uint32_t hash = formID;
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15;
    hash *= 0x846CA68BU;
    hash ^= hash >> 16;
    return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);
}

std::string SanitizeFilenameStem(std::string_view value)
{
    std::string result;
    result.reserve(std::min(value.size(), MAX_FILENAME_STEM_LENGTH));

    bool replacing = false;
    for (const unsigned char byte : value)
    {
        if (result.size() >= MAX_FILENAME_STEM_LENGTH)
        {
            break;
        }

        if (IsUnsafeFilenameByte(byte))
        {
            if (!replacing)
            {
                result.push_back('_');
                replacing = true;
            }
            continue;
        }

        result.push_back(static_cast<char>(byte));
        replacing = false;
    }

    TrimFilenameEnding(result);
    if (result.empty())
    {
        return "Actor";
    }

    if (IsReservedDeviceName(result))
    {
        result.insert(result.begin(), '_');
        if (result.size() > MAX_FILENAME_STEM_LENGTH)
        {
            result.resize(MAX_FILENAME_STEM_LENGTH);
            TrimFilenameEnding(result);
        }
    }
    return result;
}

float FitBadgeStrip(int count, float icon, float gap, float available) noexcept
{
    if (count <= 0)
    {
        return 1.0f;
    }
    const float total = icon * static_cast<float>(count) + gap * static_cast<float>(count - 1);
    if (!IsFinitePositive(total) || !IsFinitePositive(available))
    {
        return 1.0f;
    }
    return total <= available ? 1.0f : available / total;
}
}  // namespace Deck
