# Companion App（LX04 / Android 8.1）

Shairport Sync Android 是本机播放信息与控制界面。`SPSCompanionService` 使用
Android 前台服务、低优先级通知、`START_STICKY` 和开机广播维持监听。首次手动
打开后默认开机启动；系统资源限制、厂商策略和用户强行停止仍可阻止恢复。
它不安装、启动或守护 SPS/NQPTP，也不管理 AudioFocus。
App 退出或被杀不会停止原生播放；后台未连上时显示连接状态。

## 构建与部署

原生构建维持原入口，默认增加 `--with-android-companion`，启用 metadata hub 和
AirPlay 2 remote，不依赖 FIFO、MQTT、D-Bus 或 DACP/Avahi。Autotools 直接构建时，
此选项必须同时使用 `--with-os=android --with-airplay-2`；省略则不引入桥接。

```sh
export ANDROID_NDK_ROOT=/absolute/path/to/Android/sdk/ndk/27.3.13750724
python3 android/scripts/build.py --offline
export ANDROID_HOME=/absolute/path/to/Android/sdk
./android/gradlew -p android assembleDebug lintDebug
```

App 使用 Gradle 8.13、AGP 8.11.1、Kotlin/Compose Compiler 2.2.21、
Compose BOM 2026.06.01（UI/Foundation 1.11.4）、Activity Compose 1.11.0。
启用 AndroidX 与 Compose，compileSdk 36、minSdk/targetSdk 27，建议使用
JDK 17 或 21。targetSdk 27 是 API27 专用侧载版本的明确边界，不是 Play 发布配置；
新版 Android 的后台弹出、横屏和保活不在首版验收范围。APK 位于
`android/app/build/outputs/apk/debug/app-debug.apk`，不包含原生二进制。

先枚举设备并确认型号/API/ABI，再显式指定序列号：

```sh
adb devices -l
android run --device=SERIAL --apks=android/app/build/outputs/apk/debug/app-debug.apk
adb -s SERIAL shell cmd package list packages -U org.shairport.sync.android
```

根据输出的 UID 在现有 SPS 配置中添加以下内容，再通过已有方式启动新版 SPS：

```conf
android_companion = {
  enabled = true;
  allowed_uid = 10048; // 示例；必须填写本次安装的实际 UID。
};
```

桥接缺省关闭。启用但缺少有效 App UID 时只记录警告，SPS 继续正常工作。
卸载重装或切换 Android 用户后须重新核对 UID。首版 App 只信任 UID 0 的 SPS；
专用低权限域、跨用户运行和 root 之外的运行身份需要后续适配与验证。
App 不需要 root 授权。SELinux 若禁止 socket 通信则会重连，不能因此声称该域可用。
封面通过内存桥接传输，不需要 App 读取 SPS 的文件目录；已有上游封面缓存配置仍独立生效。

## Android 系统音量

`android.sync_system_volume` 默认为 `true`。同步需要 AAudio、已启用的
Companion 桥接和允许 SPS 调音的配置。Android AAudio 在开关开启时，
未指定 `general.volume_max_db` 时使用 60% 线性增益（约 −4.437 dB）。
显式设置优先，支持小数 dB。关闭同步后，未指定该项时沿用原有 0 dB 上限。

同步期间 SPS 使用固定增益，发送端通过 CompanionService 调整和读取 `STREAM_MUSIC` 档位。
已接管的会话检测到 Companion 掉线时保留当前内部增益和软件静音状态，向发送端回报
实际内部音量。后续按发送端滑块的百分比调整线性增益，最高为配置的增益。
若 Android 拒绝媒体流静音（例如 LX04），SPS 使用软件静音。

服务恢复后，回报 Android 媒体音量与当前内部增益的乘积。发送端下一次不同的调音请求
直接设置系统档位，成功后内部增益恢复默认值。查询与 AirPlay 2 音量事件使用同一回报值，
发送端回写相同值时保持当前增益。Android 媒体音量会影响使用同一音频流的应用。

音量端点为 `org.shairport.sync.android.volume.v1`，与状态桥接使用相同的 UID
信任边界。每次连接处理一个 8 字节请求与响应，均以网络字节序编码。
首字段为 `0x53505631`，次字段为有符号整数，范围 0 到 1000000 表示媒体音量。
请求的 −1 表示查询，响应的 −1 表示接口不可用。SPS 使用 200 ms 总时限，
活动同步会话每 250 ms 查询一次，AirPlay 通知使用独立线程。

## 页面行为

- 默认页面为 Kotlin/Compose `PlayerV2Activity`，应用图标、通知及新会话自动弹出均使用该页；
  旧 `PlaybackActivity` 保留为非导出页面。以 800×480 横屏为主要布局目标。
- 展示态为系统格式的当前时间、大尺寸圆角正方形封面及单行“歌曲名 · 艺术家名”；
  歌曲名纯白，艺术家为 60% 白色，超长文本连续滚动。四周至少留 24dp。
- 点击非按钮区域切换 Homeview 风格叠层控件的显示/隐藏：歌曲名、艺术家、只读进度及播放/暂停、上一首、下一首。
  首次点击只唤出控件，点击按钮不切换显隐；每次触摸或按钮操作续时，无操作 20 秒后隐藏，切换动画为 250ms。
  控制态封面保持屏幕垂直居中，控件区不加底部 padding。
  按钮触摸区域至少 48dp；无时长或无有效位置时不计算百分比。
- 背景使用封面缩略图的可分离高斯模糊，在后台仅按封面变化计算，兼容 API27；
  不依赖 API31 的 RenderEffect。无封面使用音乐符号占位。
- 控件仅展示现有协议支持的功能，不包含音量、进度拖动、歌词、队列和输出设备管理。
- 新会话首次播放时唤醒屏幕并全屏打开；暂停保留页面，换曲不重复打开。
- 停止、断开投放或本机连接异常后等待 **20 秒**。保留最后歌曲/封面、冻结进度并
  禁用控制；重复结束事件不延长截止时间。期间恢复活动会话则取消退出，新会话复用页面。
- 会话结束满 20 秒后，当前参与该会话的页面统一调用 `finish()`，不区分手动打开或自动弹出；后台监听服务继续运行。
- 已结束会话超时后重新手动打开的页面可等待新投放，不会被旧超时立即关闭；从未收到会话时也不启动会话退出计时。
- 会话结束退出与控件无操作 20 秒隐藏是两套独立计时；点击界面不会延长会话结束的退出期限。
- 返回键立即关闭，并抑制同一会话再次自动打开；通知和应用图标可以手动重新进入。
- 会话标记、手动关闭标记与退出截止时间保存在 App 私有偏好中；截止时间使用单调
  时钟，并按系统启动计数隔离重启。进程重建可恢复未被手动关闭的活动会话页面。
- 命令不立即改变本地播放状态。收到发送端状态变化才更新；不可用或发送失败会提示，
  5 秒内没有状态变化会提示未确认。实际遥控兼容性取决于发送端。

## 本机协议 v1

端点为 abstract Unix stream socket `org.shairport.sync.android.companion.v1`，
仅一个客户端。SPS 用 `SO_PEERCRED` 核验配置的 App UID，App 用
`LocalSocket.getPeerCredentials()` 核验 root 服务端。不开放 TCP，不传递配对凭据。
额外连接被关闭；此 UID 信任边界不防御已经拥有 root 的进程。

所有整数使用网络字节序。每帧头为三个 `uint32`：magic `0x53505331`（SPS1）、
消息类型、载荷字节数。magic 同时标识协议版本，未知版本断开。

| 类型 | 载荷 |
| --- | --- |
| 1，SPS → App | UTF-8 JSON 完整状态快照，每 500ms 一次，重连立即发送 |
| 2，SPS → App | `uint64` 封面代次，后接最多 4MiB 图片；仅新封面或重连时发送 |
| 3，App → SPS | 最多 1024 字节的 UTF-8 JSON 控制请求 |
| 4，SPS → App | UTF-8 JSON 请求结果 |

状态字段：`session`（进程实例与连接号组成）、`connection`、
`state`（idle/playing/paused/stopped）、`title`、`artist`、`album`、`device`、
`positionMs`、`durationMs`、`positionValid`（0/1）、`cover`（0 表示无封面）、
`controls`（play/pause/next/previous 分别对应位 0/1/4/5）。
发送端声明能力时按声明过滤；未声明时尝试基础 AP2 命令，不承诺支持。

请求字段：`id`、`session`、`command`（play/pause/next/previous）。
结果字段：`id`、`result`（dispatched/unavailable/failed）。dispatched 表示发送端
控制通道接受请求，不等于播放器已执行。执行前再次校验连接号，拒绝跨会话旧命令。

通信线程定期取最新快照，不积压历史元数据。元数据与音频线程不执行 App socket IO；
音频计时发布使用 try-lock，忙时跳过。ARM32 的帧计数通过锁保护快照传递。
App 命令队列上限 8 条，断线即清空，不在重连后重放旧命令。
消息读写有超时，SPS 的停止管道唤醒等待并 join，断线不向音频线程传播错误。
AP2 遥控的串行锁与 socket 等待设为 2 秒，失败的加密控制通道关闭，避免重试半包。

## 验证边界

结果见 `android/outputs/companion/`（不提交）：APK 构建、lint、原生 ELF/ABI 审计、
独立状态机检查、LX04 上使用生产桥接模块与合成数据的 UI/IPC 检查。
合成桥接夹具替代发送端和音频播放器，因此不证明真实 AirPlay 元数据完整性、
真实发送端遥控响应、长时音频稳定性或专用低权限 SELinux 域可用。
实际整条 AirPlay 链路仍需配合发送端单独验收。


### Compose Player V2（2026-11-06）

V2 已完成构建、lint 和 LX04 合成数据 UI/IPC 检查；28 项独立界面断言及两种布局截图见
`android/outputs/player-v2/VALIDATION.md`。测试的中断、系统面板影响和清理情况在该记录中单列。
该结果不替代真实发送端遥控或音频稳定性验收。原生服务未重启；没有提交或推送。


### R8 release 构建与部署

```sh
./android/gradlew -p android assembleRelease lintRelease
```

release 启用 R8 代码优化、混淆和资源缩减，使用默认 `proguard-android-optimize.txt`。
默认生成 `android/app/build/outputs/apk/release/app-release-unsigned.apk`，签名由部署环境提供；
混淆映射位于 `android/app/build/outputs/mapping/release/`，应与对应 APK 一起保留以供排错。
本次设备部署使用现有本机开发证书签名，保持覆盖安装和 UID，不代表正式发行签名。
部署验证和原生服务启动状态见 `android/outputs/player-v2-release/VALIDATION.md`。
