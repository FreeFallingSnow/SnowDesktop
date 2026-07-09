/**
 * @file data_paths.h
 * @brief SnowDesktop 用户数据路径与旧版路径迁移。
 */

#pragma once

#include <string>

/**
 * @brief 获取当前可执行文件所在目录。
 * @return 可执行文件目录的绝对路径，失败时返回当前目录 "."。
 */
std::wstring GetExecutableDirectoryPath();

/**
 * @brief 获取 SnowDesktop 用户数据目录。
 * @details 数据目录位于可执行文件目录下的 data 子目录；调用时会尽量创建目录。
 * @return data 目录的绝对路径。
 */
std::wstring GetDataDirectoryPath();

/**
 * @brief 获取 data 目录下的文件路径，并迁移旧版同名文件。
 * @details 若旧版文件存在于 exe 同目录且 data 中尚无对应文件，会自动移动到 data。
 * @param filename 文件名，不包含目录。
 * @return data 下的目标文件路径。
 */
std::wstring GetDataFilePath(const wchar_t* filename);

/**
 * @brief 获取 data 目录下的子目录路径，并迁移旧版同名目录。
 * @details 若旧版目录存在于 exe 同目录且 data 中尚无对应目录，会自动移动到 data。
 * @param dirname 子目录名，不包含父目录。
 * @return data 下的目标目录路径。
 */
std::wstring GetDataSubdirectoryPath(const wchar_t* dirname);

/**
 * @brief 启动时迁移所有已知旧版用户数据路径。
 * @details 将 exe 同目录下的旧版 JSON 数据文件和 backups 目录迁移到 data 下。
 */
void MigrateLegacyDataPaths();
