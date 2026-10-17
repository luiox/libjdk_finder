//jdk_finder.cpp — JDK 与原生工具链探测实现

#include "jdk_finder/jdk_finder.hpp"

#include <cstdlib>
#include <system_error>

namespace jdk_finder {
namespace {

///跨平台读环境变量（Windows 用 _dupenv_s 避免CRT警告，POSIX 用 getenv）。
std::string env_value(const char* name)
{
#ifdef _WIN32
    char*  value = nullptr;
    size_t size  = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

bool file_exists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

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

int run_quiet(const std::string& command)
{
    return std::system(command.c_str());
}

///枚举 <root> 下「版本/发行版」两层子目录中的 vcvarsall.bat（清单兜底）。
/// VS 版本目录名随发布演进（2017/2019/2022/…/18），静态年份清单会滞后，
/// 对目录名做开放枚举使其对未来 VS 版本免疫；命中即返回（不判新旧）。
fs::path vcvars_by_enumerate(const fs::path& root)
{
    std::error_code ec;
    fs::directory_iterator it(root, ec), end;
    if (ec) return {};
    while (it != end) {
        std::error_code sub_ec;
        if (it->is_directory(sub_ec)) {
            fs::directory_iterator ed(it->path(), sub_ec), ed_end;
            while (!sub_ec && ed != ed_end) {
                const fs::path candidate =
                    ed->path() / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
                if (file_exists(candidate)) return candidate;
                std::error_code ed_ec;
                ed.increment(ed_ec);
                if (ed_ec) break;
            }
        }
        std::error_code inc_ec;
        it.increment(inc_ec);
        if (inc_ec) break;
    }
    return {};
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

JdkLocation find_jdk(const FindOptions& opts)
{
    JdkLocation loc;
    std::string last_rejected_reason;

    std::vector<fs::path> candidates;
    if (auto home = env_value("JAVA_HOME"); !home.empty()) candidates.emplace_back(home);
    if (auto home = env_value("MJT_JDK_HOME"); !home.empty()) candidates.emplace_back(home);
    for (const auto& fb : opts.fallback_homes) candidates.emplace_back(fb);

    //合法 JDK 的标志：bin/javac(.exe) 存在。
    auto pick_exe = [](const fs::path& bin, const std::string& name) -> fs::path {
        fs::path with_exe = bin / (name + ".exe");
        if (file_exists(with_exe)) return with_exe;
        fs::path plain = bin / name;
        if (file_exists(plain)) return plain;
        return {};
    };

    for (const auto& home : candidates) {
        if (pick_exe(home / "bin", "javac").empty()) continue;
        JdkLocation candidate;
        candidate.java_home       = home;
        candidate.jdk_include     = home / "include";
        candidate.jdk_include_plat = candidate.jdk_include / platform_include_subdir();
        if (opts.require_jni_header && !file_exists(candidate.jdk_include / "jni.h")) {
            last_rejected_reason =
                "require_jni_header=true 但 " + (candidate.jdk_include / "jni.h").u8string() +
                " 不存在";
            continue;
        }
        candidate.available = true;
        return candidate;
    }

    if (!last_rejected_reason.empty()) {
        loc.reason = "未找到满足要求的 JDK：" + last_rejected_reason;
        return loc;
    }
    loc.reason = "未找到可用 JDK：JAVA_HOME/MJT_JDK_HOME";
    if (!opts.fallback_homes.empty()) loc.reason += "/fallback_homes";
    loc.reason += " 均未命中 bin/javac";
    return loc;
}

fs::path jdk_executable(const JdkLocation& jdk, const std::string& name)
{
    if (!jdk.available) return {};
    fs::path bin = jdk.java_home / "bin";
    fs::path with_exe = bin / (name + ".exe");
    if (file_exists(with_exe)) return with_exe;
    fs::path plain = bin / name;
    if (file_exists(plain)) return plain;
    return {};
}

namespace {

fs::path vcvars_all_from_vctools(const std::string& value)
{
    if (value.empty()) return {};
    fs::path tools  = value;
    fs::path vc_root = tools.parent_path().parent_path().parent_path();
    fs::path vcvars = vc_root / "Auxiliary" / "Build" / "vcvarsall.bat";
    return file_exists(vcvars) ? vcvars : fs::path{};
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

fs::path find_vcvars_all()
{
#ifdef _WIN32
    if (auto from_env = vcvars_all_from_vctools(env_value("VCToolsInstallDir")); !from_env.empty())
        return from_env;

    std::vector<fs::path> roots;
    if (auto pf = env_value("ProgramFiles"); !pf.empty()) roots.emplace_back(pf);
    if (auto pfx86 = env_value("ProgramFiles(x86)"); !pfx86.empty()) roots.emplace_back(pfx86);

    const char* years[]    = {"2022", "2019", "2017"};
    const char* editions[] = {"Community", "Professional", "Enterprise", "BuildTools"};
    for (const auto& root : roots) {
        for (const char* year : years) {
            for (const char* edition : editions) {
                fs::path vcvars = root / "Microsoft Visual Studio" / year / edition / "VC" /
                                  "Auxiliary" / "Build" / "vcvarsall.bat";
                if (file_exists(vcvars)) return vcvars;
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
            bool has_cl            = command_exists("cl");
            bool has_configured_msvc = has_cl && !env_value("INCLUDE").empty();
            fs::path vcvars         = find_vcvars_all();
            if (has_configured_msvc || !vcvars.empty()) {
                tc.vcvars_all = has_configured_msvc ? fs::path{} : vcvars;
                tc.available = true;
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
    bool has_cl            = command_exists("cl");
    bool has_configured_msvc = has_cl && !env_value("INCLUDE").empty();
    fs::path vcvars         = has_configured_msvc ? fs::path{} : find_vcvars_all();
    bool has_vcvars_msvc    = !vcvars.empty();
    if (has_configured_msvc || has_vcvars_msvc) {
        tc.compiler  = CCompilerKind::MSVC;
        tc.vcvars_all = vcvars;
        tc.available = true;
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
