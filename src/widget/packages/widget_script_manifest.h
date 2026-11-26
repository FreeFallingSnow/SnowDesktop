#pragma once

#include "widget/packages/widget_package.h"
#include "widget/api/widget_logical_slot_declarations.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @struct LuaWidgetManifest
 * @brief 小部件清单元数据，解析自小部件目录下的清单文件
 *
 * 清单文件定义了小部件的名称、版本、描述、默认栅格尺寸及其声明的权限列表。
 * 引擎在加载小部件时读取该结构，用于权限校验和 UI 布局。
 */
struct LuaWidgetManifest
{
    struct SettingCondition
    {
        std::string key;
        std::string operation;
        std::vector<std::string> values;
    };
    struct Setting
    {
        std::string key;
        std::string label;
        std::string description;
        std::string agentDescription;
        std::string group;
        std::string validationMessage;
        std::string dependsOn;
        std::string type;
        std::string durationUnit = "seconds";
        std::string defaultValue;
        std::string searchKey;
        std::string binding;
        std::string access = "read";
        std::string emptyLabel;
        std::string noResultsLabel;
        double minValue = 0.0;
        double maxValue = 100.0;
        double stepValue = 0.1;
        int minLength = -1;
        int maxLength = -1;
        bool required = false;
        std::optional<SettingCondition> showWhen;
        std::optional<SettingCondition> enabledWhen;
        std::vector<std::string> options;
        std::vector<std::string> optionLabels;
        std::vector<std::string> defaultValues;
        std::vector<std::string> extensions;
    };
    struct SettingGroup
    {
        std::string id;
        std::string label;
        std::string description;
        bool collapsible = false;
        bool defaultExpanded = true;
    };
    struct SettingPreset
    {
        std::string id;
        std::string label;
        std::unordered_map<std::string, std::string> values;
        std::unordered_map<std::string, std::vector<std::string>>
            arrayValues;
        bool isDefault = false;
        std::optional<SettingCondition> showWhen;
    };
    bool hasManifest = false;          ///< 是否存在清单文件
    int schemaVersion = 0;             ///< 组件包清单版本
    std::string packageId;             ///< 不可变组件包 UUID
    std::string slug;                  ///< 人类可读短名称
    int apiVersion = 0;                ///< Lua 宿主 API 契约版本
    int dataVersion = 1;               ///< 实例存储结构版本
    bool confirmRemoval = false;       ///< 删除实例前由宿主确认数据丢失
    std::string name;                  ///< 小部件显示名称
    std::string nameKey;               ///< 小部件名称翻译键
    std::string version;               ///< 版本号字符串
    std::string description;           ///< 功能描述文本
    std::string descriptionKey;        ///< 小部件描述翻译键
    std::vector<std::string> titleKeys; ///< 脚本可管理的本地化标题键
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> locales; ///< 组件自带的多语言文本
    int defaultColumns = 1;            ///< 默认占据列数（桌面栅格）
    int defaultRows = 1;               ///< 默认占据行数（桌面栅格）
    std::optional<snowdesktop::widget::AspectGridSize> defaultAspectSize;
    int minColumns = 1;                ///< 最少占据列数
    int minRows = 1;                   ///< 最少占据行数
    int maxColumns = 0;                ///< 最多占据列数，0 表示不限制
    int maxRows = 0;                   ///< 最多占据行数，0 表示不限制
    int refreshIntervalMs = 0;          ///< manifest 声明的自动刷新间隔（ms），0 = 不自动刷新
    std::vector<std::string> networkDomains; ///< 可选的精确公网域名收窄范围
    std::vector<std::string> requiredFeatures; ///< 激活前必须满足的 v2 宿主特性
    std::vector<std::string> optionalFeatures; ///< 可由脚本探测并降级的 v2 宿主特性
    std::unordered_map<std::string, snowdesktop::widget::PackageResource>
        resources; ///< v2 清单声明的包内图片和私有字体
    /// v2 清单声明的宿主管理逻辑槽位。
    snowdesktop::widget_runtime::LogicalSlotDeclarations logicalSlots;
    std::vector<Setting> settings;        ///< 宿主生成的声明式设置
    std::vector<SettingGroup> settingGroups; ///< 声明式设置分组
    std::vector<SettingPreset> presets;   ///< 宿主生成的声明式预设
    std::string publisher;
    std::string minHostVersion;
    std::string preview;
    std::string previewIntroduction;
    std::string previewIntroductionKey;
    /// Preview-only storage values supplied by the component author.
    /// They are visible through storage.get() but never persisted.
    std::unordered_map<std::string, std::string> previewStorage;
    /// Maps preview storage names to keys in the manifest locale catalogs.
    std::unordered_map<std::string, std::string> previewStorageKeys;
    std::vector<snowdesktop::widget::PreviewVariant> previewVariants;
    std::vector<snowdesktop::widget::CreationPreset> creationPresets;
    std::string entry;
    std::string signature;
    bool signatureValid = true;
    std::vector<std::string> permissions; ///< 必需权限列表
    std::vector<std::string> optionalPermissions; ///< 可拒绝并降级的权限列表
};

namespace snowdesktop::widget_runtime
{
std::string HashWidgetFile(const std::wstring& path);
LuaWidgetManifest ReadWidgetScriptManifest(const std::wstring& scriptPath,
    const std::filesystem::path& packageRoot);
}
