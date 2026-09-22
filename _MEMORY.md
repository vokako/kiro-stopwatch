# kiro-stopwatch

M5Stack StopWatch（C152，ESP32-S3 圆形 AMOLED）开发工作区：官方资料副本、PlatformIO 真机/Mac 双目标工程、桌面模拟优先的开发流。

## Why

- 开发流是「Mac 上 SDL 模拟改到满意，再烧真机」。同一份 setup()/loop() 靠 PlatformIO 的 native 与 stopwatch 两个环境分别编译，不写第二份 mock 代码；真机只验模拟不了的东西（亮度、音频、IMU 噪声、功耗、无线） · 2026-09-14
- 库不走 lib_deps 从网上拉，而是 lib_extra_dirs 指向 references/projects 里的本地克隆，让模拟器和真机编译同一版本库；M5GFX 本地副本带 patches/M5GFX-sdl-stopwatch-466.patch，刷新克隆后要重新 apply · 2026-09-14
- 屏幕几何一律写常量（圆心 233,233、可见半径 233），不从 M5.Display.width()/2 推算。原因：M5GFX 0.2.29 在真机上报 468x468（偶数对齐 + offset_x 6），模拟器报 466x466，用 width() 会差 1 px 并把外框压出可见圆右下边缘，2026-09-14 真机实测 · 2026-09-14
- swlink 采用主机侧按键映射（设备发事件，daemon 用 pynput 注键），不用 ESP32 直接枚举 USB HID：映射改动无需重刷固件，且同一条 CDC 链路能同时传传感器。代价是 daemon 必须常驻并获得 macOS 辅助功能权限 · 2026-09-14
- swlink 协议：JSON 一行一条为控制面，麦克风/PCM 这类大块数据用带 n 字段的 JSON 头行 + n 个原始字节，同一条流上跑；不引入二进制帧库，串口监视器里仍可读 · 2026-09-14
- 按键最终走 BLE HID（手表自己是键盘）而不是 daemon 注键：macOS 输入法等按 HID 层或按事件 sourcePID 过滤合成事件，合成键永远进不了它们。daemon 注键保留为无蓝牙时的后备；IMU 手势仍由 daemon 注键。映射表主本在电脑 TOML，daemon 每次连上/保存时用 keys.set 同步到手表 NVS · 2026-09-15
- 文档分四处，交接时按此顺序读：README.md（用户操作）→ AGENTS.md（会踩的坑）→ docs/design-docs/swlink-architecture.md（协议与架构全表 + 如何扩展）→ docs/unresolved.md（未验证与已知缺口）；docs/requirements/swlink.md 记录需求与完成状态。设计文档 2026-09-16 重写过一次，此前版本已过时（写着按键走主机注入、Wi-Fi/BLE 不在范围内） · 2026-09-16

## Memory

- 真机串口：M5.config().serial_baudrate 默认 0，M5.begin() 不会调 Serial.begin()，必须显式设 115200（该字段只在 Arduino 下存在，native 编译要用 #if defined(ARDUINO) 包住） · `grep -n serial_baudrate hello/src/main.cpp` · 2026-09-14
- 读取 USB CDC 输出必须拉起 DTR（pyserial dtr=True, rts=False），否则固件丢弃输出；RTS=1 且 DTR=0 会复位芯片，可用来抓启动日志。pio device monitor 无 TTY 时拒绝运行 · `grep -n 'dtr=True' README.md` · 2026-09-14
- 设备身份：Espressif USB-Serial/JTAG VID 0x303A PID 0x1001，ESP32-S3 rev v0.2，16 MB quad flash，8 MB PSRAM，MAC 由 esptool chip-id 读取（各机不同）；端口名随插拔变化，用 ls /dev/cu.usbmodem* 查 · `esptool --port /dev/cu.usbmodem31301 --no-stub chip-id` · 2026-09-14
- esptool 一次性读整块 16 MB flash 会报 Packet content transfer stopped；要 --no-stub 按 1 MiB 分块读并重试，脚本在 scripts/backup-factory.sh（尚未实际跑过一次完整备份） · `test -x scripts/backup-factory.sh` · 2026-09-14
- 真机首次编译约 4 分钟（下载 espressif32@6.12.0 工具链与 Arduino core 到 ~/.platformio），之后增量秒级；烧录约 20 秒；native 编译约 11 秒 · `ls ~/.platformio/packages | grep -c esp` · 2026-09-14
- M5Unified 本地克隆要求 M5GFX >= 0.2.29；官方 UserDemo 与 ochyai 模拟器分别固定 M5GFX 0.2.19 / 0.2.27，混用会出现 468 vs 466 这类行为差异 · `python3 -c "import json;print(json.load(open('references/projects/M5Unified/library.json'))['dependencies'])"` · 2026-09-14
- buddy/ 是 AMOLED-1.8 项目 app_buddy（ESP-IDF+LVGL）到 M5GFX 的移植：位图资产直接复用，绘制逻辑重写；USB 协议与原项目一致，原 buddy_bridge.py 可直接对接。真机 2026-09-14 验证 8 种情绪切换正常 · `uv run buddy/tools/buddy_send.py idle --port /dev/cu.usbmodem31301` · 2026-09-14
- linkfw 命名空间叫 swl，不能叫 link：host 编译时与 unistd.h 的 POSIX link() 冲突（2026-09-14 踩过） · `grep -c 'namespace swl' linkfw/src/link.hpp` · 2026-09-14
- 麦克风与扬声器共用 ES8311，M5Unified 里二者互斥：mic.start 前 M5.Speaker.end()，mic.stop 后 M5.Speaker.begin()；mic 流期间 tone/spk.pcm 返回 err。真机实测 16 kHz 每秒约 15 块×2048 字节 · `grep -n 'Speaker.end' linkfw/src/main.cpp` · 2026-09-14
- M5Unified 的 Mic_Class 在 native/SDL 构建里没有实现，链接会报 undefined symbol；mic 代码必须用 #if defined(ESP_PLATFORM) 包住 · `grep -c 'ESP_PLATFORM' linkfw/src/main.cpp` · 2026-09-14
- swlink daemon 监听 ws://127.0.0.1:9898，无鉴权，只绑回环；任何本机进程都能借它按键。link/ 用 uv 管理，uv run pytest 有 14 个无硬件单测 · `cd link && uv run pytest -q 2>&1 | tail -1` · 2026-09-14
- swlink daemon 在同一端口 9898 上既服务 WebSocket 也服务网页仪表盘（GET / 返回 link/swlink/ui/index.html，用 websockets 的 process_request 分流）；仪表盘显示设备状态、IMU 实时、触摸/按键、键位映射、执行器控制、麦克风录音回放和事件日志 · `curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:9898/ 应为 200（daemon 运行时）` · 2026-09-14
- StopWatch IMU 轴向（真机实测，用户 2026-09-14 校正）：ay=+1 左侧向下、ay=-1 右侧向下、ax=-1 竖直屏幕朝人、ax=+1 顶部向下、az=+1 屏幕朝上、az=-1 屏幕盖着。link/swlink/triggers.py 的 tilt 判定与仪表盘气泡都按此映射 · `grep -n 'ay > self.tilt_on' link/swlink/triggers.py` · 2026-09-14
- 虚拟麦克风走 BlackHole 回环声卡：link/swlink/audio.py 把手表 16 kHz 单声道重采样到设备采样率写入 BlackHole 输出，App 选 BlackHole 作输入；抖动缓冲目标 120 ms、上限 400 ms 丢旧数据。实测缓冲以约 20 ms/s 缓慢增长（手表 I2S 时钟略快于名义 16 kHz），靠上限自限，尚未做漂移校正 · `grep -n 'MAX_BUFFER_MS' link/swlink/audio.py` · 2026-09-14
- linkfw v0.2（settings.cpp NVS / net.cpp Wi-Fi+mDNS+TCP 9899+SNTP / 功耗分级）已烧录并验证 cfg 持久化与配对令牌；Wi-Fi 未用真实凭据测过，daemon 侧 TCP 传输与 mDNS 发现尚未实现 · `grep -c 'kTcpPort' linkfw/src/net.cpp` · 2026-09-14
- swlink 合成按键（pynput/CGEvent）只发给最前台应用；仪表盘 ▶ 测试会先聚焦页面上的落点输入框。daemon 从 KiroCrew shell 拉起时已有辅助功能权限（AXIsProcessTrusted=True，TextEdit 实测收到 abC）；用户从自己终端启动时需给该终端授权 · `grep -n 'keysink' link/swlink/ui/index.html` · 2026-09-14
- hold 动作的自动重复通过 pynput 子类 DarwinRepeatController 发送带 kCGKeyboardEventAutorepeat=1 的 key-down（Quartz 事件监听实测首个为 0、后续全为 1），这样应用层长按检测把它当一次持续按压。合成事件不经过 HID 层，Karabiner 等驱动级检测看不到，那需要虚拟 HID 或 USB HID 路线 · `grep -n 'kCGKeyboardEventAutorepeat' link/swlink/keymap.py` · 2026-09-15
- 单独发修饰键：系统会把 CGEventPost 的 key-down 转成 flagsChanged，左右靠键码区分（左 Option 58 / 右 61），事件 flags 里的设备左右位会被系统抹掉、无法注入。纯修饰键 tap 加 80 ms 按压时长（TAP_DWELL_S），否则'单独按 Option'类检测把瞬时按放当噪声过滤 · `grep -n 'TAP_DWELL_S' link/swlink/keymap.py` · 2026-09-15
- linkfw BLE：Bluedroid，HID 报告 ID1 键盘 8 字节 + ID2 消费者 16 位，Just Works 绑定（ESP_LE_AUTH_REQ_SC_BOND + IO_CAP_NONE），自定义服务 6e400001-…（RX 写 / TX 通知）跑 JSON 协议，mic.start 在 BLE 上被拒。Wi-Fi+BLE 同编时静态 RAM 27%，运行时未压测。禁用 BLE 需重启生效（Bluedroid 不能干净地 deinit 再 init） · `grep -n 'kConsumerReport' linkfw/src/ble.cpp` · 2026-09-15
- daemon 用 bleak 走 BLE 时，CoreBluetooth 需要运行 daemon 的 App 有蓝牙权限，否则报 'Bluetooth device is turned off'（即便蓝牙已开）；KiroCrew shell 上下文没有该权限，用户须从自己的终端起 daemon 并允许一次 · `grep -n 'turned off' link/swlink/ble.py` · 2026-09-15
- 2026-09-15 真机验证：设备以 BLE HID 键盘配对 macOS 成功（Just Works 无密码），按键由固件发 HID 报告，微信输入法的快捷键识别到了——证实合成按键路线对输入法无效、HID 路线有效 · `grep -n 'hidReady' linkfw/src/keymap.cpp` · 2026-09-15
- linkfw 默认画面是 Kiro 幽灵（face.cpp，从 buddy 移植，320x372 PSRAM sprite，idle 20 fps / sleep 5 fps）；映射表带 moods 子表（keys.set 一并下发，存 NVS 'moods'）：tap 类触发 1.5 s 后回基态，.down 触发按住期间保持；IMU 手势的表情由 daemon 转发 {"t":"trigger"} 让设备查表。2026-09-15 真机验证 mood.set 与表情切换 · `grep -n 'applyMood' linkfw/src/keymap.cpp` · 2026-09-15
- 烧录前必须先停 swlink daemon：daemon 会在设备重枚举时立刻抢占串口，esptool 报 'Device not configured'（2026-09-15 踩过） · `grep -c 'find_device_ports' link/swlink/daemon.py` · 2026-09-15
- 表情共 10 个：新增 listening（麦克风流期间自动显示，声波环 + 实时电平条，电平由固件按块 RMS 算）和 dizzy（反向旋转螺旋眼 + 摆动 + 环绕星，默认绑 imu.shake）。仪表盘用 link/swlink/ui/face.js 在浏览器重画同一套动画做预览（幽灵用路径画，不搬 393 KB 位图） · `grep -c 'DIZZY' linkfw/src/face.cpp` · 2026-09-16
- 仪表盘布局：映射表改成 CSS grid 的 .maprow（宽屏 6 列、<900px 折两列并显示字段名），卡片用 .wide/.span2 控制跨列；daemon 的 _http 现在也服务 ui/ 下的 .js/.css（限定在 UI 目录内）。表情按钮列表要在收到 _status 后重建，否则只显示页面内置的默认 8 个 · `grep -c 'maprow' link/swlink/ui/index.html` · 2026-09-16
- 点击仲裁：M5Unified 在最后一次松手约 hold 阈值后才判定次数，故 single/double/triple 互斥，长按只发 hold 不发 click；click 仅用于日志、不作触发器。因此 hold 动作应绑 <btn>.hold，绑 .down 会在双击时触发两次并与 single 叠加——仪表盘把 .down/.up 归到 Raw edges 分组并在同一按钮混用时告警 · `grep -n 'strcmp(ev, "hold")' linkfw/src/keymap.cpp` · 2026-09-16
- 网页保存映射时，只填表情不填按键的行曾被写进 [keys]（值是表情名）；已在 set_keys 过滤空动作，但历史配置里可能残留，检查 ~/.config/swlink/swlink.toml 的 [keys] 是否有表情名作为值 · `grep -c 'if str(v).strip()' link/swlink/daemon.py` · 2026-09-16
- Kiro 身子位图由 linkfw/tools/gen_kiro_body.py 生成（rsvg 2000px 栅格化 → LANCZOS 缩放 → 对 alpha 做 0.6px 高斯模糊拉宽抗锯齿过渡 → 强制中性灰避免 RGB565 量化出彩边）；当前 247x300、145 KiB flash，sprite 340x430、BY=46。眼睛用 face.cpp 的 fillEllipseAA（4x4 超采样混合进 sprite 缓冲），M5GFX 的 fillEllipse 是硬边不要用 · `grep -c 'fillEllipseAA' linkfw/src/face.cpp` · 2026-09-16
- CO5300 屏在 M5GFX 里固定 16 位 RGB565（Panel_CO5300 的 0x3A=0x55 与 _write_depth 都硬编码），无法开 18/24 位。抗锯齿过渡像素若太暗（灰度 <=24），AMOLED 自发光会把 R/G/B 子像素分别点亮而看成彩色小点，所以生成位图要给 alpha 设下限（gen_kiro_body.py --floor 40，默认 blur 0.35），边缘保留 0→56→240 两级即可 · `grep -n 'floor' linkfw/tools/gen_kiro_body.py | head -3` · 2026-09-16

## Children

<!-- mem:auto:begin  generated by `mem sync`, do not hand-edit -->
（尚无内容）
<!-- mem:auto:end -->
