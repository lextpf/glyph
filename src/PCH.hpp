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

/// @brief Export marker for DLL entry points.
#define DLLEXPORT __declspec(dllexport)

/// @brief Short name for the SKSE logging interface.
namespace logger = SKSE::log;

/**
 * @namespace RE
 * @brief Handle comparison and hashing for standard and Boost containers.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 */
namespace RE
{
/**
 * @fn bool operator<(const RE::BSPointerHandle<T>& a_lhs, const RE::BSPointerHandle<T>& a_rhs)
 * @brief Order handles by their native value.
 * @author Alex (<https://github.com/lextpf>)
 */
template <class T>
bool operator<(const RE::BSPointerHandle<T>& a_lhs, const RE::BSPointerHandle<T>& a_rhs)
{
    return a_lhs.native_handle() < a_rhs.native_handle();
}

/**
 * @fn std::size_t hash_value(const BSPointerHandle<T>& a_handle)
 * @brief Hash the native handle value for Boost containers.
 * @author Alex (<https://github.com/lextpf>)
 */
template <class T>
std::size_t hash_value(const BSPointerHandle<T>& a_handle)
{
    boost::hash<uint32_t> hasher;
    return hasher(a_handle.native_handle());
}
}  // namespace RE

/**
 * @namespace Stl
 * @brief Trampoline and vtable hook helpers.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 *
 * Call and prologue hooks use the SKSE trampoline; `WriteVfunc` patches the vtable directly.
 */
namespace Stl
{
/**
 * @fn void WriteThunkCall(std::uintptr_t a_src)
 * @brief Redirect a call to the thunk and retain the original callee.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @tparam T Provides `thunk` and `func`.
 * @pre `a_src` points to a five-byte relative call.
 * @warning Missing address library IDs or exhausted trampoline space terminate through
 * `stl::report_and_fail`; exceptions cannot intercept that failure.
 */
template <class T>
void WriteThunkCall(std::uintptr_t a_src)
{
    auto& trampoline = SKSE::GetTrampoline();
    T::func = trampoline.write_call<5>(a_src, T::thunk);
}

/**
 * @fn void WriteVfunc()
 * @brief Replace a vtable slot and retain its original function in T::func.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @tparam F Provides `VTABLE`.
 * @tparam T Provides `idx`, `func`, and `thunk`.
 * @pre `T::idx` is a valid slot in `F::VTABLE[0]`; invalid indices corrupt memory.
 * @warning Missing address library IDs terminate through `stl::report_and_fail`.
 * An unwritable patch site can fail silently in release because `REL::safe_write` asserts.
 */
template <class F, class T>
void WriteVfunc()
{
    REL::Relocation<std::uintptr_t> vtbl{F::VTABLE[0]};
    T::func = vtbl.write_vfunc(T::idx, T::thunk);
}

/**
 * @fn void HookFunctionPrologue(std::uintptr_t a_src)
 * @brief Redirect a prologue and retain a trampoline for the original call.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Overwrite five bytes at `a_src`; copy all `BYTES` into the trampoline in `T::func`,
 * followed by a jump to `a_src + BYTES`.
 *
 * @verbatim
 * Patched entry:
 *   a_src ------------------------------> T::thunk
 *
 * Optional original call from the thunk:
 *   T::func --> [Copied BYTES] --> [Jump to a_src + BYTES] --> Remaining body
 * @endverbatim
 *
 * @tparam T Provides `func` and `thunk`.
 * @tparam BYTES At least five; must cover complete instructions.
 * @param a_src Readable function prologue address.
 * @pre Copied instructions remain valid at the trampoline address. The helper does not
 * relocate relative branches or RIP-relative operands.
 */
template <class T, std::size_t BYTES>
void HookFunctionPrologue(std::uintptr_t a_src)
{
    struct Patch : Xbyak::CodeGenerator
    {
        /**
         * @fn Patch(std::uintptr_t a_originalFuncAddr, std::size_t a_originalByteLength)
         * @brief Copy prologue bytes and append a jump to the remaining function body.
         * @author Alex (<https://github.com/lextpf>)
         *
         * The caller supplies complete instructions that remain valid at the new address.
         */
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

/**
 * @fn constexpr inline auto EnumRange(auto first, auto last)
 * @brief Iterate a contiguous half-open range of enum values.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param first Included lower bound.
 * @param last Excluded upper bound; its underlying value must not be less than first.
 * @return Lazy view of each underlying value cast to the enum type.
 * @pre Both bounds use the same enum type and delimit a contiguous set of valid values.
 */
constexpr inline auto EnumRange(auto first, auto last)
{
    auto result =
        std::views::iota(std::to_underlying(first), std::to_underlying(last)) |
        std::views::transform([](auto enum_val) { return static_cast<decltype(first)>(enum_val); });

    return result;
}

/**
 * @struct EnumStringMap
 * @brief Compile-time bidirectional enum-string mapping.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Names borrow their storage, which must outlive the map and returned views. Matching is exact
 * and case-sensitive. Duplicate names or values use the first entry.
 *
 * @tparam E Enum type.
 * @tparam N Number of entries.
 */
template <typename E, std::size_t N>
struct EnumStringMap
{
    /**
     * @struct Entry
     * @brief One enum-value and string-name pair.
     * @author Alex (<https://github.com/lextpf>)
     */
    struct Entry
    {
        std::string_view name;  ///< Display name for the enumerator.
        E value;                ///< Corresponding enum value.
    };

    std::array<Entry, N> entries;  ///< Name/value pairs, scanned linearly; first match wins.

    /**
     * @fn constexpr E fromString(std::string_view s, E fallback) const
     * @brief Match a name without case folding.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return First matching value, or the supplied fallback.
     */
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

    /**
     * @fn constexpr std::string_view toString(E v) const
     * @brief Find the display name for an enum value.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return Borrowed name from the first match, or the static literal "unknown".
     */
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
 * @brief Select an address offset for the detected Skyrim edition.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Used with `REL::ID` or a direct address so one DLL serves Skyrim SE and AE/GOG.
 * CommonLibSSE-NG picks the value that matches the detected runtime.
 *
 * @param se Skyrim SE (1.5.97) offset.
 * @param ae Skyrim AE/GOG (1.6.x) offset.
 * @return The appropriate offset for the current runtime.
 */
#define GLYPH_OFFSET(se, ae) REL::Relocate((se), (ae))

#include "Version.hpp"
