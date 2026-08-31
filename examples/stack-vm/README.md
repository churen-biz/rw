# Stack VM 教程代码

这个目录保存《从第一行 C 到完整栈式 VM》系列教程的代码。

- `vm.c`：第一个里程碑，只有整数、局部变量、跳转和指令预算；
- `SPEC.md`：最终教学 VM 的功能边界和验收标准；
- `reference/`：最终解释器、二进制模块 loader 与命令行 runner；
- `tools/`：生成示例 `.svm` 文件的 C 程序；
- `tests/`：成功路径与恶意/错误模块测试。

当前的第一个里程碑可以直接运行：

```bash
cc -std=c11 -Wall -Wextra -Werror vm.c -o /tmp/stack-vm-01
/tmp/stack-vm-01 5 1000
/tmp/stack-vm-01 5 5
```

第二条命令会因为指令预算耗尽而返回失败，这是预期行为。

构建并验证最终 VM：

```bash
make clean test
make demo
```

`make demo` 用 C 生成 `build/answer.svm`，再从文件加载、验证和执行，最终输出 `i32:42`。完整设计与各里程碑验收条件见 `SPEC.md`。
