# PDF 样本内嵌字体通知

`printed_textbook_2p.pdf` 内嵌了 `DroidSansFallbackFull.ttf` 使用字形的 TrueType 子集（`pdffonts` 显示 `emb=yes, sub=yes, uni=yes`）。生成时使用 Ubuntu `fonts-droid-fallback` 包中的 `/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf`，源文件 SHA-256：`acb6440a713d880a13a21b468ba7cd43f5a2b2934972e51be791c880730777b8`。

本机 `/usr/share/doc/fonts-droid-fallback/copyright` 记载：Droid 字体版权归 Google Corp.（2006–2010），许可为 Apache License 2.0；上游来源为 [Android frameworks/base 的 data/fonts](https://android.googlesource.com/platform/frameworks/base/+/d405a43/data/fonts/)（该目录列有 `DroidSansFallbackFull.ttf` 与 `MODULE_LICENSE_APACHE2`）。本仓库根目录 [LICENSE](../../../LICENSE) 含 Apache License 2.0 全文。再分发 PDF 样本时请同时保留本通知与许可证文本。Droid 是 Google Corp. 的商标。
