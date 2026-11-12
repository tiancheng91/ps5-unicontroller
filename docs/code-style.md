# 代码风格

C 使用 clang-format 18：4 空格、100 列、控制流与函数展开、函数大括号换行。`.clang-format` 是唯一格式依据。头文件使用 include guard；接口说明写清资源所有权、线程约束、返回值与超时。状态修改和错误处理使用明确分支，业务变更避免堆叠在表达式内。

HTML/CSS/JS、Markdown、JSON、JSON 使用 Prettier 3.6.2，2 空格。Python 使用 Black 25.1.0，4 空格。Makefile 按变量、源文件、构建、测试、维护目标分组，多项列表逐行续写；配方使用真实 Tab，不能用普通文本格式化器替换。

```sh
python3 -m pip install -r requirements-dev.txt
make format CLANG_FORMAT=clang-format-18
make format-check CLANG_FORMAT=clang-format-18
```

生成目录 build/dist/target 和 .git 不参与格式化。新增源码自动被检查发现。生成资源从格式化后的源码构建，不手工改生成 C 数组。CI 先检查格式再编译和测试。

格式化不等同架构改进；HTTP协议解析与JSON维护成本单独评估，避免把自研通用协议栈作为项目长期核心。
