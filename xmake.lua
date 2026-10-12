-- libjdk_finder：JDK 与原生工具链（MSVC/gcc/clang）探测库（独立构建根）。
--
-- 依赖：
--   - libca（luiox-repo git 直连包）：core/fs 等基础面，公开头包含 libca 头。
--   - gtest（xrepo，仅 --with_tests=y；本仓暂无 unittest 目标）。
--
-- 仓库注册：本文件不声明 add_repositories——包沙箱会继承宿主的仓库注册，
-- 在工程内重复注册同名仓库会导致嵌套包构建时 remote 路径错乱。独立开发/CI
-- 先执行：
--   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git

set_languages("cxx17")
add_rules("mode.debug", "mode.release")

add_requires("libca 0.0.15", {configs = {modules = "core,fs"}})

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
    add_headerfiles("src/(jdk_finder/**.hpp)")
    add_includedirs("src", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
