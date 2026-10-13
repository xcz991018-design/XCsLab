# 后台巡检服务

## 数据库表计算法模板

模板库以 MySQL 的 `gauge_templates` 表为唯一来源，包含模板编号、完整参数 JSON、
递增版本号、创建/修改时间和删除时间。主界面模板管理、设备档案下拉框、单设备分析、
全站巡检及命令行识别均直接读取数据库。数据库不可用或模板不存在时明确报错，
不读取本地 `gauge_profile.json`，也不使用默认模板替代识别。

新数据库初始化时会创建该表；旧数据库首次连接新版程序时检查并创建缺失的表，
也可手动执行 `database/mysql/004_gauge_templates.sql`。建表账号需要 CREATE 权限。
已有模板只需一次性迁移（使用 GUI 保存的同一 MySQL 连接配置）：

```powershell
./build/Release/gauge_reader_service.exe --import-templates ./config/gauge_profile.json
```

该命令只迁移模板后退出，不启动巡检。全局设置及文件继承参数先解析成独立模板，
再在一个数据库事务中导入；重复迁移不会覆盖已有模板或恢复已删除模板。
旧 JSON 可以留作迁移备份，正常运行不再需要它。命令行单图识别使用 `--template-id`，
后台服务不再接受 `--profiles`，图形界面不再提供本地算法配置文件入口。

模板增删改直接确认数据库提交后才显示成功；不建立模板本地缓存或补写文件。
异步数据库操作不会阻塞界面。版本校验拒绝过期编辑；删除使用删除时间标记保留记录，
并在事务中检查设备档案引用。设备档案写入也在同样的锁顺序下验证模板存在。
历史巡检的算法配置快照不受后续模板修改/删除影响，新任务只冻结所选模板及其版本。

摄像头网络发现独立于数据库档案加载，模板错误或档案补写等待不会停用刷新扫描。
已保存连接信息只提供要探测的地址和 RTSP 端口，必须收到真实 ONVIF/RTSP 响应才
显示发现结果；数据库档案尚未就绪时，仅显示网络发现，不提交不完整设备快照。
档案加载和定时补写按阶段执行，避免同时争抢注册表补写锁。
可用 `gauge_reader_service --check-database` 只读检查模板数、设备数及缺失模板引用。

## 旁路识别质量 quality-v1

识别后额外计算针身、针尾、朝向质量分，不改变检测权重、success、value、confidence
或已有数据库质量状态。质量分不是概率。GUI 结果区显示旁路状态和三个分数，悬停
可查看各分项及原始指标；指标和原因保存在 result.json 的 recognition_quality 中，
巡检紧凑结果也保留该字段。未执行识别的任务不产生评估。

配置参考 config/recognition_quality.example.json。将 recognition_quality 对象合并至
仪表档案顶层或指定仪表对象；局部配置覆盖顶层配置。缺省采用示例权重和阈值，非法
权重或区间回退默认值。有效配置写入任务配置快照的 _runtime_quality_config，历史
重分析优先使用该快照。JSON/数据库补写信封携带质量结果；当前数据库历史表尚无
独立质量指标列，原有数据库 quality_status 保持兼容，不代表新旁路结论。

针身支持率使用共同范围的真实边缘点，不把预测补点算作支持。范围统计按当前
2 px 采样栅格计算，覆盖与最长缺失按已检测跨度统计，不使用表盘半径/直径。
因此不能发现已检测范围之外完全不可见的针身；最低真实截面数和连续对数量避免
极短片段被判可信。拟合残差除以针身代表宽度。针尾不使用直线性，曲线平滑及宽度
评分排除最后 15% 的弧形末端，末端仍参与支持/覆盖统计。

形状、宽度阈值为初版实验值，需现场验证。朝向沿用当前方向得分，针尾质量不足
时记录已有方向加分需复核，不重新修改方向或读数；长度、宽度比只记录诊断。

`InspectionService` 是不依赖 MainWindow 的运行服务，GUI 和
`gauge_reader_service` 使用同一套调度与处理流水线。识别算法保持一致，图像输出由运行模式控制。

## 数据流

任务队列 → 采集线程池 → 识别线程池 → 存储线程池 → 完成信号。

默认最多四个在途任务，存储单线程。限制覆盖全部阶段，因此存储变慢时
不会无限积压图像。界面只创建任务、接收进度和显示结果，图片按需从磁盘加载。
每个任务有服务生成的 UUID，并复制设备配置，不读取界面中的当前摄像头或画面。
每个任务独立创建识别引擎和标定对象，关闭共享文件名的调试输出。

## 编译与运行

```powershell
cmake -S . -B build-service
cmake --build build-service --config Release --parallel 4
ctest --test-dir build-service -C Release --output-on-failure

./build-service/Release/gauge_reader_service.exe --devices config/devices.json --output inspection_snapshots --concurrency 4 --interval 60
```

运行环境需要 Qt 和 OpenCV DLL 可被搜索到。`--interval 0` 执行一次；正数表示
上一轮完成后等待指定秒数，再启动下一轮，不重叠运行。单次运行退出码：
0 全部识别成功；1 任一任务失败；2 参数或配置无效。
本地图像设备必须通过 `--image` 显式指定图像，网络设备不会回退到本地图像。

每个 UUID 目录包含 `result.json`，图像根据模式保存。使用原子文件提交，不覆盖历史任务。
失败读数在 JSON 中为 null。采集时间使用 UTC，GUI 显示本地时间。
存储失败单独返回 STORAGE_FAILED，不把结果标为成功。

## 三种运行模式

| 模式 | 引擎输出 | 后台保存 |
| --- | --- | --- |
| production | 读数、质量指标、错误信息；不生成结果图片、HUD 或中间图 | result.json、采集原图 original.png |
| evidence | 核心四步图：原图、矫正图、检测输入灰度图、最终识别标注图（含双侧边缘与宽度）；不生成详细步骤图或 HUD | result.json、original.png、rectified.png、recognition.png（灰度图用于 GUI 展示） |
| debug | 原图、矫正图、检测输入灰度图、识别标注图和 HUD | result.json、original.png、rectified.png、gray.png、recognition.png、diagnostic_hud.png |

后台程序使用 `--mode production|evidence|debug`，默认 production。GUI 下拉框
控制下一次分析和巡检，默认 evidence。原有单图 CLI 同样支持 `--mode`，默认 debug
以保持兼容。引擎直接调用使用 `process(frame, profile, mode)`，省略 mode 保持 debug。

三种模式都直接在灰度图上检测指针，并执行相同的圆心偏移检查。模式仅控制图片和诊断输出。
production/evidence 不写共享调试文件；后台 debug 使用任务 UUID 目录存储。
单图 debug 启用 save_debug_images 时仅保存核心图片和 HUD 到 debug_images。
生产模式不会自动升级为调试模式，排查失败任务时可显式选择 debug 重跑。

所有巡检模式都异步保存本次实际采集原图，供人工复核。即使标定加载或识别失败，
只要抓拍成功也保存原图；抓拍失败不生成替代图。原图与 task_id、camera_id、UTC
采集时间及读数绑定，JSON 的 review_image_available 表示原图是否已保存。
原图缓冲仅在有界流水线中持有，保存后释放，不长期留在设备列表内存中。
单设备任务允许返回当前任务图像给界面，仅保留当前分析和当前快照；切换设备后清空。
引擎 production 模式仍不产生图片，原图留存由采集和存储服务负责。

巡检照片窗口支持按原始分辨率查看原图和使用同一次原图重新分析，不重新抓拍。
窗口打开时复制任务记录，下一轮巡检不会改变正在查看的设备和图片。
文件路径复核使用当前标定参数；数据库复核使用原任务保存的档案和配置快照，
不会替换原任务读数。数据库复核记录复核人编号、确认/修正/无法读数结论、
修正值、备注和时间；复核人编号目前由操作者输入，尚未接入账号认证或审批。
图片保留期限与容量清理仍需根据现场要求配置实现。

## 明确的输入入口和采集状态

- 实时抓拍：`CameraSnapshot`，只读取所选设备的真实帧；本地文件设备被拒绝。
- 本地图像分析：`LocalImage`，必须显式提供文件路径，不调用网络摄像头。
- 历史原图复核：`HistoricalImage`，必须提供原任务 ID、原采集时间和原图路径；
  文件丢失就失败，不重新抓拍、不使用界面缓存或测试图片替代。
  服务核对原图旁 result.json 的任务 ID、设备、时间及原图路径，身份不一致返回
  HISTORICAL_IDENTITY_MISMATCH。原有单图 CLI 也必须显式传入 --image，不再加载默认测试图。

JSON 保存 input_kind、parent_task_id、captured_at、processed_at 和
recognition_performed。历史复核保留原图的时间和设备身份，生成新的分析任务。
单设备的抓拍、文件加载、识别、证据保存均通过 InspectionService 执行；只抓图
不识别通过 captureOnly 表示，不要求加载标定文件。实时预览通过 CameraPreviewService
执行连接、读帧、切换和关闭，共用同一个源工厂。旧设备晚到的预览结果被丢弃。
没有选择图片或成功抓拍前，“分析指定图片”不会自动加载默认测试图片。

ICameraSource 的 typed API 为 readFrame()/captureSnapshot()，返回 FrameCaptureResult：
OK、NOT_OPEN、OPEN_FAILED、READ_FAILED、NO_FRAME、STALE_FRAME、UNSUPPORTED_PROTOCOL、
INVALID_INPUT。失败结果不允许送入识别。保留旧 bool API 用于兼容，失败时清空输出帧。
ONVIF 读取仅返回未叠加文字的真实帧，不再返回样例图或连接诊断画面；诊断状态在 UI
文本中显示。快照使用配置好的地址和凭据，不再尝试硬编码的 admin 地址。
OPEN_FAILED 目前不能进一步可靠区分鉴权失败与网络超时，尚需协议层诊断支持。

时间戳为服务收到图片的时间，尚不是摄像头硬件曝光时间。默认网络抓拍和预览在独立
采集子进程中执行，整次采集默认期限 8000 ms，覆盖启动、连接、读帧、关闭及图片编码。
后台可用 `--capture-timeout 8000` 设置期限（100–120000 ms）。超时返回 CAPTURE_TIMEOUT，
子进程异常退出返回 CAPTURE_WORKER_FAILED；失败图片不进入识别。取消、切换预览设备时
终止旧采集进程，监测轮询间隔 25 ms，终止后的回收最多等待 1000 ms。
设备凭据通过匿名管道传递，不写命令行或临时配置文件，采集库日志不转发。
网络预览使用常驻采集进程保持同一条视频流，每次请求回传最新帧，不逐帧重建连接。
首次画面等待、断流恢复均受采集期限约束；超时或进程异常后释放进程，下次请求重新连接。
切换设备时取消旧请求并终止旧进程，旧设备晚到的结果不会显示。
注入的 SourceFactory 仍在进程内运行，仅适用于测试或已保证有界返回的适配器；
本地图片读取和数据库历史原图读取不属于网络采集隔离范围。

## 停止与边界

停止立即禁止派发新任务，并丢弃尚未完成的结果。正在执行的网络采集会被终止；
算法和存储需要返回后才结束，服务析构等待工作线程退出。
停止完成之前不能启动下一轮。

生产采集当前支持 LOCAL_FILE 和 ONVIF/RTSP。GB28181 与 SDK 演示实现返回
UNSUPPORTED_PROTOCOL，须接入真实适配器后再启用。服务提供可注入的源工厂，
用于扩展协议和确定性测试。

本次完成进程内服务分层、独立后台程序和可选 MySQL 存储，未实现 GUI 到独立进程的 RPC、
分布式节点协调或 Windows 服务安装。GUI 内嵌服务会随 GUI 退出；
如需独立持续运行，应启动 headless 程序。识别阶段冻结配置内容，数据库保存档案、
标定和识别配置版本；数据库历史再分析读取该任务的快照。证据文件尚无自动清理策略。
模式优化减少图像输出、复制和可视化开销，算法内部必需的灰度、梯度图仍会生成。

## MySQL 档案、历史和原图

实现使用 MySQL 官方 C 客户端库，不依赖 Qt QMYSQL 插件。CMake 的 MYSQL_ROOT
可指定客户端开发目录（默认 C:/Program Files/MySQL/MySQL Server 8.0）。Windows
CMake 构建会部署 libmysql.dll、libssl-3-x64.dll 和 libcrypto-3-x64.dll。
Qt/OpenCV 的原有运行依赖仍须部署。qmake 构建也支持 MYSQL_ROOT；须将上述
MySQL DLL 放在可执行文件目录或加入 PATH。

先由数据库管理员创建专用数据库，在该数据库中依次执行以下脚本，每份只执行一次：

1. database/mysql/001_initial.sql
2. database/mysql/002_database_images.sql
3. database/mysql/003_runtime.sql

脚本不会自动创建数据库或更改服务器配置。已执行前两份的环境只执行第三份。
运行账号需要这些表的 SELECT/INSERT/UPDATE/DELETE 权限，不需要建表权限。
脚本在独立 MySQL 8.0.42 实例上通过验证；也适用于计划中的 MySQL 8.4。

GUI 顶部“MySQL 连接配置”支持填写地址、端口、数据库名、账号、密码，测试连接和
保存配置。测试在后台连接所填写的数据库，不会写入业务数据；数据库表未准备齐全时
缺失的数据库及新库表结构会自动创建；已有旧版表结构仍需执行迁移脚本。保存后关闭并重新启动程序生效，避免运行中切换采集任务的数据库。
下次正常双击启动无需 PowerShell 配置。

配置保存在当前用户的 GenericDataLocation/GaugeReader/mysql_connection.json
（Windows 通常为 %LOCALAPPDATA%/GaugeReader/mysql_connection.json）。密码使用
Windows DPAPI 当前用户加密，不存明文；换 Windows 用户/电脑需要重新填写密码。
配置文件采用原子写入，格式或密文错误会在配置窗口提示。GAUGE_MYSQL_CONFIG_PATH
可指定配置文件路径。GUI 与 headless 共用此配置；当前进程读取后固定，保存的新配置
在重启后生效。数据库及表仍需要管理员事先创建，保存配置不自动建库或建表。

部署仍可用环境变量覆盖保存配置；设置 GAUGE_MYSQL_DATABASE 时完整连接参数以
环境变量为准。配置窗口会提示该覆盖；从带有旧环境变量的 PowerShell 启动时需要
清除这些变量，或者关闭后从正常桌面入口启动。未设置该变量时自动读取保存配置。

需要环境变量部署时，在启动 GUI 或 headless 服务前设置：

```powershell
$env:GAUGE_MYSQL_HOST = '127.0.0.1'
$env:GAUGE_MYSQL_PORT = '3306'
$env:GAUGE_MYSQL_DATABASE = 'gauge_reader'
$env:GAUGE_MYSQL_USER = 'gauge_runtime'
# GAUGE_MYSQL_PASSWORD 通过部署环境注入，不写进仓库。
$env:GAUGE_MYSQL_TLS_MODE = 'required'
# 验证服务器身份时设置 verify_identity，并配置 CA：
# $env:GAUGE_MYSQL_CA_FILE = 'C:/deployment/mysql-ca.pem'
$env:GAUGE_MYSQL_OUTBOX = 'C:/GaugeReaderData/mysql_registry_outbox'
```

首次启动没有保存配置时，默认启用本机 MySQL 连接（BYS_GaugeReaderDB）；可在配置界面关闭 MySQL 以使用文件模式。启用后数据库是设备档案的权威来源；
首次连接一个未初始化档案的数据库，会导入已有 devices.json（不会凭空生成档案）。
数据库不可用或缺少表时 UI 明确显示加载失败，不以样例设备替代；30 秒后自动重试。
档案编辑先原子保存本地凭据缓存及待同步快照，再由后台同步。设备移除在数据库中
采用软删除，保留历史记录。当前完整档案快照由一个 GUI 管理实例写入；不支持多个
管理实例同时修改同一份档案。后台巡检不会覆盖现有档案。

数据库档案/历史快照/待同步 JSON 不包含摄像头密码或带凭据的 RTSP 地址。摄像头
密码仍从本机 devices.json 按 camera_id 匹配；新部署终端需配置对应的本地凭据。
数据库连接密码从进程环境或当前用户加密配置读取，不写入日志。设备档案包含 IP、端口、协议、位置、
表计档案、自动连接标志和标定参数。历史配置采用内容哈希复用版本，在线状态变化
不会创建新的配置版本。

每个任务记录采集/处理时间、输入类型、算法读数、识别质量、失败信息和父任务编号。
识别质量与工艺状态分开；目前没有工艺上下限判断，process_status 保存 not_evaluated，
不能把识别成功解释成设备工况正常。

真实采集原图无损编码为 PNG，存入独立 image_blobs.LONGBLOB，证据表关联任务与图片。
图片不采用 Base64，不随分页列表返回。SHA-256 去重并用于读取校验；数据库原图重新
分析复用原 image_id，保留采集时间，生成新的任务和读数。默认使用历史配置；在单设备
界面主动重新标定后，会显式使用更新后的配置并保存新的配置版本。原图读取验证设备/任务归属、
字节长度、校验值及图片尺寸，失败时不回退到本地文件或重新抓拍。
当前单图上限 32 MiB、解码上限 5000 万像素；服务端 max_allowed_packet 建议至少
64 MiB。图片与任务、配置、证据关联在同一事务内提交，失败时全部回滚。

GUI 顶部“数据库历史查询 / 原图复核”支持精确摄像头编号、处理时间范围和每页 50 条
分页查询。点击“从数据库拉取原图复核”后，后台才读取图片；支持原始分辨率、缩放、
导出原图、保存人工复核结果和使用数据库原图重新分析。历史列表显示最近复核结论，
原始算法值保持不变，全部复核事件独立保留。数据库连接、查询、编码、解码及文件
导出均在后台执行，关闭窗口会等待已开始的工作完成。

历史任务仍保留本地任务目录，作为失败补写与离线证据。每次入库前原子创建
output/.mysql_pending/*.json；入库成功后清除该待同步文件。UI/headless 启动及运行
期间每 30 秒按批补写，任务 UUID 保证补写不重复。数据库不可用时原算法结果仍保留，
UI 日志报告待同步，headless 单轮返回失败退出码。档案待同步目录由 GAUGE_MYSQL_OUTBOX
指定（默认在应用本地数据目录），用于崩溃重启后的恢复；不要删除这些目录及对应图片。
数据库成功入库后暂不自动删除本地原图；数据库/文件容量、备份与保留策略需另行配置。
启用数据库不会自动导入以前只有 result.json 的历史目录，因为它们缺少可靠的配置
快照；本次新增任务均入库，旧记录仍可通过原文件复核入口查看。

## MySQL 集成验证

新增 mysql_storage_and_review 测试默认跳过，避免误连接业务库。启用验证时必须使用
独立本机 127.0.0.1:33379 测试实例，root 空密码仅用于该临时实例；设置
GAUGE_MYSQL_TEST_PORT=33379 后运行 ctest。测试自动创建随机 gauge_test_* 数据库，
执行四份迁移并在结束时删除自己的测试库，不操作其他数据库。UI 测试使用 offscreen。
覆盖档案脱敏、配置快照、原图字节一致性、事务回滚、幂等写入、分页/参数绑定、历史
原图再分析、UI 按需拉取显示、人工修正、断线补写、软删除和篡改图片拒绝。

默认首次连接使用 127.0.0.1:3306、数据库 BYS_GaugeReaderDB、账号 root；请在 GUI 中填写实际密码并保存后重启。已有保存配置及环境变量优先。缺失的数据库由后台自动创建并初始化八张业务表（utf8mb4）；账号需要 CREATE 权限。连接、权限或初始化失败会显示错误并保留待入库数据。原有数据库不会自动执行 ALTER 迁移。
