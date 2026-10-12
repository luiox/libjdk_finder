# libjdk_finder

JDK 与原生工具链探测库，命名空间 `jdk_finder`。

定位 JDK（`JAVA_HOME` → `MJT_JDK_HOME` → 注入回落候选）、`jni.h`、
以及 MSVC / gcc / clang 工具链（`vcvarsall` 定位等），供需要 JNI 头定位
或 C 编译器调用的产品功能与测试侧共用同一份探测实现。

## 构建

依赖 libca（luiox-repo git 直连包）。

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f
xmake build libjdk_finder
```

## License

MIT —— 见 [LICENSE](LICENSE)。
