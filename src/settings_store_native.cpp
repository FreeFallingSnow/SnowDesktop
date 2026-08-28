#include "settings_controller.h"

#include "l10n.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>

namespace snowdesktop
{
namespace
{
class NativeSettingsStore final : public SettingsStore
{
public:
    SettingsActionResult Load(SettingsValues& values) override
    {
        SettingsActionResult result = SettingsActionResult::Success();
        const auto recordLoadFailure = [&result](
            SettingsDomain domain,
            const wchar_t* message) {
            result.status = SettingsActionStatus::Failed;
            result.failedDomains |= domain;
            if (!result.message.empty()) result.message += L"\n";
            result.message += message;
        };
        const auto fileExists = [&recordLoadFailure](
            const std::wstring& path,
            SettingsDomain domain) {
            std::error_code error;
            const bool exists = std::filesystem::exists(path, error);
            if (error)
            {
                recordLoadFailure(
                    domain,
                    L"Failed to inspect a settings file.");
            }
            return exists;
        };

        bool categorizedTabHeightLoaded = false;
        const std::wstring personalizationPath = GetPersonalizationPath();
        const bool personalizationExists = fileExists(
            personalizationPath, SettingsDomain::Personalization);
        const bool personalizationLoaded = LoadPersonalization(
            personalizationPath.c_str(),
            values.personalization,
            &categorizedTabHeightLoaded);
        if (personalizationExists && !personalizationLoaded)
        {
            recordLoadFailure(
                SettingsDomain::Personalization,
                L"Failed to read personalization settings.");
        }

        const std::wstring dockPath = GetDockSettingsPath();
        if (!LoadDockSettings(dockPath.c_str(), values.dock) &&
            fileExists(dockPath, SettingsDomain::Dock))
        {
            recordLoadFailure(
                SettingsDomain::Dock,
                L"Failed to read Dock settings.");
        }
        NormalizeDockSettings(values.dock);

        const std::wstring navigationPath = GetNavigationSettingsPath();
        if (!LoadNavigationSettings(
                navigationPath.c_str(), values.navigation) &&
            fileExists(navigationPath, SettingsDomain::Navigation))
        {
            recordLoadFailure(
                SettingsDomain::Navigation,
                L"Failed to read navigation settings.");
        }

        const std::wstring generalPath = GetGeneralSettingsPath();
        if (!LoadGeneralSettings(generalPath.c_str(), values.general) &&
            fileExists(generalPath, SettingsDomain::General))
        {
            recordLoadFailure(
                SettingsDomain::General,
                L"Failed to read general settings.");
        }

        const std::wstring categoryPath = GetCategorySettingsPath();
        if (!LoadCategorySettings(categoryPath.c_str(), values.category) &&
            fileExists(categoryPath, SettingsDomain::Category))
        {
            recordLoadFailure(
                SettingsDomain::Category,
                L"Failed to read category settings.");
        }

        if (std::strcmp(values.general.language, "system") != 0 &&
            !Locale::Instance().HasLanguage(values.general.language))
        {
            std::strncpy(
                values.general.language,
                "system",
                sizeof(values.general.language) - 1);
            values.general.language[sizeof(values.general.language) - 1] = '\0';
        }

        if ((!personalizationExists || personalizationLoaded) &&
            !categorizedTabHeightLoaded)
        {
            // Before 1.0.1.0 this value lived in category settings as a font
            // size. Preserve the existing one-time migration while moving the
            // state owner out of SettingsWindow.
            values.personalization.categorizedTabHeight = std::clamp(
                values.category.tabFontSize * 34.0f / 15.0f,
                24.0f,
                48.0f);
            if (!::SavePersonalization(
                    GetPersonalizationPath().c_str(),
                    values.personalization))
            {
                recordLoadFailure(
                    SettingsDomain::Personalization,
                    L"Failed to persist the categorized-tab-height migration.");
            }
        }

        // Even on a per-file read error, defaults provide a usable snapshot.
        // The failed domain remains visible so WinUI can show an InfoBar, and
        // no damaged file is overwritten merely because loading failed.
        result.completedDomains = SettingsDomain::NativeStored;
        return result;
    }

    SettingsActionResult SavePersonalization(
        const PersonalizationSettings& settings) override
    {
        if (::SavePersonalization(
                GetPersonalizationPath().c_str(), settings))
        {
            return SettingsActionResult::Success(
                SettingsDomain::Personalization);
        }
        return SettingsActionResult::Failure(
            L"Failed to save personalization settings.",
            SettingsDomain::Personalization);
    }

    SettingsActionResult SaveDock(
        const DockSettings& settings) override
    {
        if (::SaveDockSettings(GetDockSettingsPath().c_str(), settings))
            return SettingsActionResult::Success(SettingsDomain::Dock);
        return SettingsActionResult::Failure(
            L"Failed to save Dock settings.", SettingsDomain::Dock);
    }

    SettingsActionResult SaveNavigation(
        const NavigationSettings& settings) override
    {
        if (::SaveNavigationSettings(
                GetNavigationSettingsPath().c_str(), settings))
        {
            return SettingsActionResult::Success(
                SettingsDomain::Navigation);
        }
        return SettingsActionResult::Failure(
            L"Failed to save navigation settings.",
            SettingsDomain::Navigation);
    }

    SettingsActionResult SaveGeneral(
        const GeneralSettings& settings) override
    {
        if (::SaveGeneralSettings(GetGeneralSettingsPath().c_str(), settings))
            return SettingsActionResult::Success(SettingsDomain::General);
        return SettingsActionResult::Failure(
            L"Failed to save general settings.", SettingsDomain::General);
    }

    SettingsActionResult SaveCategory(
        const CategorySettings& settings) override
    {
        if (::SaveCategorySettings(
                GetCategorySettingsPath().c_str(), settings))
        {
            return SettingsActionResult::Success(
                SettingsDomain::Category);
        }
        return SettingsActionResult::Failure(
            L"Failed to save category settings.",
            SettingsDomain::Category);
    }
};
} // namespace

std::shared_ptr<SettingsStore> CreateNativeSettingsStore()
{
    return std::make_shared<NativeSettingsStore>();
}
} // namespace snowdesktop
