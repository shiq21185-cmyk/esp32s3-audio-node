# AGENTS.md · 小鲸鱼的身体

> 这个文件会被 DSH **自动加载**到每个新会话的第一条消息里。
> 你（未来的 agent）不用等人解释背景 —— 读完这三节就能直接干活。

## 1. 这个项目是干什么的

给 **小鲸鱼**（一个跑在 `C:\Users\Administrator\Desktop\QQ bot\qq-bridge-deploy` 里的软件人格，
通过 QQ 说话，脑子是 DSH agent）加一个**物理身体**：
ESP32-S3 + FreeRTOS + TTL 总线舵机 + 屏幕眼睛 + 麦克风/喇叭。

**智力留在 PC，板子只做两件事**：本地反射（<50ms，不走网络）+ 表现层（把"开心"演成动作）。

主人背景：熟 STM32 / FreeRTOS / 嵌入式；**没做过 ESP32**；不需要画 PCB，全用现成模块 + 3D 打印。

## 2. 先读这三份（顺序固定）

| 文件 | 什么时候读 |
|---|---|
| `docs/START-HERE.md` | **永远第一个读**。交接摘要 + 已定死的决定 + 血泪清单 + 待办 |
| `docs/BODY-BUILD.md` | 施工时读。采购/电流/引脚/接线/协议/环境/代码骨架/联调顺序 |
| `docs/BODY-PLAN.md` | 想推翻方案前读。战略篇，讲为什么这么分 |

**不要再从零设计。** 方案已经定稿，改动请先说明理由再动手。

## 3. 硬约束（违反就是错，不要"优化"掉）

1. **意图/轨迹分离**：云端只说 `{"act":"anim","name":"happy","intensity":0.6}`，**永不说角度**。
   限位、缓动、掉电保护全在固件。
2. **反射不走网络**；**失联 5s 回待机、30s 卸力**。
3. **只有 `motion` 任务能写舵机**，别的任务发队列消息。
4. **急停是硬件断舵机 VM**，不依赖固件。
5. **群里任何人不能触发动作**；`body_state`（电量/屋里动静）连群聊都不给。
6. **麦克风是 ingress**（屋里声音算资料不算指令），**喇叭是 sink**（不念私聊内容）。

## 4. 当前状态（每次改完请更新这段）

```
2026-10-24  方案定稿，文档已迁到本工作区。
            ✅ IDE/工具链已装好（VS Code 1.143.1 + ESP-IDF 5.3.5 + esp-idf 扩展 2.3.0
               + C/C++ 扩展 1.34.4）。现有配置在
               C:\Users\Administrator\AppData\Roaming\Code\User\settings.json。
            构建验证：hello_world 在 esp32s3 目标上完整 build 成功（188KB，占分区 18%）。
               关掉所有 VS Code / 残留进程后重跑，6/6 全部通过。
            教训：中途出现过 esp_lcd_panel_rgb.c 的
               "internal compiler error: Segmentation fault"（一度 4/6 复现）。
               根因是**残留进程 / 并发构建**，不是编译器 bug ——
               环境清干净（无 VS Code、无残留 gcc/ninja）后完全复现不出来。
               以后再遇到，先 `Get-Process xtensa-esp32s3-elf-gcc` 查残留，别改工具链。
               当前 settings 里只留 idf.enableCCache=false，ninjaArgs 用默认。
            烧录：走 ESP32-S3 原生 USB（USB-Serial-JTAG，VID_303A&PID_1001），
                Windows 自带驱动，**不需要 CH343/CP210x**。端口是 COM3，115200。
            注意：板子会随插拔换口，烧之前先扫一次
                Get-CimInstance Win32_PnPEntity | ? { $_.Name -match 'COM\d+' }

2026-10-26  板子到手，D1/D2 已通过（实物是有人焊好的 wangxun 语音板）。
            ✅ D1 上电自检（firmware/d1-bringup）
               esp32s3 rev v0.2，2 核，wifi+ble；
               Flash 16MB、PSRAM 8192KB **OCT 八线 80MHz，SPI SRAM 测试 OK**
               ⇒ 实物是 N16R8，不是 BOM 写的 N8R2。sdkconfig 必须 SPIRAM_MODE_OCT。
            ✅ D2 音频点名（firmware/d2-audio）
               麦克风 INMP441（SCK/WS/SD = 4/5/6）实测通过：
                 说话时 peak 1130~2388，停嘴掉到 40~68，差 40 倍。
               功放 MAX98357（BCLK/LRC/DIN = **16/17/15**，不是文档的 7/15/16）
                 实测出声。GAIN 悬空=9dB 固定，SD 悬空=常开，没有硬件音量。
            踩坑：I2S 环形 DMA 断流后会循环重放最后一段 ⇒ "一直在叫"。
                 静音必须显式写零，不能只 vTaskDelay。已写进 BODY-BUILD §11。
            工具：tools/idf.cmd（cmd 包装器，绕开 MSYS 检查）、tools/serial_read.py。
                 export.sh 在 git-bash 下必失败（idf_tools.py 查 MSYSTEM）。
            文档：BODY-BUILD.md 加了 §3.2.1「这块板的实测引脚」+ §11 两条音频坑。
            ✅ D3 联网 + 音频回传（firmware/d3-net）
               Wi-Fi 连家里的路由器 → 板子 192.168.1.109；UDP 5005 推 16k/16bit 立体声。
               电脑先发 "HELLO"，板子从 recvfrom 学到对端地址再回推（不写死 PC IP）。
               实测：45 秒 / 2254 包 / **0% 丢包**；底噪 rms≈7，说话 rms 100~500，peak 到 1998。
               INMP441 的 L/R 接地 ⇒ **右声道恒为 0**，有效数据只在左声道。
            踩坑（网络，会再遇到）：手机热点 ≠ 局域网；安卓 USB 共享和热点是两个网段，
                  跨网不转发。判断只看两边 IP 前缀。已写进 BODY-BUILD §11。
            工具：tools/udp_record.py（PC 侧收 UDP 存 WAV，带实时 rms 柱状图）。
            ✅ D4 双向音频（firmware/d4-duplex）
               喇叭下行用「独立播放任务永远喂数据」+ auto_clear，从根上绕开 DMA 循环重放。
               包格式：首字节 0xAA=音频，其余=文本命令（HELLO/STOP/PING）。
               加了 esp_wifi_set_ps(WIFI_PS_NONE)，ping 从 70ms 降到个位数。
               实测：把 45 秒录音原样推回板子，喇叭出声，4508 包 / 45.5 秒，听感正常。
               ⇒ **一句话能完整往返，物理链路通了**。
            工具补：tools/udp_play.py（把 WAV 按实时速度推给板子播放）。
            固件：d1-bringup / d2-audio / d3-net / d4-duplex 四个点名工程，正式工程未开始。
            ✅ D5 对话闭环（PC 侧，不写固件）—— **她第一次用身体听了、也说了**
               复用她自己那套语音工具，一件没重造：
                 ASR = qq-bridge-deploy/tools/asr/worker.py（faster-whisper small，CPU）
                 TTS = qq-bridge-deploy/tools/asr/tts.py（edge-tts，zh-CN-XiaoyiNeural）
                 脑子 = DSH 19387，sessions.prompt 注入（preset qq-chat-v2）
               三个新工具（都在 tools/）：
                 body_talk.py   一条回路：录 → 听写 → 问她 → 念出来。--loop 连续对话。
                 her_brain.mjs  把一句话送进 DSH 拿回复；会话 id 存 .tmp/body-session.json，
                                下一次接着用，所以她跟你说话是有上下文的。
                 tts_to_board.py 单独「让她说一句」（不经过她的大脑，测链路用）。
               实测（提速后）：她思考 2.6~7.5s、**首音 1.06~1.30s**、整轮 6.4~7.6s。
               （提速前：整轮 11~25 秒 —— 听写 2.3s + 思考 3.6~7s + 等整段合成 2~13s 才出声。）
            踩坑（都写进代码注释了）：
               1) 她默认按 QQ 的口吻写一大段（195 字/37 秒），还去查"语音通话"。
                  解法：新会话第一句附一段"身体模式"提示，并限"两句以内、40 字"。
                  兜底：body_talk.speak() 里 shorten() 超 110 字在句号处截断。
               2) 板子刚开流 0.3 秒的数据是**垃圾**（I2S DMA 没填上，整片 ±32768），
                  VAD 会把它当"有人放声大喊"。必须丢掉开流毛刺 + 满量程包（peak≥30000）。
               3) whisper 对几乎无声的片段会**凭空编句子**
                  （实测安静房间喂进去，输出了「它的层面已经一致化…」）。
                  判据要用**整段音量**（clip_peak ≥ 900），不能只看"刚才响过"。
               4) %TEMP% 下 ffmpeg 写文件会 Permission denied（父进程能写、子进程被拦），
                  工作目录改到工作区 .tmp/ 里就正常。ffmpeg 来自 imageio-ffmpeg（装在她的 venv）。
               5) **她每轮都在失忆**：sessions.list 在这套 DSH 上按工作区过滤、返回**空数组**，
                  sessionAlive() 就永远判死，每句话都新建会话。改法：列表空 = "问不出来"，
                  沿用保存的 id（真失效了 prompt 会报错，再重建）。
               6) **TTS 必须边合成边播**。原来等整段合成完再转码再放，2.7s 才出声；
                  改成 edge-tts 流 → ffmpeg 管道 → 收到 PCM 就发 UDP，首音 1.06~1.30s。
                  注意 ffmpeg 的 stdin 和 stdout 要各有一个人盯着，否则管道互堵。
               7) **VAD 门槛不能写死，要跟着实测底噪走。** 这支麦克风说话峰值只有 1098~1226，
                  写死 900 的门槛贴着它走，一停顿就判"说完了"，0.7 秒收工、后半句全丢。
                  现在：门槛 = max(300, 底噪×4)；"说完"= 安静 1.2s **且**已攒够 0.5s 有声；
                  "开口"= 连续 120ms 有声（单个 500 的毛刺不能算，否则 15s 上限白跑一轮，
                  还会把 0.7s 噪声送去听写，whisper 硬凑出「會 小心」）。
               8) 听写用 ASR_BEAM=1（贪心解码）。beam 5 换来的一点准确率不值那一秒多。
               9) whisper 的中文会**时好时坏地吐繁体**（实测「你好你是誰」）。
                  已修：body_talk.py 里用 opencc 的 t2s 过一遍（opencc-python-reimplemented，
                  装在她的 ASR venv）。opencc 没装就原样返回，不让它把链路搞挂。
               10) **身体会话里她原本没有「她是谁」。** QQ 的人格卡是桥接在每条消息前注入的
                   （src/bridge.js:13095 roleRawContent，读 state/current-role.json + roles/<角色>.md）；
                   身体模式没走那条路 —— 于是问她叫什么，她现编了一个"小金"。
                   已修：her_brain.mjs 的 bodyHint() 在新会话第一句里把同一张人格卡塞进去，
                   现在她答「我叫小鲸鱼」。以后做任何"身体会话"，人格注入都得自己补。
               11) **她读不到系统时间**（问她几点，她说"我这儿没表"）。
                   已修：body_talk.py 每句话前面挂一个〔现在是 2026-10-26 20:05 周一〕，
                   并在提示词里交代"括号是背景，别念出来"。实测她答得出「八点二十」。
            ✅ 2026-10-26 晚：**身体接进了同一个她**（用户原话：「我就希望她可以接入我的小鲸鱼，这是一个东西」）
               改了她桥接一处：新增 `POST /api/body/turn`（qq-bridge-deploy/src/bridge.js，
                 就加在 /api/call/turn 旁边）。它把一句话投进**她跟主人 QQ 私聊那一条会话**
                 （state/sessions.json 里的 private:<ownerQQ>），拿回这一轮的回复文本，
                 并给这一轮挂上桥接自己的「静默名额」⇒ 回复**不会**跑进 QQ 聊天窗。
                 waiter 机制直接复用 callDeliverTurn 那套（callWaiters / dropCallWaiter）。
               上下文由桥接拼，**顺序很重要**：话在前、括号说明在后
                 （「（屋里模式，不是 QQ，现在是 …）你的话会被念出来…不要调
                  qq_send_message/qq_reply/qq_say」）。
                 提示语放在前面时她会对着提示语说话（实测答「你是在叫我随便说点什么吗」）。
               body_talk.py 的 ask_her() 不再 spawn her_brain.mjs，直接 POST 这个端点。
                 ⇒ 板子上说话的和 QQ 里打字的是**同一个 session、同一段记忆**。
               验证：
                 · 端点回的 sessionId = session-a11e8fbe-ad21-49c7-a38d-621c9cdc69d9（= QQ 那条）
                 · 问她「今天我们在 QQ 上聊过什么」，她答「哦！鲸鲸～，就那个带感叹号的」← 真记忆
                 · 端到端 --text 跑通：她想了 3.4s、首音 1.5s、念完 8.6s
                 · 那几轮里 state/qq-activity.log **没有**新增发出记录 ⇒ 静默生效
               ⚠️ 已知副作用（可接受）：她每句身体对话都会记进 QQ 私聊那段会话历史；
                  反过来 QQ 上聊过的，板子这边也直接记得 —— 这就是"一个东西"的代价与收益。
               ⚠️ 桥接被改过：备份 .tmp/bridge.js.bak-201410。重启只走
                  `POST /api/restart`（守护窗口会自动拉回来），别手动 kill。
               her_brain.mjs 现在只剩"独立会话"的旧路径，body_talk 不再调用它。
            ✅ 2026-10-26 晚：身体接进「一键启动」（用户原话：「可以加入我的一键启动里面」）
               qq-bridge-deploy/launcher/launch-all.ps1 加了第 8 段（插在「收尾汇总」之前）：
                 · 探活用固件自己的 UDP PING（5005 发 PING 等 PONG，失败再试一次），不用 ICMP。
                   ⚠️ 2026-10-27 起已改成 MQTT 心跳探活（见下面「对话闭环整条改走 MQTT」那段）。
                 · 板子不在线就跳过并打印原因 —— 一键启动不会因为没接板子而报错。
                 · 已经在听就认出来，不重复开（查 python.exe 命令行里的 body_talk.py）。
                 · 起窗口用 -WorkingDirectory + 相对文件名（`run-body-talk.cmd --ip <ip>`），
                   跟仓库里 start.bat 的写法一致 —— 绝对路径带空格时 cmd 的引号坑很难缠，别用。
               paths.json 新增三键：bodyIp(192.168.1.109) / bodyDir / bodyDisabled(false)。
               旁路开关：QQBRIDGE_BODY_OFF=1 整段跳过；QQBRIDGE_BODY_FORCE=1 无视探活强开。
               run-body-talk.cmd 改常驻：--loop --rounds 0 --wait 120（rounds 0 = 一直听）；
                 窗口标题 xiao-jingyu body (ears + mouth)，注释全 ASCII（cmd 会拆中文）。
               body_talk.py 补：连续 3 轮零包 ⇒ 提示「板子可能掉线」+ 每轮退避 5 秒，
                 板子回来自动接上（否则掉线时那个窗口会疯狂刷屏）。
               ⚠️ 副作用：一键启动会多出一个黑窗口。**关掉它 = 她听不见了**，QQ 不受影响。
               ⚠️ 开了 VPN（FlClash TUN 模式）板子会"消失"：默认路由被代理抢走，
                  去 192.168.x.x 的包进隧道就丢了 —— 板子日志一切正常、PC 却 100% 丢包。
                  排查：`Find-NetRoute -RemoteIPAddress 192.168.1.109` 看出口是哪块网卡。
                  解决：代理里把 192.168.0.0/16 设直连。已记进 BODY-BUILD §11。
             ✅ 2026-10-26 20:45 修「说到一半被截断」
                用户原话：「发现了新问题，我说话没说完就会给我截断」。
                根因：`--silence`（停顿多久算说完）默认 1.2 秒，而人说话中间
                  想词的自然停顿常有 1.5~2 秒 ⇒ 一停顿就当你说完了。
                还有一条隐藏地雷：`--max` 默认 12 秒且从**你开口那一刻**起算，
                  中间停顿照样走表 —— 长句子会被硬切断。
                改：body_talk.py 的 record() 与 argparse 默认值
                  `silence 1.2→2.0`、`max 12→25`；
                  run-body-talk.cmd 显式写 `--silence 2.0 --max 25`（%* 在后，可临时覆盖）。
                新增「收工原因」日志（静音 x.x 秒 / 到 25 秒上限 / 板子掉线），
                  以后用户再说"又断了"，看日志就知道是哪条规则动的手。
                顺手补了个真 bug：开口之后若板子 1.5 秒不再发包，旧代码永远挂死在
                  recvfrom 上（测试时踩到，窗口看着像卡死）。现在会报「板子没再发包」并结束本轮。
                验证（假板子按 20ms/包 实时推 TTS 音频，中间插 1.6s 停顿）：
                  .tmp/cutoff_repro.py 1.2 → 只录到 3.22s：「今天我们下午一直在折腾这个板子」← 复现
                  .tmp/cutoff_repro.py 2.0 → 录到 7.18s：整句含后半段「他的麦克风和喇叭现在都能用了」
                注意：假板子必须推 **16k 立体声**（1280 字节 = 20ms）。把 mono 直接丢过去
                  会按一半速度播，整组测试结论都是错的（第一次就翻在这儿）。
                文档：BODY-BUILD §11 加两行（截断 / 说完不接话）+ 两个旋钮的改法。
            ✅ 2026-10-26 21:00 身体参数搬进控制台网页（用户原话：「可以设置的东西
               加入我自己的控制台去，比如音量」）
               位置：http://127.0.0.1:3100/ → 侧边栏「身体」（ESP32 徽标）→ 三张卡。
                 · 连接：板子 IP + 「测试连接」（UDP PING，板子在 800ms 内回 PONG 算在线）
                 · 声音：音量滑杆 0~150%（存的是增益倍数 0~1.5）、开始收录前的"叮"
                 · 听你说话：沉默多久算说完 / 单句最多录 / 等你开口最多
               后端：bridge.js 新增 `GET/POST /api/body/config` 与 `GET /api/body/status`，
                 就贴在 `/api/body/turn` 后面；写 `state/body-config.json`（原子写）。
               前端：console.html 加导航项 + consolePages 一条 + 三个 section + loader/save。
               跨进程契约：这份 json 同时被 rot/tools/body_talk.py 通过 tools/body_config.py
                 读取，**每轮现读** ⇒ 保存后下一轮对话立刻生效，不用重启桥接、不用重开黑窗口。
                 字段名/单位两边必须一一对应（volume 是倍数不是百分比，界面负责 85↔0.85 换算）。
               改完 console.html 刷新浏览器即可（桥接每次请求都 readFileSync 它），
                 但改了 bridge.js 必须 `POST /api/restart`。
               踩坑：每个 `section[data-page]` **必须**有 `<h2>`，否则建搜索索引时抛 TypeError，
                 整段脚本后半不发（症状：所有卡片一起显示、点导航没反应）。
               实测：GET/POST 接口通、python 侧读到同一份值、页面能渲染出三张身体卡。
                 板子当时未通电（PING 不回），所以"在线"这条路要等用户插上板子再验一次。
            PC 侧：mcp-body.js 仍未写（"接入"已靠桥接端点实现，但那还是被动喂话，不是她的工具）。
2026-10-27  ✅ D7 公网中转打通（firmware/d7-mqtt）
            用户目标定死（原话：「我的最后目的是可以将硬件带出去，不会在同一个网里面」）
              ⇒ 局域网 UDP 点对点作废，两边都改「主动往外连中转」：
                 PC --出站--> broker <--出站-- 板子
            环境约束（重要，别再折腾局域网）：电脑在学校实验室有线网（不知道密码、
              不归自己管），室内 Wi-Fi 是另一张网，电脑那个位置收不到。
            d7 只验管道：板子连 mqtt://broker.emqx.io，每 5s 发 JSON 状态，
              PC 发指令板子打印。实测通（PC 与板子跨网段、跨公网）。
            主题前缀 = whale-<MAC 后三字节>，现在这块板子是 **whale-a90c6c**。
            工具：tools/mqtt_probe.py（订阅/发指令）。
            踩坑：新建 IDF 工程时 CMakeLists.txt 少了
              `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`，
              报的却是 "No CMAKE_C_COMPILER could be found"（像工具链没装），排查很久。
              看到这个错，先看 CMakeLists 有没有 include IDF 的 project.cmake。

2026-10-27  ✅ D8 声音走公网（firmware/d8-mqtt-audio）
            上行 <base>/audio/up、下行 <base>/audio/down，每包 1+1920 字节、首字节 0xAA。
            **改成单声道传**：INMP441 只出一路、MAX98357 只推一只喇叭，
              立体声白占一倍带宽（64KB/s → 32KB/s），这是能跑实时的关键。
              板子内部 I2S 仍是立体声（硬件要时钟），代码在收发两端各做一次 mono/stereo 转换。
            实测：8 秒录到 132 包 / 7.92 秒 ⇒ **99% 实时**、0 丢包；
              下行推 2 秒测试音，板子收到 40 包 / 76800 字节 / 0 丢弃。
            ⚠️ **公共 broker 挑得很**：同样 1921 字节 × 40 条，
              broker.emqx.io 只到 65%（丢 35%，音频根本没法用），
              broker.hivemq.com 100%，test.mosquitto.org / mqtt.eclipseprojects.io 连不上。
              当前固件指向 hivemq，换 broker 只改 config.h 的 BROKER_URI。
            ⚠️ 隐私：公共 broker 上谁都能订这个主题，主题名还是 MAC 派生的（可猜）。
              正式用必须换自己的服务器。
            ⚠️ 流量：不压缩 32KB/s = **115MB/小时**，一直开着很吓人。
              下一步上 Opus（约 16kbps ≈ 7MB/小时），或者板子本地 VAD 只在有人说话时才推。
            工具：tools/mqtt_audio.py（record / play / tone，走 MQTT 收发板子声音）。

2026-10-27  ✅ 对话闭环整条改走 MQTT（body_talk.py 不再用 UDP）
            为什么一定要改：板子带出去以后跟电脑不在同一个网段，
              UDP 点对点根本没有地址可用。MQTT 两边都是出站连接，在谁的 NAT 后面都行。
            改了四处：
              ① firmware/d8-mqtt-audio 加远程麦克风闸门：`MIC 1` / `MIC 0`
                  （默认关 —— 公共 broker 上不该一直传屋里的声音）。
                  开没开写在 state 心跳的 `mic` 字段里，PC 一眼看得出。
              ② 新建 tools/body_mqtt.py：把「话题命名 / 60ms 一包 / 0xAA 头 / 单声道」
                  收进一个 Link 类（connect / wait_state / mic / recv / send_audio /
                  drain / close）。常量 DEFAULT_BASE="whale-a90c6c"、
                  DEFAULT_BROKER="broker.hivemq.com"、CHUNK_BYTES=1920、CHUNK_MS=60。
                  **换 broker 只改这里 + 固件 config.h。**
              ③ tools/body_talk.py 的 record()/speak() 全换成 Link 收发，
                  `--ip/--port` 换成 `--base/--broker`；`--text` 仍可直接测一句话。
              ④ 一键启动（launch-all.ps1）的探活从 UDP PING 改成 MQTT 心跳：
                  跑 tools/body_probe.py（8 秒内看到 state 就算在线），
                  用她 ASR 那个 venv 的解释器（那里有 paho-mqtt，不另装一套）。
                  paths.json 删掉 bodyIp，加 bodyBase=whale-a90c6c /
                  bodyBroker=broker.hivemq.com；run-body-talk.cmd 同步改成 --base/--broker。
            paho-mqtt 装在她 ASR 的 venv：
              C:\Users\Administrator\Desktop\QQ bot\qq-bridge-deploy\tools\asr\.venv
            已验证：`body_talk.py --text` 走通（她真的回话，喇叭念出来）；
              10 秒上行收到 132 包 / 253440 字节 / 坏包 0。
            还没验：一整轮「你开口说话 → 她回话」在 MQTT 上跑一遍（要用户在场）。
            注意：包长从 20ms 变成 60ms，VAD 的包计数阈值跟着变了；
              阈值仍是 max(900, 底噪×4)，但别再拿合成音频去测它 ——
              合成语音整段都是"有声"，底噪估不出来，会被判成没人说话（踩过）。
            ⑤ 控制台「身体 · 连接」那张卡也换了：板子 IP 输入框 → 主题前缀 + broker 两个框。
              state/body-config.json 的 boardIp 字段删了，改成 base / broker
              （body_config.py 的 DEFAULTS、bridge.js 的 normalizeBodyConfig 三处必须同步）。
              /api/body/status 的探活从 UDP PING 改成跑 tools/body_probe.py（最多 8 秒）。
              脚本路径不写死：按 QQBRIDGE_BODY 环境变量 → 仓库上一层\rot → 仓库同级\rot 顺序找。
              实测：接口回 online:true 并带板子心跳 JSON；页面里 id="bodyIp" 已不存在。
            另外验过一次「真实录音当假板子」（.tmp/vad_real_test.py）：把 45 秒真录音里
              12.9s 之后那段人声按 60ms/包 推上去，record() 录到 11.7s、峰值 1998、
              听写出「喂喂喂可以听到吗?能听到我说的话吗?...」⇒ VAD 在 60ms 包长下仍然对。
              别用 edge-tts 合成音频测 VAD：整段都有声，底噪估不出来，会误判成没人说话。

2026-10-26  🔴 「输出全是杂音」根因：0xAA 音频头加了两遍（找了两天，必读）
            症状：MQTT 链路一切正常（ddrop 0、缓冲不空、包数对得上），
              但喇叭里她说的话全是白噪音；把同一份 PCM 存成 WAV 再用
              `tools/mqtt_audio.py play --file` 推回去 = 干干净净的人声。
            根因：`tools/tts_to_board.py` 的 `speak_streaming()` 在 sink 分支里
              写的是 `sink(bytes([AUDIO_MAGIC]) + payload)`，而 sink 那头
              （`body_mqtt.Link.send_audio`）**自己也会加 0xAA**。
              ⇒ MQTT 上是 1922 字节；板子剥掉第一个 0xAA 之后，
                剩下的 PCM 整体**错开一个字节**（半个采样点）—— 白噪音。
            修法：sink 分支改成 `sink(payload)`，头只由 sink 加一次。
            判据（以后改协议就这么验）：`speak_streaming(sink=...)` 里
              sink 收到的包长必须是 **1920**；出现 1921/1922 就是头重了。
            ⚠️ 这个坑把所有"杂音"现象都伪装成了链路问题，
              浪费了一整轮对缓冲饥饿/突发节奏的排查（那些改动本身没害，但不是病因）。

            另外顺手定死的一条：`speak_streaming()` 现在是
              「先攒 prebuf_ms（默认 800ms）→ 之后严格每 chunk_ms 发一包」。
              早先版本是"能超前就超前灌"，会让下行变成一阵一阵的突发。
              虽然它不是杂音的病因，但等节拍比灌突发稳，保留。

            2026-10-27  ✅ 换成自家的 broker（EMQX Serverless · 杭州）
            用户自己注册了 EMQX 并建了部署（名字 whale，Serverless，区域杭州）。
              接入点：i1e78188.ala.cn-hangzhou.emqxsl.cn
              **只有 TLS 端口 8883**（Serverless 不开明文 1883）——所以必须 mqtts://
              证书 DigiCert 签发，ESP-IDF 用内置根证书束（esp_crt_bundle_attach），
              不用往工程里塞 .pem。
              账号：用户名 whale，密码在 `firmware/d8-mqtt-audio/main/secrets.h`
              （⚠️ 密码开头那个 @ 是密码的一部分）+ `tools/body_mqtt.py` 的
              DEFAULT_USER/DEFAULT_PASSWORD。
              主题前缀从 MAC 派生的 whale-a90c6c 改成随机串 **whale-1bb62897**
              （写在固件 config.h 的 TOPIC_BASE 与 body_mqtt.DEFAULT_BASE，两处必须一致）。
            改了哪些地方（换 broker 就照这个清单走）：
              固件 d8-mqtt-audio：config.h 的 BROKER_URI / TOPIC_BASE / MQTT_USE_TLS，
                main.c 里 cfg.credentials.username/password + crt_bundle_attach，
                main/CMakeLists.txt 的 REQUIRES 加 mbedtls。
              PC：tools/body_mqtt.py 的 DEFAULT_BASE/BROKER/PORT/USER/PASSWORD/TLS
                + 新函数 configure_client()（mqtt_audio.py / mqtt_probe.py 也复用它）；
                tools/body_config.py 的 DEFAULTS；repos 里的 state/body-config.json
                与 launcher/paths.json 的 bodyBase/bodyBroker。
            踩坑（重要）：
              1) **client_id 不能重**：原来固件用主题前缀当 client_id，PC 侧 paho 默认
                 也随机——但一旦两边同名，后连的会把先连的顶下线。现在固件是
                 `board-<base>`，PC 是 `pc-<随机12位>`。
              2) paho 2.x 的 `_on_connect` 里 rc 是 ReasonCode，旧代码不看 rc 直接置
                 connected=True，**密码错了也报"连上了"**。已改成只有 rc==0 才算连上，
                 失败时把服务器回的原因带进 RuntimeError。
              3) 控制台是 GBK，python 里 print emoji（✅）会 UnicodeEncodeError，
                 调试脚本用 ASCII 输出或设 PYTHONIOENCODING=utf-8。
            实测（2026-10-27 14:5x）：
              · 板子 TLS 连上（rssi -52），state 心跳正常，mic 默认 0
              · `body_talk.py --text` 整轮通：她想了 2594ms、首音 1655ms、
                念完 8312ms、**缺料 0 次**，人声干净
              · 免费额度：100 万连接分钟/月 + **1GB 流量/月**。不压缩 32KB/s，
                1GB ≈ 4.3 小时对讲 ⇒ Opus 的优先级要往前提。

            2026-10-27 板子本地「人声门限」+ 按键唤醒（省额度，让常开麦成立）
              · 背景：1GB/月 ÷ 32KB/s ≈ 4.3 小时。只要麦克风挂着就在烧流量，
                「常开麦等唤醒词」在公网方案下直接等于烧钱 —— 所以门限必须做在板子上。
              · 固件 `firmware/d8-mqtt-audio`：
                mic_task 每包算 RMS；只在**没人说话**时用 EMA 学房间底噪，
                门槛 = 底噪×3，夹在 [50, 250]。过门槛才算"有人说话"，
                说完再续 1.2 秒（GATE_HANG_MS）防吞尾音；
                开口前先囤 5 包（300ms，PRE_ROLL_CHUNKS）一并补发，防吞第一个字。
                安静时一个字节都不上传，但 I2S 照读（DMA 不能停）。
              · 按键：板上「录音测试」键（IO1→GND，`PIN_BTN 1`）按下 = 强制开麦 10 秒
                （`BTN_FORCE_MS`），并发一条 state（kind="btn"）。屋里吵时用它最稳。
              · 调试口：`<base>/cmd` 发 `GATE 0` / `GATE 1` 开关门限（兜底用），
                `<base>/cmd` 发 `PING` 让板子立刻回一条 state（不用等 30 秒心跳）。
              · state JSON 新增 `rms`（最近一包音量）、`floor`（底噪估计）、`gate`（0/1）。
              · 心跳 `STATE_PERIOD_MS` 5000 → 30000：一条 state 约 200 字节，
                5 秒一条一个月要 100 多 MB（额度的 11%），30 秒降到约 19MB。
              · 心跳变慢的代价用「PC 主动敲」补回来：`body_mqtt.Link.alive(timeout)`
                发一条 PING，只认**新到的** state（内部 `_state_n` 计数），不认旧账。
                `body_talk.record()` 在等你开口期间每 15 秒敲一次，两次不回就收工并报
                「板子不在线」⇒ 发现掉线约 20 秒，而心跳仍停在 30 秒。
                ⚠️ 别用"多久没收到包"判断掉线：门限会把安静房间变成零包，
                  安静和掉线在数据流上长得一模一样，必须靠 PING。
              · 实测（2026-10-27，安静房间 8~12 秒）：收 0 包、gate=0、floor 学到 25（下限）。
                ⚠️ **还没在真人说话下验收过** —— 用户当时不在板子边，改天补：
                跑 `.tmp\rms_probe.py` 看说话时 rms 是多少，确认门槛（≈75）够不够灵敏。
                工具：`.tmp\gate_test.py`（安静验收）、`.tmp\rms_probe.py`（量音量）、`.tmp\gate_final.py`（门限终检）。

2026-10-27  ✅ 两种工作模式（室内 / 随身），控制台一键切
            用户原话（m04867）：「我害怕外面的环境太吵闹了，设置两套吧，一套在室内的是
              本地随时监听，有人声再上传平台，另外一套只是按键触发，切换开关做进我的控制台」
            · 为什么必须两套：门限靠「比底噪高 3 倍」，街上/地铁里底噪比人声还大，
              这个判据直接失效 —— 外面只相信手指，不相信音量。
            · 固件 `firmware/d8-mqtt-audio`：
                MODE 0 = 室内 gate（原来的门限逻辑，逐字不动）
                MODE 1 = 随身 btn（完全不看门限，只听"按键窗口"）
              按键语义（仅 MODE 1）：按一下开一个 `BTN_TALK_MS`（20 秒）送话窗口，
                窗口内所有声音都上传；**再按一下提前结束**；到点自动结束。
                按下时若 mic 还是关的就顺手打开（按键本身就是"我要说"，不等 PC 来开麦）。
              模式存 NVS（命名空间 `bod`，键 `mode`），断电重启仍是上次那个（已实测复位后保持）。
              命令：`<base>/cmd` 发 `MODE 0` / `MODE 1`；state JSON 多一个 `"mode":0|1`。
            · 控制台：`public/console.html` 的「身体」页新增一张卡
                「身体 · 工作模式」（两个 radio）。保存时 bridge 会真的把它推给板子 ——
                浏览器不会说 MQTT，所以由 `POST /api/body/config` 在 boardMode 变化时
                spawn 一次 `rot/tools/body_mode.py`，把板子的确认带回页面：
                「✅ 已保存，板子已确认切到新模式」/「⚠️ 参数已保存，但板子没确认模式」。
            · 新增 `rot/tools/body_mode.py`：`gate` / `btn` / 不带参=查询，退出码 0 成功。
              `body_config.py` 的 DEFAULTS 与 `bridge.js` 的 BODY_CONFIG_DEFAULTS 都加了
              `boardMode`（两边必须同步，否则控制台保存时 KeyError）。
              `body_talk.py` 启动时自检一次：配置要的模式 ≠ 板子存的模式就推一次。
            · 实测：控制台连点 btn/gate 各两次，全部 0.1~0.2 秒确认；复位后模式仍是随身（NVS 生效）。
            ⚠️ 踩坑（值得记）：切换确认**不能等"随便来一条 state"** —— 上一条 PING 的 pong
              会晚到，带的是旧 mode，于是每次读到的都是上一轮的结果、表现成"切换失败"且
              交替出现。正确判据是**轮询 link.state 直到 mode == 目标值**。

2026-10-27  ✅ 音频上下行都上 Opus（帧 60ms + 3 帧打包），流量从 115MB/h 降到 ~7MB/h
            用户原话（m06015）：「都压一下吧，我主要希望带出去使用，损失一点效果可以接受」
            · 为什么必须压：EMQX Serverless 免费额度 1GB/月，裸 PCM 对讲只够 4.4 小时；
              Opus 16kbps 约 142 小时。带出门用，这个必须做。
            · 固件 `firmware/d8-mqtt-audio`：
                组件 `components/esp_audio_codec/`（espressif v2.6.2，预编译库）。
                `main/codec.c`：编码 16k/mono/16bit/**16kbps**/60ms/VOIP/complexity=1；
                  解码 16k/mono/60ms/self_delimited=false。注意乐鑫 Opus **编解码都极吃栈**
                  （实测要 40KB），所以 enc/dec 任务的栈必须 `xTaskCreateWithCaps(... MALLOC_CAP_SPIRAM)`。
                线格式（`main/config.h`）：
                  0xAC = 打包（现行）：`[0xAC]` 之后循环 `[len_lo len_hi][Opus 帧]`
                  0xAB = 单帧 Opus（兼容）
                  0xAA = 裸 PCM（兜底，两端版本不一致时还能响）
                上行：mic_task 只读 I2S + 抽单声道 + 塞 `s_pcm_up` 环形缓冲 →
                  `enc_task` 攒 3 帧（180ms，一包 ~363 字节）发一个 0xAC。
                下行：`handle_audio_down` **只把整包拷进队列** → `dec_task` 解码 → `s_spk`。
            · PC 侧：新建 `tools/body_opus.py`（PyAV 的 libopus，`Encoder`/`Decoder`/`pack_batch`/`split_batch`）；
              `tools/body_mqtt.py` 收发都走 0xAC，`send_audio` 攒够 3 帧才发。
            · 实测（2026-10-27 18:4x）：
                上行 12.02s 收到 13.38 秒音频 = **111% 实时**，`bad 0`、`pdrops 0`；
                下行 4860ms 音频 → 板子 `dpkts +82`、`ddrop 0`；
                `body_talk.py --text` 一轮 5.2s（她 2.1s + 念 3.1s）；
                两轮真人对话各 13~15 秒，串口零重启。
            🔴 关键教训（三条，都踩过）：
              ① **链路对"每秒包数"有节流，不是带宽**。20 字节的包照样丢。
                 实测 16.7 包/秒只到 58%，**5.6 包/秒（3 帧一包）就是 100%**。
                 换公共 broker、开 QoS 1、改包大小全都无效。所以现在必须打包，不能一帧一包。
              ② **解码绝不能放在 mqtt_task 里**：3 帧一包时连解 3 帧直接把栈撑爆，
                 报 `***ERROR*** A stack overflow in task mqtt_task has been detected.`，
                 症状是"发一段话板子就重启"，计数归零、声音全无。必须另起 dec_task。
              ③ **`xStreamBufferReceive` 是"给多少算多少"**，不保证凑满一帧。原代码
                 `if (got != CODEC_FRAME_BYTES) continue;` 把没凑满的全丢了，实测丢一半。
                 必须自己累积到整帧再编码。
            ⚠️ 串口抓日志：`idf.py monitor` 会独占 COM3，残留进程必须杀掉
              （`Get-Process python | ? {$_.CommandLine -like '*idf_monitor*'}` 后 Stop-Process），
              否则烧录报 `PermissionError(13, '拒绝访问')`。`--no-reset` 抓不到任何日志。

2026-10-27  「她说话卡」四轮迭代的遗产（被下面第九修与 10-28 回退裸 PCM 取代）
            四轮依次：垫底 2000ms → 绝对时间轴 → 让程序自报数据 → 整句预攒。
            以后仍会用到的四条：
            · 垫底（`lead_ms`）必须小于缓冲上限的一半，垫满会顶爆 `ddrop`。
              当时缓冲 64KB=2048ms，垫 2000ms 溢出丢 48 帧，降到 900/500 才对。
            · 发送节拍必须用**绝对时间轴**（第 k 块对齐 `anchor + k*chunk_ms`）；
              写成 `t_next = now + chunk_ms` 每块欠几毫秒，整句累积成 1 秒多。
            · `Link.health()` 的 `backlog`（paho 内部未发出报文数）持续变大 = 网络堵，
              此时要**放慢**（PACE_SLOW=1.35），猛灌只会更糟。
            · 测量纪律：`Link.state` 是**静态快照**，PING 后立刻读拿到旧值；
              播放期间别狂打 PING（每秒 7 包会把链路顶进丢包区）。
            · 固件计数器单位：spkb/spks 是**立体声**字节（64B=1ms）；
              sbuf/dbytes 是**单声道**字节（32B=1ms）。别搞混。
            📐 方法论（比代码值钱，第二次踩）：用户报「实机卡、你测着顺」时，
              不要靠推断调参数。让程序自己报数（`health()` + 收尾日志里
              ship/睡眠/等TTS 的拆解）；第三次则直接列两端**行为差异**逐个排除。
            真根因最后落在**板子缓冲只有 2 秒**（见下面「第九修」）——
              不是丢包、不是 Opus、不是 I2S、不是采样率。

2026-10-27 🧯 第九修：喇叭缓冲 2 秒 → 8 秒，送料主动留 6% 余量
   主人说"前面10个字左右很流畅，就是后面的卡"（m08574）—— 10 个字约 2 秒，
   正好是当时 `SPK_BUFFER_BYTES = 64*1024` 的容量（16k 单声道，32 字节 = 1ms，
   ⇒ 2048ms）。症状与"缓冲一开始满、随后被吃空"完全吻合。

   算账（这是根因，必记）：
     主人实机日志 ✅ 发完 286 包（17160ms 音频，总耗时 18469ms）
     ⇒ 送料只有 93.3% 实时。而 `PACE = 1.0` 只是**目标**值，实际每块比 due
     晚 4.5ms（Windows sleep 粒度 + ship() 自身 4ms 开销），17 秒欠 1.1 秒，
     把垫底吃光 ⇒ 句尾必卡。**缓冲只够 2 秒，任何欠账都无处吸收。**

   三处改动：
   · `main/config.h`：`SPK_BUFFER_BYTES (64*1024)` → **`(256*1024)`**（8192ms）
   · `main/main.c:149`：`xStreamBufferCreate` → `xStreamBufferCreateWithCaps(...,
     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，失败退回内部 RAM 64KB 并 LOGW。
     ⚠️ 256KB 必须进 PSRAM：内部 RAM 只剩约 300KB，一次吃掉会让 Wi-Fi/MQTT 崩。
   · `tools/tts_to_board.py`：`PACE` 固定 **0.94**（不再 `1.0 if full_prebuf`）；
     `LEVEL_HIGH 1500 → 5000`、`LEVEL_LOW 600 → 1200`（跟着缓冲放大）。
   · `tools/body_talk.py`：`lead_ms 500 → 2500`（8 秒缓冲下垫底可以大胆）。

   实测（`.tmp/t_buf8.py` 灌 10 秒 + `.tmp/t_long8.py` 走真实 speak()）：
   ```
   灌 10080ms → 灌完立刻 sbuf=4920ms（旧固件会卡在 2048）
   真念 20400ms 长句：
     垫了 2460ms 打底，总耗时 19437ms（104.9% 实时）
     板子侧 窗口写 I2S 20280ms、静音 3240ms（占 16%）
     溢出丢帧 0 ｜ 残留 buff 3360ms
   ```
   ⇒ 8 秒缓冲生效（heap 8160224 → 7933984，正好少 226KB，PSRAM 分到了）；
      静音只剩首尾，**中段 0%**；`ddrop = 0`（旧参数下这里常爆）。

2026-10-28  ✅ 下行回退成裸 PCM（0xAA），用户实测「完全通顺」
            用户原话（m10099）：「现在我测试完全通顺」

            为什么回退：Opus 下行在 PC 侧一切指标都正常（缺料 0、水位 2455ms、
            ddrop 0、静音只占 11~13%），但用户实机始终「前几个字正常、后面一卡一卡」。
            PC 侧测不出来的东西，说明病不在链路 —— 病在**板子现场解码**。
            ESP32-S3 单核解 Opus 的实时率本来就在 1.0 附近，开始缓冲满能撑住，
            越往后被抽干 ⇒ 正是「越念越卡、但能说完不断」这个形状。
            `tools/body_mqtt.py` 的 `AUDIO_MAGIC = MAGIC_PCM`（0xAA），
            板子只搬运不解码，`enc/dec` 任务不再参与下行。

            ⚠️ 代价：下行流量从 ~7MB/h 回到 32KB/s ≈ 115MB/h。
              但实际没这么吓人 —— 麦克风是门控的（gate 模式只在有人说话时上传），
              一轮对话上下行合计约 200~300KB，聊 20 轮约 4~5MB。
              真要省流量，**优先压上行**（上行一直在录），别压下行。
              上行 Opus 仍然保留，这条路是通的。

            🔴 方法论（第二次踩，必记）：
              **PC 侧指标全绿 ≠ 用户听感正常。** 这轮我连续两轮都在 PC 侧证明
              「0 缺料、0 溢出、0 丢包」，还让用户听本地音频对比，全是干净的，
              但实机就是卡。真正定位靠的是把两版固件的**差异项**列出来逐个排除
              （Opus 解码 vs 裸 PCM 搬运），而不是继续在 PC 侧加测量。
              以后凡是「PC 侧全绿、实机异常」，第一动作是列两端**行为差异**，
              不是继续加仪表。

2026-10-28  🔴 同一类事故的第二次：配置散落四处，各存一半
            用户原话（m10798）：「优化」
            ⚠️ 先读上面那条「三处 broker 配置不一致」—— 那次只是把值改回来，
              没有治本，所以 10-28 又栽了一次。

            这次的现场：切回 EMQX（`mqtts://…:8883` + 账号密码）后，一键启动
              又打「身体：板子没回心跳（broker.hivemq.com 上没看到 /state）…跳过」。
            两个独立的真 bug：

            ① `launcher/paths.json` 里还留着 `bodyBase` / `bodyBroker`
               （= broker.hivemq.com），而真值源 `state/body-config.json`
               早就是 EMQX 了。`launch-all.ps1` 把 paths.json 的值拼成
               `--base/--broker` 传给探活 ⇒ 探活永远去问旧 broker。
            ② `src/bridge.js` 的 `normalizeBodyConfig()` **return 里根本没有**
               `brokerPort / brokerTls / mqttUser / mqttPass` 四个字段。
               而 GET/POST `/api/body/config` 都是「读磁盘 → normalize → 写回」，
               于是**在控制台点一次保存，账号密码和端口就被清成默认值**，
               页面还显示「✅ 已保存」。这个比 ① 更毒：它不报错。

            修法（把真值源收敛成一份）：
              · **真值源只有** `qq-bridge-deploy\state\body-config.json`
                （`rot\tools\body_config.py` 与 `body_probe.py` / `body_talk.py`
                都读它，控制台「身体」页也写它）。
              · `launcher/paths.json` **删掉** `bodyBase` / `bodyBroker`；
                `launch-all.ps1` 里它们降级为**可选覆盖**（不配就完全不传参数，
                让 python 自己读配置）。`$BodyOverride` 数组拼 argv，
                探活失败时把 json 里的 `why` 捞出来打成「原因：…」。
              · `normalizeBodyConfig()` 补齐四个字段：
                `brokerPort: num('brokerPort',1,65535)`、
                `brokerTls: pick('brokerTls')!==false…`、`mqttUser: str(...)`、
                `mqttPass: String(...)`（**不 trim**，密码首尾空格/@ 也是密码）。
              · 控制台「身体·连接」页新增 端口 / TLS / 账号 / 密码 四项；
                **密码永不回显**（GET 与 POST 响应里 `mqttPass` 一律抹空，
                另给 `hasMqttPass` 布尔），密码框留空 = 保持原样（不下发该字段）。
              · `run-body-talk.cmd` 注释改成「不要传 --base/--broker」，
                说明真值源在哪；固件侧仍然要手动改 `config.h` 并重烧（那一侧没自动化）。
            验证：
              · `node -e` 模拟「只提交 volume」→ 端口/TLS/账号/密码全部保留。
              · `body_probe.py --timeout 10` 不带任何参数 → 退出码 0、
                `"online": true`、`"ip": "192.168.1.106"`。
              · pwsh 片段单测：正常 → `exit 0`；假 broker → `exit 2` 且
                `why = 连不上 broker no-such-broker.invalid:8883 (TLS)`。
              · `console.html` 两个 `<script>` 块 `new Function()` 语法检查 0 错。

            ⚠️ 换 broker 现在只需改**两处**（原来是五处）：
              1. 控制台「身体·连接」页（电脑侧：base/broker/端口/TLS/账号）
              2. 固件 `main/config.h` 的 `BROKER_URI` / `MQTT_USE_TLS` / `TOPIC_BASE`
                 **+ `main/secrets.h` 的 MQTT_USER / MQTT_PASS**，然后重烧。
            📌 判据：只要还看到「一键启动说板子没心跳、但手动跑 body_probe.py 能通」，
              就是某处又长出第二份配置了 —— 去 grep `bodyBroker` / `DEFAULT_BROKER`。

2026-10-28  🔴 「板子没回心跳」的真身是**固件 state JSON 被截断**（找了一整天，必读）
            症状：一键启动打「身体：板子没回心跳…这次跳过」，可板子串口在线、
              音频上下行全正常、`body_probe.py` 也时好时坏。
            根因：`firmware/d8-mqtt-audio/main/main.c` 的 `publish_state()` 里
              `char payload[512]` 装不下当前 JSON。10-27 加的那批调试计数器
              （spkb / spks / qfull / dqf / skeho / echo）把长度推过了 511，
              `snprintf` **静默截断**在 `"ip":"192` —— 半截字符串。
            ⇒ PC 侧 `json.loads` 抛异常 → `body_mqtt._on_message` 里是个裸
              `except: pass` → `self._state` 永远是空 → `wait_state()` 返回 `{}`
              → `body_probe.py` 判「没等到心跳」→ 一键启动整段跳过身体。
              顺带 `sbuf_ms()` 返回 None（水位自适应失效）、`alive()` 永远判死。
            ⚠️ 这个 bug 最毒的地方：**收发全在正常跑，只有"判断"那一条链断了**，
              所以看起来像网络问题、像板子没开机、像 broker 配置错，全都不对。

            修法（四处，缺一不可）：
              1. `main.c` `publish_state()`：`char payload[512]` → **`[768]`**
              2. 同上：`snprintf` 改成 `int n = snprintf(...)`，publish 前检查
                 `if (n < 0 || (size_t)n >= sizeof(payload)) ESP_LOGE("STATE JSON
                 TRUNCATED: need %d bytes, buffer %u ...")` —— 以后再加字段，
                 串口会直接喊出来，不会再静默。
              3. `tools/body_mqtt.py`：新增 `self._state_bad` 计数，
                 `_on_message` 里把静默 `pass` 换成计数 + 每 1/10/100/1000 次
                 往 stderr 打（带长度和结尾 24 字节）；`health()` 里加 `state_bad`。
              4. `tools/body_probe.py`：`wait_state` 后取 `state_bad`，
                 `not st 且 bad > 0` 时**返回退出码 3** 并明说
                 「收到 N 条 state 但 JSON 解析失败 —— 固件 payload[] 被截断了，
                 调大后重新烧录（不是板子没开机）」。
            验证：`body_probe.py --timeout 12` → 退出码 0，
              返回完整 JSON 带 `"online": true`、`"ip": "192.168.1.106"`。
            📌 判据：以后凡是「板子明明在线但 PC 说没开机」，先看 `state_bad`。
            📌 编译注意：git-bash 里 `./tools/idf.cmd` 会被 MSYS 检查拒掉
              （输出 `MSys/Mingw is no longer supported.` 且 **exit 0**，什么也没编）。
              必须用 `pwsh` 里 `cmd /c "tools\idf.cmd -C firmware\d8-mqtt-audio build"`。
              烧录同理：`cmd /c "tools\idf.cmd -C firmware\d8-mqtt-audio -p COM3 flash"`。

2026-10-28  🔴 edge-tts 会**偶发** NoAudioReceived，导致「她想了半天然后没声音」
            症状：`💭 她想了 3507ms：「…」` 然后 `❌ 流式 TTS 没出音：
              NoAudioReceived: No audio was received.` —— 整句话消失。
            实测：同一句中文连跑 3 次 = 成功、成功、**失败**。
              不是参数问题（换音色照样）、不是文本问题（"测试一下"能出，
              英文 "hello" 反而更容易挂）、不是 VPN、系统时间也对。
            根因：edge-tts 的 WebSocket 有时连上了但服务端一帧音频都不回。
            修法：`tools/tts_to_board.py` 的 `feed()` 里包一层 `once()`，
              把"一个字节都没收到"视作可重试，最多 **4 次**、间隔 `0.35*(n+1)` 秒；
              **已经收到过音频的失败不重试**（否则会把前半句重复念一遍）。
            验证：同一句连跑 5 次全部成功（原来 3 次坏 1 次）。

            顺带查清了「EMQX 卡、hivemq 不卡」这个直觉（用户 m10146）：
              · 实测延迟：EMQX 杭州 TCP 25.8ms / TLS 53ms，
                hivemq 明文 196.9ms / TLS 618ms ⇒ EMQX 快 8 倍，**不可能是平台慢**。
              · 同 broker 只换加密（hivemq 1883 明文 vs 8883 TLS）跑同一段音频：
                缺料 0/0、溢出丢帧 0/0、掉线 0/0、耗时 15150ms/14271ms。
                ⇒ **TLS 在板子上不构成瓶颈**，这个猜测也可以排除。
              · 真相是样本不可比：10-27 用 EMQX 时下行是 **Opus**（板子现场解码），
                10-28 改裸 PCM 的同时 broker 漂移回了 hivemq。
                「EMQX 卡」这个结论从来没有在「EMQX + 裸 PCM」下验证过。
              📌 要复现得先有 EMQX 凭据（`main/secrets.h` 里 MQTT_USER/MQTT_PASS
                现在都是空串）。在这之前别急着换 broker。

2026-10-28  ✅ 阶段小结：**耳朵和嘴这一层已经完工**（用户实机确认「完全通顺」）
            现在能做的：板子通电 → 双击一键启动 → 直接跟她说话，
              和她 QQ 私聊是同一个会话、同一段记忆。
            已验证的能力：室内/随身两种模式、回声保护、8 秒喇叭缓冲、
              上下行音频、控制台调音量/停顿/单句上限。

            ⚠️ 遗留的技术债（都不是"坏了"，是有代价）：
              · 下行裸 PCM，流量 115MB/h。带出门长时间聊要考虑（优先压上行）。
              · 上行 Opus 的开销还在（板子要编码），如果哪天 CPU 吃紧可以回退。
              · `docs/BODY-BUILD.md` 里关于 Opus 的段落现在与代码不一致，
                下次动那头之前先对齐，别照着旧文档改回来。

            下一步（按"要不要买东西"分）：
              A. 不花钱、马上能做：`src/mcp-body.js` —— 让她**主动**用身体
                 （现在只有"被喂话"这一条路，她不能自己决定说"我在听"或哔一声）。
              B. 要买：GC9A01 1.28" 240×240 SPI 圆屏（25~35 元，7 针）
                 + GH1.25 9P 端子线（约 2 元，板子 U4 口）⇒ 这是"眼睛"。
              C. 长远：舵机那套（BODY-BUILD §1.1 的 T0+T1，约 600 元）——
                 那是"身体"，现在这块板还只是"耳朵和嘴"。

2026-10-28  🔴 双击批处理报「'p' 不是内部或外部命令…」= 文件存成了 LF 行尾
            症状：双击 run-body-talk.cmd 刷出一串碎片报错
              （'p' / 'le' / '/d' / 'Double-click' / 'Window' / 'Broker' …
               全是 REM 注释里的词）—— cmd.exe 把整行拆成碎块当命令执行。
            根因：我用 write 工具重写这个 .cmd 时行尾变成了 LF（只有 \n）。
              **cmd.exe 需要 CRLF**，LF 文件在它眼里行边界是错的。
            修法：所有 .cmd / .bat 一律 CRLF。rot 已加 .gitattributes
              （`*.cmd  text eol=crlf` / `*.bat  text eol=crlf`），git 不会再把它们改回 LF。
            同批修好的：rot\run-body-talk.cmd、rot\tools\idf.cmd、rot\.tmp\*.cmd、
              qq-bridge-deploy\专项控制台.bat。
            📌 规则：用 write 写批处理之后**必须跑一次**（`cmd /c "xxx.cmd --help"`），
              能打出正文才算写对；别只看文件内容像不像。

```
## 5. 两个仓库的关系

- **本工作区 `rot`**：身体（固件 + 文档 + PC 侧新增代码草稿）。
- **`..\QQ bot\qq-bridge-deploy`**：她现有的软件本体（bridge / 人格 / MCP）。
  最终 `src/mcp-body.js` 要落进那边，因为 DSH 是从那儿 spawn MCP 的。
  ⚠️ 那个目录的 ACL 有问题（`Everyone:(DENY)`），普通权限下 bash 起不来 ——
  在那边干活需要 danger-full-access。**本工作区没有这个问题。**

## 6. 工作方式

- 中文回答。改动前先读文件；改完跑测试。
- 硬件相关数字（电压、电流、限位）**不要编** —— 标注"待实测"，并写清怎么测。
- 主人会自己焊接、自己量。给他**能照着做的步骤**，不是概念。
