//native_toolchain.cpp — 原生 C 编译器探测与 MSVC vcvarsall 定位

#include "jdk_finder/jdk_finder.hpp"
#include "jdk_finder/detail.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "libca/fs/file_util.hpp"

namespace jdk_finder {

namespace fs = std::filesystem;

namespace {

int run_quiet(const std::string& command)
{
    return std::system(command.c_str());
}

///枚举 <root> 下「版本/发行版」两级子目录，找其下 vcvarsall.bat（清单兜底）。
/// VS 版本目录名随发布演进（2017/2019/2022/…/18），静态年份清单会滞后，
/// 对目录名做开放枚举使其对未来 VS 版本免疫；命中即返回（不判新旧）。
fs::path vcvars_by_enumerate(const fs::path& root)
{
    for (const auto& version_dir : detail::child_directories(root)) {
        for (const auto& edition_dir : detail::child_directories(version_dir)) {
            const fs::path candidate = edition_dir / "VC" / "Auxiliary" / "Build" /
                                       "vcvarsall.bat";
            if (detail::file_exists(candidate)) return candidate;
        }
    }
    return {};
}

fs::path vcvars_all_from_vctools(const std::string& value)
{
    if (value.empty()) return {};
    //ca::env::get 产出 UTF-8 字节，fs::path(std::string) 在 Windows 按本地代码页
    //解字节（CJK 安装路径必乱），与 detail::env_path 同一纪律经 ca::fs::Path 转码。
    fs::path tools   = ca::fs::Path::from_utf8_lossy(value).native();
    fs::path vc_root = tools.parent_path().parent_path().parent_path();
    fs::path vcvars  = vc_root / "Auxiliary" / "Build" / "vcvarsall.bat";
    return detail::file_exists(vcvars) ? vcvars : fs::path{};
}

const char* compiler_executable_name(CCompilerKind kind)
{
    switch (kind) {
    case CCompilerKind::MSVC:  return "cl";
    case CCompilerKind::GCC:   return "gcc";
    case CCompilerKind::CLANG: return "clang";
    case CCompilerKind::ZIG:   return "zig";
    case CCompilerKind::AUTO:  break;
    }
    return "";
}

}   //anonymous namespace

bool command_exists(const std::string& command)
{
#ifdef _WIN32
    return run_quiet("where " + command + " >nul 2>nul") == 0;
#else
    return run_quiet("command -v " + command + " >/dev/null 2>&1") == 0;
#endif
}

fs::path find_vcvars_all()
{
#ifdef _WIN32
    if (auto from_env = vcvars_all_from_vctools(detail::env_value("VCToolsInstallDir"));
        !from_env.empty())
        return from_env;

    std::vector<fs::path> roots;
    if (auto pf = detail::env_path("ProgramFiles"); !pf.empty()) roots.push_back(pf);
    if (auto pfx86 = detail::env_path("ProgramFiles(x86)"); !pfx86.empty())
        roots.push_back(pfx86);

    const char* years[]    = {"2022", "2019", "2017"};
    const char* editions[] = {"Community", "Professional", "Enterprise", "BuildTools"};
    for (const auto& root : roots) {
        for (const char* year : years) {
            for (const char* edition : editions) {
                fs::path vcvars = root / "Microsoft Visual Studio" / year / edition / "VC" /
                                  "Auxiliary" / "Build" / "vcvarsall.bat";
                if (detail::file_exists(vcvars)) return vcvars;
            }
        }
        //静态清单未命中（如 VS 2026 起版本目录名不在清单内）→ 枚举兜底。
        if (auto scanned = vcvars_by_enumerate(root / "Microsoft Visual Studio");
            !scanned.empty())
            return scanned;
    }
    return {};
#else
    return {};
#endif
}

namespace {

///MSVC 探测（requested=MSVC 与 AUTO 共用）：cl 在 PATH 且 INCLUDE 已配置视为
///环境就绪（vcvars_all 留空）；否则定位 vcvarsall.bat 补足。不可用时
///available=false 且 reason 填说明。
NativeToolchain probe_msvc()
{
    NativeToolchain tc;
    tc.compiler              = CCompilerKind::MSVC;
    const bool has_cl        = command_exists("cl");
    const bool has_configure = has_cl && !detail::env_value("INCLUDE").empty();
    if (has_configure) {
        tc.available = true;
        return tc;
    }
    fs::path vcvars = find_vcvars_all();
    if (!vcvars.empty()) {
        tc.vcvars_all = vcvars;
        tc.available  = true;
        return tc;
    }
    tc.reason = has_cl ? "MSVC 环境未配置且未找到 vcvarsall.bat"
                       : "未找到 MSVC（cl 不在 PATH 且 vcvarsall.bat 未定位到）";
    return tc;
}

}   //anonymous namespace

NativeToolchain find_native_compiler(CCompilerKind requested)
{
    NativeToolchain tc;

    if (requested != CCompilerKind::AUTO) {
        if (requested == CCompilerKind::MSVC) return probe_msvc();
        tc.compiler = requested;
        if (command_exists(compiler_executable_name(requested))) {
            tc.available = true;
            return tc;
        }
        tc.reason = std::string("未在 PATH 找到 ") + compiler_executable_name(requested);
        return tc;
    }

#ifdef _WIN32
    if (NativeToolchain msvc = probe_msvc(); msvc.available) return msvc;
    if (command_exists("clang")) {
        tc.compiler  = CCompilerKind::CLANG;
        tc.available = true;
        return tc;
    }
    if (command_exists("gcc")) {
        tc.compiler  = CCompilerKind::GCC;
        tc.available = true;
        return tc;
    }
    if (command_exists("zig")) {
        tc.compiler  = CCompilerKind::ZIG;
        tc.available = true;
        return tc;
    }
#elif defined(__APPLE__)
    if (command_exists("clang")) {
        tc.compiler  = CCompilerKind::CLANG;
        tc.available = true;
        return tc;
    }
    if (command_exists("gcc")) {
        tc.compiler  = CCompilerKind::GCC;
        tc.available = true;
        return tc;
    }
    if (command_exists("zig")) {
        tc.compiler  = CCompilerKind::ZIG;
        tc.available = true;
        return tc;
    }
#else
    if (command_exists("gcc")) {
        tc.compiler  = CCompilerKind::GCC;
        tc.available = true;
        return tc;
    }
    if (command_exists("clang")) {
        tc.compiler  = CCompilerKind::CLANG;
        tc.available = true;
        return tc;
    }
    if (command_exists("zig")) {
        tc.compiler  = CCompilerKind::ZIG;
        tc.available = true;
        return tc;
    }
#endif

    tc.reason = "未探测到可用的 C 编译器（MSVC/gcc/clang/Zig）";
    return tc;
}

}   //namespace jdk_finder
