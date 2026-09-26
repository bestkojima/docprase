# 图像编解码依赖

本目录原样固定 [nothings/stb](https://github.com/nothings/stb) 的 commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`：

| 文件 | 版本 | SHA-256 |
| --- | --- | --- |
| `stb_image.h` | 2.30 | `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` |
| `stb_image_write.h` | 1.16 | `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05` |

作者在各头文件末尾提供 Public Domain 或 MIT 两种许可选项。本项目按 MIT 许可使用，许可原文保留在各头文件末尾。生产构建不从网络下载依赖。

当前仅启用 PNG/JPEG 解码及 PNG 编码。入口将图像转换为拥有自身内存的 RGB8；16 位和带 alpha 的编码图像会明确拒绝，避免未经约定的色彩或透明度处理。单页上限为 1600 万像素，编码输入上限为 64 MiB。
