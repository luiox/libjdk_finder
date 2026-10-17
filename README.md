# libjdk_finder

JDK 与原生工具链探测库，命名空间 `jdk_finder`。

定位 JDK（`JAVA_HOME` → `MJT_JDK_HOME` → `fallback` 候选 → 默认安装位置
`~/.jdk` 与 Windows 各盘 `Program Files` / `Program Files (x86)`）、查询
java 版本与发行商（`release` 文件解析，零进程开销）、定位 `jni.h`，以及
MSVC / gcc / clang 工具链（`vcvarsall` 定位等），供需要 JNI 头定位或
C 编译器调用的产品功能与测试侧共用同一份探测实现。

## 构建

依赖 libca（luiox-repo git 直连包）。

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f
xmake build libjdk_finder
```

## License

MIT —— 见 [LICENSE](LICENSE)。
