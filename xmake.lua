-- libjdk_finder：JDK 与原生工具链（MSVC/gcc/clang）探测库（独立构建根）。
--
-- 依赖：
--   - libca（luiox-repo git 直连包）：core/fs/env。fs 提供 Path 编码边界与
--     FileUtil 枚举，env 提供跨平台 UTF-8 环境变量读取；公开头包含 libca 头
--     （core/status.hpp，java_info 的 Result 返回）。
--   - gtest（xrepo，仅 --with_tests=y；本仓暂无 unittest 目标）。
--
-- 仓库注册：本文件不声明 add_repositories——包沙箱会继承宿主的仓库注册，
-- 在工程内重复注册同名仓库会导致嵌套包构建时 remote 路径错乱。独立开发/CI
-- 先执行：
--   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git

set_languages("cxx17")
add_rules("mode.debug", "mode.release")

add_requires("libca 0.0.16", {configs = {modules = "core,fs,env"}})

option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable unit tests (pulls gtest)")
option_end()

target("libjdk_finder")
    set_kind("static")
    -- jdk_finder.hpp include libca 头；public 供消费方传递。
    add_packages("libca", {public = true})
    add_files("src/jdk_finder/*.cpp")
    --公开头只装 jdk_finder.hpp：detail.hpp 是实现内部助手，不进安装面。
    add_headerfiles("src/(jdk_finder/jdk_finder.hpp)")
    add_includedirs("src", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
