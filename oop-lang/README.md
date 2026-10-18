# oop-lang：早期实验代码树，不参与主构建

这个目录是项目早期的另一套实现尝试（有独立的 `src/`、`runtime/`），
**没有被主 `CMakeLists.txt` 引用，不参与构建，也不参与测试。**

主实现请看仓库根目录的 `frontend/` `irgen/` `aot/` `runtime/`。

## 关于本目录里的 raft 引用

`examples/distributed_test.cpp` 与 `examples/distributed.pys` 引用了
`raft.h` 与 `raft.*()` 方法。这些文件**在 2026-10 已被移除**，
本目录因不参与构建所以未同步更新——里面保留的是当时的写法，作为历史参考。

自研 raft 模块的移除原因、替代方案与恢复方式，见
[../docs/raft-removal.md](../docs/raft-removal.md)。

需要参考分布式 API 的当前用法，请看 `docs/` 下的设计文档，
不要以本目录的示例为准。
