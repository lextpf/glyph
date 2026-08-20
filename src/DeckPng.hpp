#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

/**
 * @namespace Deck
 * @brief WIC PNG output for character cards.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Blocking disk I/O runs on the encoder worker. No shared state or game objects are used.
 */
namespace Deck
{
/**
 * @fn bool EncodeBgraPng(const std::filesystem::path& path, int width, int height, std::span<const
 *     std::uint8_t> bgra, std::string& error)
 * @brief Write packed straight-alpha BGRA8 pixels as a 96 DPI PNG.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Requires GUID_WICPixelFormat32bppBGRA; other negotiated formats fail.
 * Initializes a multithreaded COM apartment and balances successful initialization.
 * RPC_E_CHANGED_MODE reuses the caller's existing apartment without uninitializing it.
 *
 * @param path Destination in an existing directory. The caller must own this path.
 * @param width Positive width in pixels.
 * @param height Positive height in pixels.
 * @param bgra Exactly width * height * 4 bytes; stride and total size must fit UINT.
 * @param error Cleared on entry; contains a reason on false return and stays empty on success.
 * @return True after WIC commit; false on invalid arguments, COM initialization, or WIC failure.
 * @warning Any WIC failure attempts to delete the destination, even if it already existed and
 *          no bytes were written. Argument rejection and COM initialization failure preserve it.
 */
[[nodiscard]] bool EncodeBgraPng(const std::filesystem::path& path,
                                 int width,
                                 int height,
                                 std::span<const std::uint8_t> bgra,
                                 std::string& error);
}  // namespace Deck
