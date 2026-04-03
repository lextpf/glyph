#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#define IMGUI_DEFINE_MATH_OPERATORS

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <RE/F/FightReactions.h>

#include <dxgi.h>
#include <shlobj.h>
#include <ranges>
#include <shared_mutex>

#include <boost/functional/hash.hpp>
#include <unordered_map>
#include <unordered_set>

#include <freetype/freetype.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <xbyak/xbyak.h>
#include <srell.hpp>

#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

/// Export macro for DLL entry points
#define DLLEXPORT __declspec(dllexport)

/// Alias for SKSE's logging interface
namespace logger = SKSE::log;

/**
 * Extensions to CommonLibSSE's reverse-engineered types.
 *
 * Adds comparison and hash support for `BSPointerHandle` types, enabling
 * their use as keys in ordered and unordered containers.
 *
 * @ingroup Utilities
 */
namespace RE
{
/**
 * Less-than comparison for BSPointerHandle.
 *
 * Enables use in `std::map`, `std::set`, and sorted algorithms.
 * Compares the underlying native handle values.
 *
 * @tparam T Handle target type (e.g., Actor, TESObjectREFR).
 * @param[in] a_lhs Left-hand operand.
 * @param[in] a_rhs Right-hand operand.
 * @return `true` if lhs handle value is less than rhs.
 *
 * @see hash_value
 */
template <class T>
bool operator<(const RE::BSPointerHandle<T>& a_lhs, const RE::BSPointerHandle<T>& a_rhs)
{
    return a_lhs.native_handle() < a_rhs.native_handle();
}

/**
 * Boost-compatible hash function for BSPointerHandle.
 *
 * Enables use with `boost::hash` and `boost::unordered_map/set`.
 *
 * @tparam T Handle target type.
 * @param[in] a_handle Handle to hash.
 * @return Hash value derived from native handle.
 */
template <class T>
std::size_t hash_value(const BSPointerHandle<T>& a_handle)
{
    boost::hash<uint32_t> hasher;
    return hasher(a_handle.native_handle());
}
}  // namespace RE

/**
 * Hook utilities and STL extensions for SKSE plugins.
 *
 * Provides template functions for common hooking patterns used in SKSE plugins.
 * All hook functions use SKSE's trampoline for safe code redirection.
 *
 * @see SKSE::GetTrampoline()
 */
namespace Stl
{
/// Redirect a 5-byte `call` instruction to `T::thunk` via SKSE's trampoline.
///
/// The original callee address is saved in `T::func` so the hook can
/// forward to it.
///
/// @tparam T Hook descriptor providing `static decltype(func) func` and
///           `static thunk(...)`.
/// @param a_src Address of the `call` instruction to patch.
/// @pre @p a_src points to a 5-byte relative `call` instruction.
template <class T>
void WriteThunkCall(std::uintptr_t a_src)
{
    auto& trampoline = SKSE::GetTrampoline();
    T::func = trampoline.write_call<5>(a_src, T::thunk);
}

/// Overwrite a vtable entry with `T::thunk`.
///
/// Saves the original function pointer in `T::func`.
///
/// @tparam F Class whose vtable is patched. Must provide `VTABLE`.
/// @tparam T Hook descriptor providing `idx`, `func`, and `thunk`.
template <class F, class T>
void WriteVfunc()
{
    REL::Relocation<std::uintptr_t> vtbl{F::VTABLE[0]};
    T::func = vtbl.write_vfunc(T::idx, T::thunk);
}

/// Prologue-patching hook for functions that cannot use a simple call redirect.
///
/// Uses Xbyak to generate a trampoline that copies the first @p BYTES bytes
/// of the original function, then jumps back to `a_src + BYTES`. A 5-byte
/// branch at @p a_src is overwritten to redirect to `T::thunk`. The
/// trampoline address is stored in `T::func` so `T::thunk` can call the
/// original prologue.
///
/// @tparam T     Hook descriptor providing `func` and `thunk`.
/// @tparam BYTES Number of prologue bytes to relocate (must be >= 5).
/// @param a_src  Address of the function prologue to patch.
/// @pre @p BYTES covers complete instructions at @p a_src (no mid-instruction split).
template <class T, std::size_t BYTES>
void HookFunctionPrologue(std::uintptr_t a_src)
{
    struct Patch : Xbyak::CodeGenerator
    {
        Patch(std::uintptr_t a_originalFuncAddr, std::size_t a_originalByteLength)
        {
            for (size_t i = 0; i < a_originalByteLength; ++i)
            {
                db(*reinterpret_cast<std::uint8_t*>(a_originalFuncAddr + i));
            }

            jmp(ptr[rip]);
            dq(a_originalFuncAddr + a_originalByteLength);
        }
    };

    Patch p(a_src, BYTES);
    p.ready();

    auto& trampoline = SKSE::GetTrampoline();
    trampoline.write_branch<5>(a_src, T::thunk);

    auto alloc = trampoline.allocate(p.getSize());
    std::memcpy(alloc, p.getCode(), p.getSize());

    T::func = reinterpret_cast<std::uintptr_t>(alloc);
}

/// Create a view over enum values from @p first (inclusive) to @p last (exclusive).
///
/// Maps each underlying integer back to the enum type via `static_cast`.
/// @tparam E  Enum type (deduced from arguments).
/// @param first First enumerator in the range (included).
/// @param last  Past-the-end enumerator (excluded).
/// @return A `views::iota | views::transform` range of enum values.
constexpr inline auto EnumRange(auto first, auto last)
{
    auto result =
        std::views::iota(std::to_underlying(first), std::to_underlying(last)) |
        std::views::transform([](auto enum_val) { return static_cast<decltype(first)>(enum_val); });

    return result;
}

/**
 * Compile-time bidirectional enum-string mapping.
 *
 * @tparam E    Enum type.
 * @tparam N    Number of entries.
 */
template <typename E, std::size_t N>
struct EnumStringMap
{
    /// A single enum-value / string-name pair.
    struct Entry
    {
        std::string_view name;  ///< Display name for the enumerator.
        E value;                ///< Corresponding enum value.
    };

    std::array<Entry, N> entries;

    /// Look up an enum value by its string name.
    ///
    /// @param s        String to search for (exact match).
    /// @param fallback Value returned when no entry matches @p s.
    /// @return The matching enum value, or @p fallback if not found.
    constexpr E fromString(std::string_view s, E fallback) const
    {
        for (const auto& e : entries)
        {
            if (e.name == s)
            {
                return e.value;
            }
        }
        return fallback;
    }

    /// Convert an enum value to its string name.
    ///
    /// @param v Enum value to look up.
    /// @return The matching name, or `"unknown"` if not found.
    constexpr std::string_view toString(E v) const
    {
        for (const auto& e : entries)
        {
            if (e.value == v)
            {
                return e.name;
            }
        }
        return "unknown";
    }
};
}  // namespace Stl

/**
 * Select address offset based on Skyrim edition.
 *
 * Used with REL::ID or direct addresses to support both Skyrim SE and AE builds.
 * The correct value is selected at compile time based on the SKYRIM_AE define.
 *
 * @param se Skyrim SE (1.5.97) offset.
 * @param ae Skyrim AE (1.6.x) offset.
 * @return The appropriate offset for the current build target.
 */
#ifdef SKYRIM_AE
#define GLYPH_OFFSET(se, ae) ae
#else
#define GLYPH_OFFSET(se, ae) se
#endif

#include "Version.h"
