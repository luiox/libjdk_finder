//jdk_finder.cpp — JDK 定位、默认安装位置扫描与 java 信息查询

#include "jdk_finder/jdk_finder.hpp"
#include "jdk_finder/detail.hpp"

#include <cctype>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "libca/fs/file_util.hpp"

#ifdef _WIN32
#    include <windows.h>
#endif

namespace jdk_finder {

namespace fs = std::filesystem;

namespace {

fs::path platform_include_subdir()
{
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

///用户主目录。Windows 取 USERPROFILE，其余平台取 HOME。空 path 表示均未设置。
fs::path user_home()
{
#ifdef _WIN32
    fs::path profile = detail::env_path("USERPROFILE");
    if (!profile.empty()) return profile;
#endif
    return detail::env_path("HOME");
}

///~/.jdk 本身与其一级子目录：前者对应单装（bin/javac 直接在根下），
///后者对应多 JDK 目录（每个子目录是一套安装）。候选以 bin/javac(.exe) 收敛，
///与 find_jdk 的合法性校验同一标准，噪声目录（Program Files 全量枚举产物）
///不再透传。
void append_user_jdk_homes(std::vector<fs::path>& out)
{
    fs::path base = user_home();
    if (base.empty()) return;
    base /= ".jdk";
    if (!detail::dir_exists(base)) return;
    if (!detail::pick_executable(base / "bin", "javac").empty()) out.push_back(base);

    for (const auto& child : detail::child_directories(base)) {
        if (!detail::pick_executable(child / "bin", "javac").empty())
            out.push_back(child);
    }
}

#ifdef _WIN32

///逻辑盘符根列表。GetLogicalDriveStrings 精确返回存在的盘符，
///避免逐字母盲探不存在的驱动器；盘符恒为 ASCII，无编码问题。
std::vector<fs::path> logical_drives()
{
    std::vector<fs::path> drives;
    char buffer[512];
    const UINT count = ::GetLogicalDriveStringsA(sizeof(buffer) - 1, buffer);
    if (count == 0 || count >= sizeof(buffer)) return drives;
    for (const char* p = buffer; *p != '\0'; p += std::strlen(p) + 1)
        drives.emplace_back(p);
    return drives;
}

///各盘 Program Files / Program Files (x86) 下的一级与二级子目录。
///发行商目录名随发布演进（Java/Eclipse Adoptium/Microsoft/…），与 VS 版本目录
///同一理由不维护静态清单；两层深度覆盖「Program Files/<厂商>/<jdk>」与
///「Program Files/<jdk>」两种形态。候选以 bin/javac(.exe) 收敛：全量枚举产物
///里的非 JDK 目录（7-Zip/CMake 等）不透传，find_jdk 侧校验保留（幂等）。
void append_program_files_homes(std::vector<fs::path>& out)
{
    const char* names[] = {"Program Files", "Program Files (x86)"};
    for (const fs::path& drive : logical_drives()) {
        for (const char* name : names) {
            const fs::path root = drive / name;
            if (!detail::dir_exists(root)) continue;
            for (const auto& first : detail::child_directories(root)) {
                if (detail::pick_executable(first / "bin", "javac").empty()) continue;
                out.push_back(first);
                for (const auto& second : detail::child_directories(first)) {
                    if (detail::pick_executable(second / "bin", "javac").empty()) continue;
                    out.push_back(second);
                }
            }
        }
    }
}

#endif   //_WIN32

std::string trim_copy(std::string s)
{
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    size_t begin = 0;
    while (begin < s.size() && is_space(s[begin])) ++begin;
    size_t end = s.size();
    while (end > begin && is_space(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

///release 行 KEY="value" 解析填入 JavaInfo；值按 UTF-8 字节透传不转码。
void apply_release_line(const std::string& line, JavaInfo& info)
{
    const auto eq = line.find('=');
    if (eq == std::string::npos) return;
    std::string key   = trim_copy(line.substr(0, eq));
    std::string value = trim_copy(line.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    if      (key == "JAVA_VERSION")         info.java_version = std::move(value);
    else if (key == "IMPLEMENTOR")          info.vendor = std::move(value);
    else if (key == "IMPLEMENTOR_VERSION")  info.vendor_version = std::move(value);
    else if (key == "JAVA_RUNTIME_VERSION") info.runtime_version = std::move(value);
    else if (key == "OS_NAME")              info.os_name = std::move(value);
    else if (key == "OS_ARCH")              info.os_arch = std::move(value);
}

}   //anonymous namespace

std::vector<fs::path> default_jdk_homes()
{
    std::vector<fs::path> homes;
    append_user_jdk_homes(homes);
#ifdef _WIN32
    append_program_files_homes(homes);
#endif
    return homes;
}

JdkLocation find_jdk(const FindOptions& opts)
{
    std::vector<fs::path> candidates;
    if (auto home = detail::env_path("JAVA_HOME"); !home.empty()) candidates.push_back(home);
    if (auto home = detail::env_path("MJT_JDK_HOME"); !home.empty())
        candidates.push_back(home);
    candidates.insert(candidates.end(), opts.fallback_homes.begin(), opts.fallback_homes.end());
    if (opts.search_default_homes) {
        std::vector<fs::path> scanned = default_jdk_homes();
        candidates.insert(candidates.end(), scanned.begin(), scanned.end());
    }

    std::string last_rejected_reason;
    for (const auto& home : candidates) {
        //合法 JDK 的标志：bin/javac(.exe) 存在。
        if (detail::pick_executable(home / "bin", "javac").empty()) continue;
        JdkLocation candidate;
        candidate.java_home        = home;
        candidate.jdk_include      = home / "include";
        candidate.jdk_include_plat = candidate.jdk_include / platform_include_subdir();
        if (opts.require_jni_header && !detail::file_exists(candidate.jdk_include / "jni.h")) {
            last_rejected_reason = "require_jni_header=true 但 " +
                                   (candidate.jdk_include / "jni.h").u8string() + " 不存在";
            continue;
        }
        candidate.available = true;
        return candidate;
    }

    JdkLocation loc;
    if (!last_rejected_reason.empty()) {
        loc.reason = "未找到满足要求的 JDK：" + last_rejected_reason;
        return loc;
    }
    loc.reason = "未找到可用 JDK：JAVA_HOME/MJT_JDK_HOME";
    if (!opts.fallback_homes.empty()) loc.reason += "/fallback_homes";
    loc.reason += opts.search_default_homes ? "/默认安装位置" : "";
    loc.reason += " 均未命中 bin/javac";
    return loc;
}

fs::path jdk_executable(const JdkLocation& jdk, const std::string& name)
{
    if (!jdk.available) return {};
    return detail::pick_executable(jdk.java_home / "bin", name);
}

ca::StatusResult<JavaInfo> java_info(const fs::path& jdk_home)
{
    if (jdk_home.empty())
        return ca::Err(ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                     "java_info：jdk_home 为空"));

    const ca::fs::Path home(jdk_home);
    const ca::fs::Path release = home / ca::fs::Path::from_utf8_lossy("release");
    if (!ca::fs::FileUtil::is_file(release)) {
        return ca::Err(ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                     "未找到 " + release.to_utf8_lossy() +
                                         "（JDK 9+ 与主流发行版均有该元数据文件，老 JDK 8 套件可能缺失）"));
    }

    //strip_bom：个别 JDK 的 release 文件带 UTF-8 BOM，先剥离再逐行解析。
    if (auto content = ca::fs::FileUtil::read_all_text(release, /*strip_bom=*/true);
        content.is_err()) {
        return ca::Err(ca::ErrStatus(
            ca::StatusCode::INTERNAL,
            "读取 release 失败：" + release.to_utf8_lossy() + "：" +
                ca::fs::to_string(content.unwrap_err())));
    }
    else {
        JavaInfo info;
        std::istringstream body(content.unwrap());
        std::string line;
        while (std::getline(body, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            apply_release_line(line, info);
        }
        //JAVA_VERSION 是版本信息的核心字段，缺失视为元数据损坏，不做部分成功。
        if (info.java_version.empty())
            return ca::Err(ca::ErrStatus(
                ca::StatusCode::INTERNAL,
                "release 文件未含 JAVA_VERSION：" + release.to_utf8_lossy()));
        return ca::Ok(std::move(info));
    }
}

ca::StatusResult<JavaInfo> java_info(const JdkLocation& jdk)
{
    if (!jdk.available)
        return ca::Err(ca::ErrStatus(
            ca::StatusCode::INVALID_ARGUMENT,
            "java_info：JdkLocation 未定位到 JDK（" + jdk.reason + "）"));
    return java_info(jdk.java_home);
}

}   //namespace jdk_finder
