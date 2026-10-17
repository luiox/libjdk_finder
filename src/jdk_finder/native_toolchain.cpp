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
    fs::path tools   = value;
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

NativeToolchain find_native_compiler(CCompilerKind requested)
{
    NativeToolchain tc;

    if (requested != CCompilerKind::AUTO) {
        tc.compiler = requested;
        if (requested == CCompilerKind::MSVC) {
            bool has_cl              = command_exists("cl");
            bool has_configured_msvc = has_cl && !detail::env_value("INCLUDE").empty();
            fs::path vcvars          = find_vcvars_all();
            if (has_configured_msvc || !vcvars.empty()) {
                tc.vcvars_all = has_configured_msvc ? fs::path{} : vcvars;
                tc.available  = true;
                return tc;
            }
            tc.reason = has_cl ? "MSVC 环境未配置且未找到 vcvarsall.bat"
                              : "未找到 MSVC（cl 不在 PATH 且 vcvarsall.bat 未定位到）";
            return tc;
        }
        if (command_exists(compiler_executable_name(requested))) {
            tc.available = true;
            return tc;
        }
        tc.reason = std::string("未在 PATH 找到 ") + compiler_executable_name(requested);
        return tc;
    }

#ifdef _WIN32
    bool has_cl              = command_exists("cl");
    bool has_configured_msvc = has_cl && !detail::env_value("INCLUDE").empty();
    fs::path vcvars          = has_configured_msvc ? fs::path{} : find_vcvars_all();
    bool has_vcvars_msvc     = !vcvars.empty();
    if (has_configured_msvc || has_vcvars_msvc) {
        tc.compiler   = CCompilerKind::MSVC;
        tc.vcvars_all = vcvars;
        tc.available  = true;
        return tc;
    }
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
