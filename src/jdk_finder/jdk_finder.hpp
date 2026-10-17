//jdk_finder.hpp — JDK 与原生工具链探测
//
//服务于需要定位 JDK（JNI 头）和 C 编译器（MSVC/gcc/clang/Zig）的产品功能与测试。
//当前消费方：
//- 测试侧（javac/java 编译运行样本）
//
//零外部依赖（仅 std + libca），可被任意产品库和 test_helper 共用，避免探测逻辑
//在产品代码与测试代码各写一份。
//
//命名约定对齐 libca：类型 PascalCase，函数与结构体成员 snake_case，
//公开枚举值 SCREAMING_SNAKE_CASE。
//
//设计要点：
//- JDK 发现顺序：JAVA_HOME → MJT_JDK_HOME → fallback_homes →
//  默认安装位置扫描（~/.jdk 与 Windows 各盘 Program Files，开放枚举，
//  不维护发行商静态清单）。
//- 可执行文件双探测：先 <bin>/<name>.exe 再 <bin>/<name>（跨平台友好）。
//- JNI 头校验可选：纯 JVM 用途不要求 jni.h；native 编译用途设 require_jni_header=true。
//- java 信息只读 <home>/release：官方落盘元数据（IMPLEMENTOR=发行商），零进程
//开销、零 process 依赖；不运行 java、不解析 PE/ELF（版本资源仅 Windows 有，
//ELF 无对应字段，跨平台行为会分裂）。

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "libca/core/status.hpp"   //ca::StatusResult

namespace jdk_finder {

namespace fs = std::filesystem;

///JDK 定位结果。available=false 时 reason 填说明。
struct JdkLocation
{
    bool        available = false;
    std::string reason;
    fs::path    java_home;
    fs::path    jdk_include;       //<home>/include
    fs::path    jdk_include_plat;  //<home>/include/{win32|darwin|linux}
};

///find_jdk 的可选配置。
struct FindOptions
{
    ///默认扫描之前的显式回落候选（按顺序尝试）。
    std::vector<fs::path> fallback_homes;
    ///是否要求 <jdk>/include/jni.h 存在。默认 false：纯 JVM 用途不需要。
    ///native 编译（--compile）设 true。
    bool require_jni_header = false;
    ///JAVA_HOME / MJT_JDK_HOME / fallback_homes 均未命中时，是否继续扫描
    ///default_jdk_homes()。默认 true；仅需环境变量与显式候选时设 false。
    bool search_default_homes = true;
};

///默认 JDK 安装位置候选：
///- ~/.jdk 本身及其一级子目录（覆盖单装与多 JDK 目录两种布局）；
///- 仅 Windows：各盘符根下 Program Files 与 Program Files (x86) 的两级枚举。
///候选经 bin/javac(.exe) 收敛：目录存在且形态符合 JDK 安装特征即返回，
///具体合法性（如 require_jni_header 的附加要求）仍由 find_jdk 复核。
std::vector<fs::path> default_jdk_homes();

///探测 JDK：JAVA_HOME → MJT_JDK_HOME → opts.fallback_homes →
///（opts.search_default_homes 时）default_jdk_homes()。
///命中需存在 bin/javac(.exe)（视为合法 JDK 安装的标志）；
///require_jni_header=true 时额外校验 <jdk>/include/jni.h。
JdkLocation find_jdk(const FindOptions& opts = {});

///在已定位的 JDK 下解析可执行文件绝对路径（双探测 .exe / 无后缀）。
///name 形如 "javac" / "java"（不带后缀）。找不到返回空 path。
fs::path jdk_executable(const JdkLocation& jdk, const std::string& name);

///java 版本与发行商信息（issue #1）。
///字段对应 <home>/release 的 JAVA_VERSION / IMPLEMENTOR /
///IMPLEMENTOR_VERSION / JAVA_RUNTIME_VERSION / OS_NAME / OS_ARCH。
///字段值为空表示 release 未提供（如老发行版无 IMPLEMENTOR_VERSION）。
struct JavaInfo
{
    std::string java_version;    //如 "21.0.8"
    std::string vendor;          //发行商，如 "Eclipse Adoptium"
    std::string vendor_version;  //发行商衍生版本号，可能为空
    std::string runtime_version; //如 "21.0.8+9-LTS"
    std::string os_name;         //如 "Windows"
    std::string os_arch;         //如 "x86_64"
};

///查询 java 版本/发行商等信息（issue #1）。解析 <home>/release（key="value"
///行格式，零进程开销）。release 缺失或缺 JAVA_VERSION 时返回 Err。
///注：java.vm.name 等运行时才有的值此处不提供，需要时由消费方经
///jdk_executable 自行获取 java 可执行文件运行。
ca::StatusResult<JavaInfo> java_info(const JdkLocation& jdk);

///同上，直接以 JDK 主目录查询。
ca::StatusResult<JavaInfo> java_info(const fs::path& jdk_home);

///原生 C 编译器种类。
enum class CCompilerKind
{
    AUTO,
    MSVC,
    GCC,
    CLANG,
    ZIG,
};

///探测到的原生工具链。
struct NativeToolchain
{
    bool           available = false;
    std::string    reason;
    CCompilerKind  compiler = CCompilerKind::AUTO;
    fs::path       vcvars_all;  //仅 MSVC：vcvarsall.bat 路径（INCLUDE 环境未配置时需要）
};

///探测原生 C 编译器。requested=AUTO 时按平台默认顺序探测。
///Windows：MSVC（cl 在 PATH 或 vcvarsall 可定位）→ clang → gcc → Zig
///macOS：clang → gcc → Zig
///Linux：gcc → clang → Zig
NativeToolchain find_native_compiler(CCompilerKind requested = CCompilerKind::AUTO);

///定位 VS 的 vcvarsall.bat（Windows）。
///顺序：VCToolsInstallDir 环境变量 → ProgramFiles/ProgramFiles(x86) 下
///Microsoft Visual Studio/{2022,2019,2017}/{Community,Professional,Enterprise,BuildTools}
///扫描 → 该目录下「版本/发行版」开放式枚举兜底（覆盖未来 VS 版本目录名）。
///非 Windows 或未找到返回空 path。
fs::path find_vcvars_all();

///检查命令是否在 PATH 中可用（Windows 走 where，POSIX 走 sh -c 'command -v'）。
bool command_exists(const std::string& command);

}   //namespace jdk_finder
