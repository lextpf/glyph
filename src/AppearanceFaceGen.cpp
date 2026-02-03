#include "AppearanceTemplateInternal.h"

#include <iomanip>
#include <sstream>
#include <vector>

namespace AppearanceTemplate
{
namespace
{
// Compute the FaceGen file ID for a plugin
// Light plugins (ESL/ESPFE): FaceGen uses lower 12 bits (e.g., FEDC6810 -> 00000810)
// Regular plugins (ESP/ESM): FaceGen uses lower 24 bits (e.g., 05012345 -> 00012345)
RE::FormID FaceGenFileID(RE::FormID resolvedFormID, const RE::TESFile* plugin)
{
    if (plugin && plugin->IsLight())
    {
        // Light plugins: extract lower 12 bits
        return resolvedFormID & 0x00000FFF;
    }
    // Regular plugins: extract lower 24 bits
    return resolvedFormID & 0x00FFFFFF;
}
}  // namespace

std::string BuildFaceGenMeshPath(const std::string& pluginName, RE::FormID formID)
{
    // FormID is stored as 8-digit uppercase hex
    std::ostringstream ss;
    ss << "meshes\\actors\\character\\facegendata\\facegeom\\" << pluginName << "\\"
       << std::setfill('0') << std::setw(8) << std::hex << std::uppercase << formID << ".nif";
    return ss.str();
}

std::string BuildFaceGenTintPath(const std::string& pluginName, RE::FormID formID)
{
    std::ostringstream ss;
    ss << "textures\\actors\\character\\facegendata\\facetint\\" << pluginName << "\\"
       << std::setfill('0') << std::setw(8) << std::hex << std::uppercase << formID << ".dds";
    return ss.str();
}

bool ApplyFaceGen(RE::TESNPC* templateNPC)
{
    if (!templateNPC)
    {
        logger::error("AppearanceTemplate: ApplyFaceGen called with null template");
        return false;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player)
    {
        logger::error("AppearanceTemplate: Player not available for FaceGen");
        return false;
    }

    auto playerBase = player->GetActorBase();
    if (!playerBase)
    {
        logger::error("AppearanceTemplate: Player base not available for FaceGen");
        return false;
    }

    // Get BSFaceGenManager
    auto faceGenManager = RE::BSFaceGenManager::GetSingleton();
    if (!faceGenManager)
    {
        logger::warn("AppearanceTemplate: BSFaceGenManager not available");
        return false;
    }

    // Build candidate plugin list for FaceGen, in priority order:
    // 1) INI override (TemplateFaceGenPlugin)
    // 2) Winning overrides (sourceFiles)
    // 3) User-specified (TemplatePlugin)
    // 4) Origin/master (sourceFiles)
    std::string faceGenPluginOverride;
    std::string templatePluginSetting;
    {
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        const auto& app = Settings::Appearance();
        faceGenPluginOverride = app.TemplateFaceGenPlugin;
        templatePluginSetting = app.TemplatePlugin;
    }

    std::vector<const RE::TESFile*> candidates;
    auto* dh = RE::TESDataHandler::GetSingleton();
    auto lookupFile = [&](const std::string& name) -> const RE::TESFile*
    {
        if (!dh)
        {
            return nullptr;
        }
        for (auto* f : dh->files)
        {
            if (f && f->fileName && _stricmp(f->fileName, name.c_str()) == 0)
            {
                return f;
            }
        }
        return nullptr;
    };

    if (!faceGenPluginOverride.empty())
    {
        if (auto* f = lookupFile(faceGenPluginOverride))
        {
            candidates.push_back(f);
            logger::info("AppearanceTemplate: Using INI override for FaceGen plugin: {}",
                         f->fileName);
        }
    }

    if (templateNPC->sourceFiles.array && !templateNPC->sourceFiles.array->empty())
    {
        // Winning to origin, manual reverse because TESFileArray has no rbegin/rend
        for (int i = static_cast<int>(templateNPC->sourceFiles.array->size()) - 1; i >= 0; --i)
        {
            auto* f = (*templateNPC->sourceFiles.array)[i];
            if (f)
            {
                candidates.push_back(f);
            }
        }
    }

    if (!templatePluginSetting.empty())
    {
        if (auto* f = lookupFile(templatePluginSetting))
        {
            candidates.push_back(f);
        }
    }

    // Ensure origin/master is tried last if not already
    if (templateNPC->sourceFiles.array && !templateNPC->sourceFiles.array->empty())
    {
        const RE::TESFile* origin = (*templateNPC->sourceFiles.array)[0];
        if (origin)
        {
            candidates.push_back(origin);
        }
    }

    // Deduplicate while preserving insertion order
    std::unordered_set<const RE::TESFile*> seen;
    seen.reserve(candidates.size());
    std::vector<const RE::TESFile*> unique;
    unique.reserve(candidates.size());
    for (auto* f : candidates)
    {
        if (seen.insert(f).second)
        {
            unique.push_back(f);
        }
    }

    RE::FormID resolvedFormID = templateNPC->GetFormID();
    std::string triedPrimary;
    std::string triedSecondary;
    std::string meshPath, tintPath;
    bool meshFound = false;

    for (size_t i = 0; i < unique.size(); ++i)
    {
        const RE::TESFile* plugin = unique[i];
        if (!plugin || !plugin->fileName)
        {
            continue;
        }
        RE::FormID faceID = FaceGenFileID(resolvedFormID, plugin);
        meshPath = BuildFaceGenMeshPath(plugin->fileName, faceID);
        tintPath = BuildFaceGenTintPath(plugin->fileName, faceID);

        RE::BSResourceNiBinaryStream meshStream(meshPath.c_str());
        if (i == 0)
        {
            triedPrimary = meshPath;
        }
        else
        {
            triedSecondary = meshPath;
        }

        if (meshStream.good())
        {
            logger::info("AppearanceTemplate: Found FaceGen mesh: {}", meshPath);
            logger::info("AppearanceTemplate: FaceGen lookup - plugin: {}, FormID: {:08X}",
                         plugin->fileName,
                         faceID);
            meshFound = true;
            break;
        }
        else
        {
            logger::debug("AppearanceTemplate: FaceGen not found at: {}", meshPath);
        }
    }

    if (!meshFound)
    {
        logger::warn("AppearanceTemplate: FaceGen mesh not found!");
        if (!triedPrimary.empty())
        {
            logger::warn("AppearanceTemplate: Tried primary path: {}", triedPrimary);
        }
        if (!triedSecondary.empty() && triedSecondary != triedPrimary)
        {
            logger::warn("AppearanceTemplate: Tried fallback path: {}", triedSecondary);
        }
        logger::warn("AppearanceTemplate: Falling back to record-only appearance copy");
        logger::warn(
            "AppearanceTemplate: To fix, ensure FaceGen files exist or set TemplateFaceGenPlugin "
            "in INI");
        return false;
    }

    // Check if tint exists, optional but good to know
    bool tintExists = false;
    {
        RE::BSResourceNiBinaryStream tintStream(tintPath.c_str());
        tintExists = tintStream.good();
    }
    logger::info("AppearanceTemplate: FaceGen tint exists: {}", tintExists ? "yes" : "no");

    // Apply FaceGen by setting faceNPC to the template
    // This tells the game where to load FaceGen data from
    playerBase->faceNPC = templateNPC;
    logger::info("AppearanceTemplate: Set faceNPC to template ({:08X})", resolvedFormID);

    if (tintExists)
    {
        logger::info("AppearanceTemplate: FaceGen tint will be loaded from: {}", tintPath);
    }

    return true;
}

}  // namespace AppearanceTemplate
