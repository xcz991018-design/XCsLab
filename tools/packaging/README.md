# Windows 离线部署包

目标：Windows 10 1903 及以上 / Windows 11 x64，CPU 推理。目标机无需安装 Python、Paddle 或 Qt，无需联网下载 OCR 模型。
摄像头与数据库仍使用软件原有配置；发布包不包含开发机账号、设备档案或数据库。

## 构建

先构建 Release GUI 与离线验收程序：

```powershell
cmake --build build --config Release --target gauge_reader_gui
cmake -S tools/ocr_package_test -B build/ocr_package_test -DQt5_DIR=C:/Qt/Qt5.14.2/5.14.2/msvc2017_64/lib/cmake/Qt5 -DOpenCV_DIR=C:/openCV/opencv/build
cmake --build build/ocr_package_test --config Release
```

准备 Python 3.12 的 `site-packages`（包含 `tools/ocr/requirements.txt` 的固定版本）及 PP-OCRv5 server 检测/识别模型。
目前项目开发环境可直接作为输入。打包脚本仅复制所需文件，不复制 venv 解释器或 `pyvenv.cfg`。

下载官方 [Python 3.12.10 Windows x64 嵌入式运行时](https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip)，
保存为 `build/packaging-cache/python-3.12.10-embed-amd64.zip`。
构建器校验 SHA256 `4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3`，
来源为 Python 官方对应的 SPDX 文件。嵌入式解释器通过相对路径 `_pth` 隔离注册表、环境变量及系统 Python。
固定运行时版本用于重现，后续升级 Python 时需同步更新校验值并重新验收原生扩展兼容性。

```powershell
python tools/packaging/build_offline_package.py --output build/dist/GaugeReader-offline-v1 --zip
```

输出目录必须不存在，避免覆盖已有包。可用 `--release`、`--site-packages`、`--models`、`--qt-bin`、
`--vc-redist`、`--verifier`、`--python-archive`、`--opencv-bin`、`--manifest-tool` 指定其他输入路径。
打包阶段运行 `windeployqt`，复制 OpenCV FFmpeg 视频后端、MSVC CRT/OpenMP 到应用和 Python 目录，
并保留 Python 包、模型的许可证与元数据。
`manifest.json` 记录所有发布文件的 SHA256 和依赖版本。

构建器使用 Windows SDK 的 `mt.exe` 保留并补充 Python EXE 的嵌入清单，
将其进程代码页设为 UTF-8，修复 Paddle 底层在中文安装路径读取模型失败的问题。
不更改系统语言设置，也不复制模型到其他目录；验收程序检查 `GetACP()==65001`。
该行为需要 Windows 10 1903+，参见 [Microsoft 的进程 UTF-8 清单说明](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page)。
发布的 Python EXE 因嵌入清单而不同于官方原始文件，以发布清单中的 SHA256 为准。

## 目录与运行行为

```text
GaugeReader-offline-v1/
  gauge_reader_gui.exe
  gauge_reader_service.exe
  ocr_verify.exe
  *.dll / platforms/ / imageformats/ ...
  verify-offline.ps1
  manifest.json
  licenses/
  ocr/
    offline.json
    paddle_scale_ocr.py
    runtime/python.exe + python312.zip + Lib/site-packages/
    models/detection/ + recognition/
    validation/dial.png + verify_python.py
```

完整解压到较短路径后启动 GUI。`offline.json` 存在时，C++ 强制使用包内解释器和桥接脚本，
忽略开发环境的 `GAUGE_OCR_*` 路径覆盖；运行时缺失即报错，不回退到系统 Python。
模型显式从包内目录加载，Python audit hook 拒绝连接、DNS 和数据报发送；不触发模型下载。
可写缓存放在 `%LOCALAPPDATA%/GaugeReader/ocr/cache`，不要求安装目录可写。
普通开发目录没有 `offline.json` 时，仍支持原来的环境变量和 venv 用法。
开发构建缺少 OCR 环境时会直接提示，不再回退到未配置的系统 Python。
Qt Creator 使用 `build/debug` 时，可执行
`python tools/ocr/deploy_runtime.py --bundle build/dist/GaugeReader-Windows-x64-offline-ready --destination build/ocr`
把离线环境部署到共享 OCR 目录；无须把整套软件改为 Release 运行。

## 验收

在发布目录中执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\verify-offline.ps1 -OutputDirectory C:\GaugeReaderReports
```

脚本将子进程 PATH 限制为 Windows 目录，给 Python/venv/OCR 环境变量写入无效路径，
每次使用新的用户目录和缓存。先检查清单校验值，再调用包内 Python 做真实 OCR，
检查所有 Python 模块位于包内、所有非系统 DLL 和 VC 运行库均来自包内、无网络尝试。
随后通过编译的 C++ 验收程序运行生产 `PaddleScaleOcr::run`，要求生成6个正确排列的刻度点。
验收过程中不设置摄像头或数据库。

报告含 `python-report.json`、`cpp-report.json`、日志和总体 `result.json`。
主机路径隔离测试不是全新 Windows 虚拟机测试，也不是操作系统级断网测试。
建议把 ZIP 完整解压到另一位置（包括带空格、中文的路径）后再运行，以验证可搬移性。

构建器还会在包旁生成 `.wsb`：映射安装目录为只读，仅报告目录可写，禁用网络、剪贴板、音频及 vGPU。
在支持 Windows Sandbox 的 Windows 电脑上打开它即可运行干净访客验收。
跨电脑复制时，先把 `.wsb` 中两个 `HostFolder` 改为目标机的解压目录和可写报告目录（均为绝对路径）。
访客模式检查账号及不存在活动网络适配器，并在报告中标记 `windows_sandbox` / `os_network_disabled=true`。
如果主机没有 Sandbox，则只能报告主机隔离测试已完成，不得声称已完成干净访客验收。

## 本次验证记录（2026-10-23）

- Release GUI、算法回归、桥接进程协议与标定界面回归通过。
- 使用官方嵌入式 Python、包内原生依赖和本地模型，标准路径隔离验收通过。
- 将安装目录搬到含中文、空格的路径，使用全新用户缓存、无效 Python/OCR 环境变量以及 Python 网络拒绝策略后，
  Python/DLL 来源检查通过，网络尝试为0；生产 C++ 调用生成6个标定点，耗时约46秒。
- 缺少包内解释器时拒绝运行而不回退到系统 Python；缺少本地模型时拒绝运行而不下载。
- 最终 ZIP 内15,048个文件逐一通过 SHA256 校验，压缩包大小459,003,357字节。
- 主机为 Windows 家庭中文版，没有 Windows Sandbox；尚未执行全新 Windows 访客与操作系统级断网验收。
  随包旁生成的 `.wsb` 可在支持 Windows Sandbox 的电脑上继续该项验收。

产物：`build/dist/GaugeReader-Windows-x64-offline-ready.zip`，同目录有 `.zip.sha256` 和 `.wsb`。
主机测试报告位于 `build/offline-validation/`，不会作为用户配置进入发布包。
