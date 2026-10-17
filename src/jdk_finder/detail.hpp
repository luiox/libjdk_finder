//detail.hpp — jdk_finder 内部共享助手。
//不列入 add_headerfiles，不随公开头发布。

#pragma once

#include "jdk_finder/jdk_finder.hpp"

#include "libca/env/env.hpp"
#include "libca/fs/file_util.hpp"

namespace jdk_finder::detail {

///存在性判断经 libca FileUtil，与族仓统一错误分类保持一致。
inline bool file_exists(const fs::path& p)
{
    return ca::fs::FileUtil::exists(ca::fs::Path(p));
}

inline bool dir_exists(const fs::path& p)
{
    return ca::fs::FileUtil::is_directory(ca::fs::Path(p));
}

///双探测可执行文件：先 <bin>/<name>.exe 再 <bin>/<name>。找不到返回空 path。
inline fs::path pick_executable(const fs::path& bin, const std::string& name)
{
    fs::path with_exe = bin / (name + ".exe");
    if (file_exists(with_exe)) return with_exe;
    fs::path plain = bin / name;
    if (file_exists(plain)) return plain;
    return {};
}

///环境变量读取。ca::env::get 保证含非 ASCII 的值按 UTF-8 正确往返。
inline std::string env_value(const char* name)
{
    if (auto value = ca::env::get(name)) return *value;
    return {};
}

///环境变量派生路径的编码边界：ca::env 给 UTF-8 字节，直接 fs::path(std::string)
///在 Windows 上会按本地代码页解字节，CJK 主目录必乱；经 ca::fs::Path 完成
///UTF-8 ↔ 平台原生编码转换（Windows UTF-16 / POSIX 字节），不依赖 fs::u8path
///（std 标准库这项在 C++20 已废弃，Windows 窄转宽有代码页歧义）。
inline fs::path env_path(const char* name)
{
    ca::fs::Path path = ca::fs::Path::from_utf8_lossy(env_value(name));
    return path.native();
}

///目录一级子目录枚举。list_entries 走 libca fs，免手写错误码推进循环；
///枚举失败（权限等）按「无子目录」处理：扫描列表为空不算错误。
inline std::vector<fs::path> child_directories(const fs::path& dir)
{
    std::vector<fs::path> children;
    if (auto listed = ca::fs::FileUtil::list_entries(ca::fs::Path(dir)); listed.is_ok()) {
        for (const ca::fs::Path& entry : listed.unwrap()) {
            fs::path entry_path = entry.native();
            if (dir_exists(entry_path)) children.push_back(std::move(entry_path));
        }
    }
    return children;
}

}   //namespace jdk_finder::detail
