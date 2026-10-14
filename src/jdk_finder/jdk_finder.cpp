//jdk_finder.cpp — JDK 与原生工具链探测实现

#include "jdk_finder/jdk_finder.hpp"

#include <cstdlib>
#include <system_error>

namespace jdk_finder {
namespace {

///跨平台读环境变量（Windows 用 _dupenv_s 避免CRT警告，POSIX 用 getenv）。
std::string envValue(const char* name)
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

bool fileExists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

fs::path platformIncludeSubdir()
{
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

int runQuiet(const std::string& command)
{
    return std::system(command.c_str());
}

///枚举 <root> 下「版本/发行版」两层子目录中的 vcvarsall.bat（清单兜底）。
/// VS 版本目录名随发布演进（2017/2019/2022/…/18），静态年份清单会滞后，
/// 对目录名做开放枚举使其对未来 VS 版本免疫；命中即返回（不判新旧）。
fs::path vcvarsByEnumerate(const fs::path& root)
{
    std::error_code ec;
    fs::directory_iterator it(root, ec), end;
    if (ec) return {};
    while (it != end) {
        std::error_code subEc;
        if (it->is_directory(subEc)) {
            fs::directory_iterator ed(it->path(), subEc), edEnd;
            while (!subEc && ed != edEnd) {
                const fs::path candidate =
                    ed->path() / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
                if (fileExists(candidate)) return candidate;
                std::error_code edEc;
                ed.increment(edEc);
                if (edEc) break;
            }
        }
        std::error_code incEc;
        it.increment(incEc);
        if (incEc) break;
    }
    return {};
}

}   //anonymous namespace

bool commandExists(const std::string& command)
{
#ifdef _WIN32
    return runQuiet("where " + command + " >nul 2>nul") == 0;
#else
    return runQuiet("command -v " + command + " >/dev/null 2>&1") == 0;
#endif
}

JdkLocation findJdk(const FindOptions& opts)
{
    JdkLocation loc;
    std::string lastRejectedReason;

    std::vector<fs::path> candidates;
    if (auto home = envValue("JAVA_HOME"); !home.empty()) candidates.emplace_back(home);
    if (auto home = envValue("MJT_JDK_HOME"); !home.empty()) candidates.emplace_back(home);
    for (const auto& fb : opts.fallbackHomes) candidates.emplace_back(fb);

    //合法 JDK 的标志：bin/javac(.exe) 存在。
    auto pickExe = [](const fs::path& bin, const std::string& name) -> fs::path {
        fs::path withExe = bin / (name + ".exe");
        if (fileExists(withExe)) return withExe;
        fs::path plain = bin / name;
        if (fileExists(plain)) return plain;
        return {};
    };

    for (const auto& home : candidates) {
        if (pickExe(home / "bin", "javac").empty()) continue;
        JdkLocation candidate;
        candidate.javaHome       = home;
        candidate.jdkInclude     = home / "include";
        candidate.jdkIncludePlat = candidate.jdkInclude / platformIncludeSubdir();
        if (opts.requireJniHeader && !fileExists(candidate.jdkInclude / "jni.h")) {
            lastRejectedReason =
                "requireJniHeader=true 但 " + (candidate.jdkInclude / "jni.h").string() +
                " 不存在";
            continue;
        }
        candidate.available = true;
        return candidate;
    }

    if (!lastRejectedReason.empty()) {
        loc.reason = "未找到满足要求的 JDK：" + lastRejectedReason;
        return loc;
    }
    loc.reason = "未找到可用 JDK：JAVA_HOME/MJT_JDK_HOME";
    if (!opts.fallbackHomes.empty()) loc.reason += "/fallbackHomes";
    loc.reason += " 均未命中 bin/javac";
    return loc;
}

fs::path jdkExecutable(const JdkLocation& jdk, const std::string& name)
{
    if (!jdk.available) return {};
    fs::path bin = jdk.javaHome / "bin";
    fs::path withExe = bin / (name + ".exe");
    if (fileExists(withExe)) return withExe;
    fs::path plain = bin / name;
    if (fileExists(plain)) return plain;
    return {};
}

namespace {

fs::path vcvarsAllFromVCTools(const std::string& value)
{
    if (value.empty()) return {};
    fs::path tools  = value;
    fs::path vcRoot = tools.parent_path().parent_path().parent_path();
    fs::path vcvars = vcRoot / "Auxiliary" / "Build" / "vcvarsall.bat";
    return fileExists(vcvars) ? vcvars : fs::path{};
}

const char* compilerExecutableName(CCompilerKind kind)
{
    switch (kind) {
    case CCompilerKind::Msvc:  return "cl";
    case CCompilerKind::Gcc:   return "gcc";
    case CCompilerKind::Clang: return "clang";
    case CCompilerKind::Zig:   return "zig";
    case CCompilerKind::Auto:  break;
    }
    return "";
}

}   //anonymous namespace

fs::path findVcvarsAll()
{
#ifdef _WIN32
    if (auto fromEnv = vcvarsAllFromVCTools(envValue("VCToolsInstallDir")); !fromEnv.empty())
        return fromEnv;

    std::vector<fs::path> roots;
    if (auto pf = envValue("ProgramFiles"); !pf.empty()) roots.emplace_back(pf);
    if (auto pfx86 = envValue("ProgramFiles(x86)"); !pfx86.empty()) roots.emplace_back(pfx86);

    const char* years[]    = {"2022", "2019", "2017"};
    const char* editions[] = {"Community", "Professional", "Enterprise", "BuildTools"};
    for (const auto& root : roots) {
        for (const char* year : years) {
            for (const char* edition : editions) {
                fs::path vcvars = root / "Microsoft Visual Studio" / year / edition / "VC" /
                                  "Auxiliary" / "Build" / "vcvarsall.bat";
                if (fileExists(vcvars)) return vcvars;
            }
        }
        // 静态清单未命中（如 VS 2026 起版本目录名不在清单内）→ 枚举兜底。
        if (auto scanned = vcvarsByEnumerate(root / "Microsoft Visual Studio");
            !scanned.empty())
            return scanned;
    }
    return {};
#else
    return {};
#endif
}

NativeToolchain findNativeCompiler(CCompilerKind requested)
{
    NativeToolchain tc;

    if (requested != CCompilerKind::Auto) {
        tc.compiler = requested;
        if (requested == CCompilerKind::Msvc) {
            bool hasCl             = commandExists("cl");
            bool hasConfiguredMsvc = hasCl && !envValue("INCLUDE").empty();
            fs::path vcvars         = findVcvarsAll();
            if (hasConfiguredMsvc || !vcvars.empty()) {
                tc.vcvarsAll = hasConfiguredMsvc ? fs::path{} : vcvars;
                tc.available = true;
                return tc;
            }
            tc.reason = hasCl ? "MSVC 环境未配置且未找到 vcvarsall.bat"
                              : "未找到 MSVC（cl 不在 PATH 且 vcvarsall.bat 未定位到）";
            return tc;
        }
        if (commandExists(compilerExecutableName(requested))) {
            tc.available = true;
            return tc;
        }
        tc.reason = std::string("未在 PATH 找到 ") + compilerExecutableName(requested);
        return tc;
    }

#ifdef _WIN32
    bool hasCl             = commandExists("cl");
    bool hasConfiguredMsvc = hasCl && !envValue("INCLUDE").empty();
    fs::path vcvars         = hasConfiguredMsvc ? fs::path{} : findVcvarsAll();
    bool hasVcvarsMsvc     = !vcvars.empty();
    if (hasConfiguredMsvc || hasVcvarsMsvc) {
        tc.compiler  = CCompilerKind::Msvc;
        tc.vcvarsAll = vcvars;
        tc.available = true;
        return tc;
    }
    if (commandExists("clang")) {
        tc.compiler  = CCompilerKind::Clang;
        tc.available = true;
        return tc;
    }
    if (commandExists("gcc")) {
        tc.compiler  = CCompilerKind::Gcc;
        tc.available = true;
        return tc;
    }
    if (commandExists("zig")) {
        tc.compiler  = CCompilerKind::Zig;
        tc.available = true;
        return tc;
    }
#elif defined(__APPLE__)
    if (commandExists("clang")) {
        tc.compiler  = CCompilerKind::Clang;
        tc.available = true;
        return tc;
    }
    if (commandExists("gcc")) {
        tc.compiler  = CCompilerKind::Gcc;
        tc.available = true;
        return tc;
    }
    if (commandExists("zig")) {
        tc.compiler  = CCompilerKind::Zig;
        tc.available = true;
        return tc;
    }
#else
    if (commandExists("gcc")) {
        tc.compiler  = CCompilerKind::Gcc;
        tc.available = true;
        return tc;
    }
    if (commandExists("clang")) {
        tc.compiler  = CCompilerKind::Clang;
        tc.available = true;
        return tc;
    }
    if (commandExists("zig")) {
        tc.compiler  = CCompilerKind::Zig;
        tc.available = true;
        return tc;
    }
#endif

    tc.reason = "未探测到可用的 C 编译器（MSVC/gcc/clang/Zig）";
    return tc;
}

}   //namespace jdk_finder
