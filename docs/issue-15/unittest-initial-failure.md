# 首次完整 unittest 环境错误记录

第一次运行 `/home/dr/project/google_edge/litert-env/bin/python -m unittest discover -s tests -v` 时，新加入的 `test_issue15_quality` 导入评分器，评分器又导入 `tests/pdf_real.py`；后者顶层需要 `jsonschema`，而这个旧探针环境没有该包。命令退出 1，终端摘要为 `Ran 13 tests in 39.231s`、`FAILED (errors=1)`，错误尾部为：

```text
File "/home/dr/project/docprase/tests/pdf_real.py", line 9, in <module>
  import jsonschema
ModuleNotFoundError: No module named 'jsonschema'
```

初次完整日志在随后复测时被同一路径覆盖；上述为当时保留的终端输出摘录，不冒充完整原始日志。独立的运行环境复现输出见 [`evidence/unittest-dependency-reproduction.log`](evidence/unittest-dependency-reproduction.log)。评分器随后改为只解析 #11 文件里的参考字面量，并用标准库处理表格参考；新增两项测试保留在自动发现中。最终完整 unittest 结果见 [`evidence/final-unittest.log`](evidence/final-unittest.log)。
