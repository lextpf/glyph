#include "AppearanceTemplateInternal.h"

#include <limits>
#include <stdexcept>

namespace AppearanceTemplate
{
namespace
{
// Parse a hex string to FormID. Rejects sign prefixes, trailing chars, and values exceeding uint32
// range.
RE::FormID ParseHexFormID(const std::string& str)
{
    if (str.empty())
    {
        throw std::invalid_argument("empty form id");
    }
    if (str[0] == '-' || str[0] == '+')
    {
        throw std::invalid_argument("form id must not have a sign prefix");
    }
    size_t consumed = 0;
    const unsigned long long parsed = std::stoull(str, &consumed, 16);
    if (consumed != str.size())
    {
        throw std::invalid_argument("trailing characters in form id");
    }
    if (parsed > static_cast<unsigned long long>((std::numeric_limits<RE::FormID>::max)()))
    {
        throw std::out_of_range("form id overflow");
    }
    return static_cast<RE::FormID>(parsed);
}

// Construct a runtime FormID from a plugin file and a base (local) FormID.
//
// Light plugins (ESL/ESPFE): 0xFE | lightIndex(12 bits) | localID(12 bits)
//   - lightIndex from GetSmallFileCompileIndex(); 0xFFFF = not loaded
//   - localID is the lower 12 bits of the base FormID
//
// Regular plugins (ESP/ESM): compileIndex(8 bits) | localID(24 bits)
//   - compileIndex from GetCompileIndex(); 0xFF = not loaded
//   - localID is the lower 24 bits of the base FormID
RE::FormID BuildFormID(const RE::TESFile* file, RE::FormID baseFormID)
{
    if (!file)
    {
        return 0;
    }
    if (file->IsLight())
    {
        uint32_t lightIndex = file->GetSmallFileCompileIndex();
        if (lightIndex == 0xFFFF)
        {
            return 0;
        }
        return 0xFE000000 | (lightIndex << 12) | (baseFormID & 0xFFF);
    }
    const auto compileIndex = static_cast<uint32_t>(file->GetCompileIndex());
    if (compileIndex == 0xFF)
    {
        return 0;
    }
    return (static_cast<RE::FormID>(compileIndex) << 24) | (baseFormID & 0x00FFFFFF);
}

const RE::TESFile* FindPluginByName(RE::TESDataHandler* dh, const std::string& name)
{
    for (auto* file : dh->files)
    {
        if (file && file->fileName && _stricmp(file->fileName, name.c_str()) == 0)
        {
            return file;
        }
    }
    return nullptr;
}
}  // namespace

RE::FormID ResolveFormID(const std::string& formIdStr, const std::string& pluginName)
{
    if (formIdStr.empty() || pluginName.empty())
    {
        return 0;
    }

    RE::FormID baseFormID = 0;
    try
    {
        baseFormID = ParseHexFormID(formIdStr);
    }
    catch (const std::exception& e)
    {
        logger::error("AppearanceTemplate: Invalid FormID format '{}': {}", formIdStr, e.what());
        return 0;
    }

    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler)
    {
        logger::error("AppearanceTemplate: TESDataHandler not available");
        return 0;
    }

    const RE::TESFile* plugin = FindPluginByName(dataHandler, pluginName);
    if (!plugin)
    {
        logger::error("AppearanceTemplate: Plugin not found: {}", pluginName);
        return 0;
    }

    RE::FormID localFormID = baseFormID & 0x00FFFFFF;

    // Strategy 1: standard light/regular lookup via BuildFormID.
    RE::FormID resolvedFormID = BuildFormID(plugin, baseFormID);
    if (resolvedFormID != 0 && RE::TESForm::LookupByID(resolvedFormID))
    {
        logger::info("AppearanceTemplate: Resolved {}|{} to FormID {:08X}",
                     formIdStr,
                     pluginName,
                     resolvedFormID);
        return resolvedFormID;
    }

    // Fallback for ESL-flagged plugins whose local FormID exceeds the 12-bit
    // light range.  The engine may load these into a regular compile index slot
    // (AE 1.6.1130+ behaviour), but CommonLib reports GetCompileIndex()==0xFF
    // for light-flagged files.  Scan all possible regular slots to find the form.
    if (plugin->IsLight() && localFormID > 0xFFF)
    {
        for (uint32_t idx = 0; idx < 0xFF; ++idx)
        {
            RE::FormID candidate = (static_cast<RE::FormID>(idx) << 24) | localFormID;
            if (RE::TESForm::LookupByID(candidate))
            {
                logger::info("AppearanceTemplate: Resolved {}|{} via index scan to FormID {:08X}",
                             formIdStr,
                             pluginName,
                             candidate);
                return candidate;
            }
        }
    }

    logger::error(
        "AppearanceTemplate: Failed to resolve {} in configured plugin {} (candidate {:08X})",
        formIdStr,
        pluginName,
        resolvedFormID);
    return 0;
}

}  // namespace AppearanceTemplate
