#include "shell_icon_bitmap.h"

#include "icon_render_rules.h"
#include "shortcut_icon_resource.h"
#include "common/utils.h"
#include "shell/shell_call_diagnostics.h"
#include "shell/shortcut_application_rules.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>

namespace shellCalls = snowdesktop::shell_call_diagnostics;

namespace
{
std::wstring ExpandIconResourcePath(std::wstring_view path)
{
    if (path.empty())
        return {};

    std::wstring value(path);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);

    const DWORD required = ExpandEnvironmentStringsW(
        value.c_str(), nullptr, 0);
    if (required == 0)
        return value;

    std::vector<wchar_t> expanded(required);
    if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(),
            required) == 0)
        return value;
    return expanded.data();
}

std::wstring ResolveRelativeIconResourcePath(
    const std::wstring& shortcutPath, std::wstring iconPath)
{
    if (iconPath.empty() || !PathIsRelativeW(iconPath.c_str()))
        return iconPath;

    const size_t separator = shortcutPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return iconPath;
    return shortcutPath.substr(0, separator + 1) + iconPath;
}

HBITMAP ExtractIconResourceBitmap(const std::wstring& resourcePath,
    int iconIndex, int sourceSize, SIZE& bitmapSize)
{
    if (resourcePath.empty() || shellCalls::Call(
                                    L"Resource.GetFileAttributes",
                                    [&] {
                                        return GetFileAttributesW(resourcePath.c_str());
                                    },
                                    resourcePath) == INVALID_FILE_ATTRIBUTES)
        return nullptr;

    HICON icon = nullptr;
    const HRESULT result = shellCalls::Call(
        L"Resource.SHDefExtractIcon",
        [&] {
            return SHDefExtractIconW(resourcePath.c_str(), iconIndex, 0, &icon, nullptr,
                                     MAKELONG(sourceSize, 0));
        },
        resourcePath);
    if (FAILED(result) || !icon)
    {
        if (icon)
            DestroyIcon(icon);
        return nullptr;
    }

    HBITMAP bitmap = shellCalls::Call(
        L"Resource.CreateAlphaBitmap",
        [&] {
            return CreateAlphaBitmapFromIcon(icon, sourceSize, sourceSize, bitmapSize);
        },
        resourcePath);
    DestroyIcon(icon);
    return bitmap;
}

HBITMAP ExtractShellLinkSourceIcon(std::wstring_view sourcePath,
    int sourceSize, SIZE& bitmapSize)
{
    if (sourcePath.empty())
        return nullptr;

    const std::wstring shortcutPath(sourcePath);
    ComPtr<IShellLinkW> shellLink;
    if (FAILED(shellCalls::Call(L"Link.CoCreateInstance",
                                [&] {
                                    return CoCreateInstance(CLSID_ShellLink, nullptr,
                                                            CLSCTX_INPROC_SERVER,
                                                            IID_PPV_ARGS(&shellLink));
                                })) ||
        !shellLink)
        return nullptr;

    ComPtr<IPersistFile> persistFile;
    if (FAILED(shellCalls::Call(L"Link.QueryPersistFile",
                                [&] {
                                    return shellLink.As(&persistFile);
                                })) ||
        FAILED(shellCalls::Call(
            L"Link.Load",
            [&] {
                return persistFile->Load(shortcutPath.c_str(), STGM_READ);
            },
            shortcutPath)))
        return nullptr;

    std::vector<wchar_t> iconLocation(32768, L'\0');
    int iconIndex = 0;
    if (SUCCEEDED(shellCalls::Call(L"Link.GetIconLocation",
                                   [&] {
                                       return shellLink->GetIconLocation(
                                           iconLocation.data(),
                                           static_cast<int>(iconLocation.size()), &iconIndex);
                                   })) &&
        iconLocation[0] != L'\0')
    {
        std::wstring iconPath = ResolveRelativeIconResourcePath(
            shortcutPath, ExpandIconResourcePath(iconLocation.data()));
        if (HBITMAP bitmap = ExtractIconResourceBitmap(
                iconPath, iconIndex, sourceSize, bitmapSize))
            return bitmap;
    }

    std::vector<wchar_t> targetPath(32768, L'\0');
    if (SUCCEEDED(shellCalls::Call(L"Link.GetPath.Raw",
                                   [&] {
                                       return shellLink->GetPath(
                                           targetPath.data(), static_cast<int>(targetPath.size()),
                                           nullptr, SLGP_RAWPATH);
                                   })) &&
        targetPath[0] != L'\0')
    {
        std::wstring expandedTarget =
            ExpandIconResourcePath(targetPath.data());
        if (HBITMAP bitmap = ExtractIconResourceBitmap(
                expandedTarget, 0, sourceSize, bitmapSize))
            return bitmap;
    }

    return nullptr;
}

HBITMAP ExtractInternetShortcutSourceIcon(std::wstring_view sourcePath,
    int sourceSize, SIZE& bitmapSize)
{
    const auto resource = shellCalls::Call(
        L"Url.ReadIconResource",
        [&] {
            return snowdesktop::shortcut_icon_resource::ReadInternetShortcutIconResource(
                sourcePath);
        },
        sourcePath);
    if (!resource)
        return nullptr;
    return ExtractIconResourceBitmap(
        resource->path, resource->index, sourceSize, bitmapSize);
}
} // namespace

HBITMAP GetLocalIconResourceBitmap(std::wstring_view path, SIZE& bitmapSize, int requestedSize)
{
    bitmapSize = {};
    const auto resources = shellCalls::Call(L"Local.ReadIconResources", [&] {
        return snowdesktop::shortcut_icon_resource::ReadLocalIconResources(path);
    });
    const int sourceSize = snowdesktop::icon_render_rules::SourcePixelsForTarget(requestedSize);
    for (const auto& resource : resources)
        if (auto bitmap = ExtractIconResourceBitmap(resource.path, resource.index, sourceSize, bitmapSize))
            return bitmap;
    return nullptr;
}

HBITMAP GetDirectIconResourceBitmap(std::wstring_view resourcePath,
    int iconIndex, SIZE& bitmapSize, int requestedSize)
{
    bitmapSize = {};
    if (resourcePath.empty())
        return nullptr;

    const int sourceSize =
        snowdesktop::icon_render_rules::SourcePixelsForTarget(requestedSize);
    return ExtractIconResourceBitmap(
        std::wstring(resourcePath), iconIndex, sourceSize, bitmapSize);
}

/**
 * @brief 获取 Shell 项的高分辨率图标位图。
 *
 * 优先通过 IShellItemImageFactory 获取高分辨率缩略图或图标，
 * 失败时依次尝试 JUMBO/EXTRALARGE/LARGE 图像列表，
 * 最后回退到 SHGetFileInfo 获取图标。
 *
 * @param pidl Shell 项的绝对 PIDL。
 * @param fallbackIndex 图像列表中的回退索引。
 * @param bitmapSize [out] 输出位图的尺寸。
 * @param allowThumbnail 是否允许 Shell 返回缩略图表示。
 * @param requestedSize 目标源位图长边。
 * @param preferDirectIconExtraction 是否优先绕过 ImageFactory 直接提取 HICON。
 * @param forShortcut 是否按快捷方式语义请求未叠加箭头的源图标。
 * @param sourcePath Shell 项的文件系统路径。
 * @param returnedThumbnail [out] 实际返回缩略图时写入 true，普通图标或失败时写入 false。
 * @return 成功时返回带有 Alpha 通道的 HBITMAP，失败时返回 nullptr。
 */
HBITMAP GetHighResolutionShellIconBitmap(PCIDLIST_ABSOLUTE pidl,
    int fallbackIndex, SIZE& bitmapSize, bool allowThumbnail,
    int requestedSize, bool preferDirectIconExtraction,
    bool forShortcut, std::wstring_view sourcePath,
    bool* returnedThumbnail)
{
    bitmapSize = {};
    if (returnedThumbnail)
        *returnedThumbnail = false;
    const int sourceSize =
        snowdesktop::icon_render_rules::SourcePixelsForTarget(requestedSize);
    if (forShortcut)
    {
        namespace shortcutRules =
            snowdesktop::shortcut_application_rules;
        if (shortcutRules::HasExtension(sourcePath, L".url"))
        {
            if (HBITMAP bitmap = shellCalls::Call(
                    L"Url.SourceIcon",
                    [&] {
                        return ExtractInternetShortcutSourceIcon(sourcePath, sourceSize,
                                                                 bitmapSize);
                    },
                    sourcePath))
                return bitmap;
        }
        else if (shortcutRules::HasExtension(sourcePath, L".lnk"))
        {
            if (HBITMAP bitmap = shellCalls::Call(
                    L"Link.SourceIcon",
                    [&] {
                        return ExtractShellLinkSourceIcon(sourcePath, sourceSize, bitmapSize);
                    },
                    sourcePath))
                return bitmap;
        }
    }
    if (preferDirectIconExtraction)
    {
        ComPtr<IShellFolder> parentFolder;
        PCUITEMID_CHILD child = nullptr;
        if (SUCCEEDED(shellCalls::Call(L"Extract.SHBindToParent",
                                       [&] {
                                           return SHBindToParent(pidl, IID_PPV_ARGS(&parentFolder),
                                                                 &child);
                                       })) &&
            parentFolder && child)
        {
            PCUITEMID_CHILD children[]{ child };
            ComPtr<IExtractIconW> extractor;
            if (SUCCEEDED(shellCalls::Call(L"Extract.GetUIObjectOf",
                                           [&] {
                                               return parentFolder->GetUIObjectOf(
                                                   nullptr, 1, children, IID_IExtractIconW, nullptr,
                                                   reinterpret_cast<void**>(
                                                       extractor.GetAddressOf()));
                                           })) &&
                extractor)
            {
                std::vector<wchar_t> iconFile(32768, L'\0');
                int iconIndex = 0;
                UINT iconFlags = 0;
                const UINT locationFlags = GIL_FORSHELL |
                    (forShortcut ? GIL_FORSHORTCUT : 0u);
                const HRESULT locationResult = shellCalls::Call(L"Extract.GetIconLocation", [&] {
                    return extractor->GetIconLocation(locationFlags, iconFile.data(),
                                                      static_cast<UINT>(iconFile.size()),
                                                      &iconIndex, &iconFlags);
                });
                if (locationResult == S_OK)
                {
                    HICON icon = nullptr;
                    HRESULT extractResult = shellCalls::Call(
                        L"Extract.Extract",
                        [&] {
                            return extractor->Extract(iconFile.data(), static_cast<UINT>(iconIndex),
                                                      &icon, nullptr, MAKELONG(sourceSize, 0));
                        },
                        iconFile.data());
                    if (extractResult == S_FALSE && iconFile[0] != L'\0')
                    {
                        if (icon)
                        {
                            DestroyIcon(icon);
                            icon = nullptr;
                        }
                        extractResult = shellCalls::Call(
                            L"Extract.SHDefExtractIcon",
                            [&] {
                                return SHDefExtractIconW(iconFile.data(), iconIndex,
                                                         iconFlags & GIL_SIMULATEDOC, &icon,
                                                         nullptr, MAKELONG(sourceSize, 0));
                            },
                            iconFile.data());
                    }
                    if (SUCCEEDED(extractResult) && icon)
                    {
                        HBITMAP bitmap = shellCalls::Call(L"Bitmap.CreateAlphaBitmap", [&] {
                            return CreateAlphaBitmapFromIcon(icon, sourceSize, sourceSize,
                                                             bitmapSize);
                        });
                        DestroyIcon(icon);
                        if (bitmap)
                            return bitmap;
                    }
                    else if (icon)
                    {
                        DestroyIcon(icon);
                    }
                }
            }
        }
    }
    ComPtr<IShellItemImageFactory> imageFactory;
    if (SUCCEEDED(shellCalls::Call(L"ImageFactory.SHCreateItemFromIDList",
                                   [&] {
                                       return SHCreateItemFromIDList(pidl,
                                                                     IID_PPV_ARGS(&imageFactory));
                                   })) &&
        imageFactory)
    {
        const auto tryImageFactory = [&](UINT flags, bool thumbnail) -> HBITMAP
        {
            SIZE size{ sourceSize, sourceSize };
            HBITMAP bitmap = nullptr;
            if (FAILED(shellCalls::Call(thumbnail ? L"ImageFactory.GetImage.Thumbnail"
                                                  : L"ImageFactory.GetImage.Icon",
                                        [&] {
                                            return imageFactory->GetImage(size, flags, &bitmap);
                                        })) ||
                bitmap == nullptr)
                return nullptr;

            HBITMAP alphaBitmap = shellCalls::Call(L"ImageFactory.CopyAlphaBitmap", [&] {
                return CopyBitmapToAlphaDib(bitmap, bitmapSize);
            });
            DeleteObject(bitmap);
            if (alphaBitmap && returnedThumbnail)
                *returnedThumbnail = thumbnail;
            return alphaBitmap;
        };

        if (allowThumbnail)
        {
            if (HBITMAP thumbnail = tryImageFactory(
                    SIIGBF_THUMBNAILONLY, true))
                return thumbnail;
        }
        if (HBITMAP icon = tryImageFactory(SIIGBF_ICONONLY, false))
            return icon;
    }

    ComPtr<IImageList> imageList;
    HRESULT hr = shellCalls::Call(L"ImageList.SHGetImageList.Jumbo", [&] {
        return SHGetImageList(SHIL_JUMBO, __uuidof(IImageList),
                              reinterpret_cast<void**>(imageList.GetAddressOf()));
    });
    if (FAILED(hr) || !imageList)
    {
        imageList.Reset();
        hr = shellCalls::Call(L"ImageList.SHGetImageList.ExtraLarge", [&] {
            return SHGetImageList(SHIL_EXTRALARGE, __uuidof(IImageList),
                                  reinterpret_cast<void**>(imageList.GetAddressOf()));
        });
    }
    if (FAILED(hr) || !imageList)
    {
        imageList.Reset();
        hr = shellCalls::Call(L"ImageList.SHGetImageList.Large", [&] {
            return SHGetImageList(SHIL_LARGE, __uuidof(IImageList),
                                  reinterpret_cast<void**>(imageList.GetAddressOf()));
        });
    }

    HICON icon = nullptr;
    if (SUCCEEDED(hr) && imageList && fallbackIndex >= 0)
    {
        shellCalls::Call(L"ImageList.GetIcon", [&] {
            return imageList->GetIcon(fallbackIndex, ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon);
        });
        if (icon != nullptr)
        {
            HBITMAP bitmap = shellCalls::Call(L"Bitmap.CreateAlphaBitmap", [&] {
                return CreateAlphaBitmapFromIcon(icon, sourceSize, sourceSize, bitmapSize);
            });
            DestroyIcon(icon);
            if (bitmap != nullptr)
            {
                return bitmap;
            }
        }
    }

    SHFILEINFOW iconInfo{};
    shellCalls::Call(L"Fallback.SHGetFileInfo.Icon", [&] {
        return SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &iconInfo, sizeof(iconInfo),
                              SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS);
    });
    icon = iconInfo.hIcon;
    if (icon != nullptr)
    {
        HBITMAP bitmap = shellCalls::Call(L"Bitmap.CreateAlphaBitmap", [&] {
            return CreateAlphaBitmapFromIcon(icon, sourceSize, sourceSize, bitmapSize);
        });
        DestroyIcon(icon);
        return bitmap;
    }

    return nullptr;
}
