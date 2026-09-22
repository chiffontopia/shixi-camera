# 石溪相机 —— GEC6818 手机式多应用 UI（拍照 / 录像 / 图库 / AI 聊天 / 打砖块 / 音乐）

一个跑在 **GEC6818 开发板（S5P6818 + 7 寸 800×480 电容触摸屏）** 上的手机风格应用集合。
直接用 Linux framebuffer + evdev 触摸实现，**不依赖 Qt / LVGL / libjpeg**，整程序静态链接，
交叉编译后只有一个可执行文件，拷到板子上就能跑。

```
桌面（Launcher，图标 2 行 × 3 列，按应用数自适应）
 ├─ 拍照     全屏取景 + 快门 + 白闪动画 + 对焦框 + 左下角图库缩略图
 ├─ 录像     红键开始/停止，实时计时与帧数，边录边写 MJPEG AVI
 ├─ 图库     4×2 网格 + 左右翻页 + 大图查看 + 视频播放 + 删除（二次确认）
 ├─ AI 聊天  接真实大模型（宿主机 CCX 转发）· 拼音输入法 · 等待/取消/重试/清空
 ├─ 打砖块   5×8 砖块 + 固定子步物理 + 暂停 + 结算面板 + 最佳分落盘
 └─ 音乐     播放/暂停/切歌/停止 + 进度条拖动跳转 + 音量条；**切屏后继续播放**
```

---

## 一、从零开始：连上开发板 → 跑起来

下面这条链路是**从没用过这块板子开始**的完整流程，照顺序做即可。已经连过板子的话，
可以直接跳到 [第 5 步](#5-编译并一键部署)。

### 1. 硬件准备

| 东西 | 说明 |
|---|---|
| GEC6818 开发板 + 7 寸屏 | 800×480 电容触摸屏（`gslX680`，坐标 0..1024 / 0..600），随机附带 |
| **5V 电源适配器** | 接板子 CN1 圆口。**别只靠 USB 线供电**：摄像头一出图就掉线（实测 `error -32`）多半是这里 |
| 网线一根 | 一头插电脑网口，一头插板子网口（直连即可，不需要交换机/路由器） |
| USB 摄像头（可选） | 免驱 UVC 的；插板子的 USB Host 口。**OTG 口不能当 host 用**（板上没接收发器） |
| 串口线（可选） | 板子起不来、网也不通时，用 UART0（DB9）接 SecureCRT/XShell 看启动日志 |

### 2. 板子开机后的默认状态

板子 `/etc/profile` 里自带 `ifconfig eth0 169.254.193.77`，所以：

| 项目 | 值 |
|---|---|
| 板子 IP | `169.254.193.77`（eth0，链路本地地址） |
| 登录 | `root` / `123456`（SSH 22、Telnet 23 都已开） |
| 程序安装目录 | `/root/shixi/` |
| 内核 / C 库 | Linux 3.4.39-gec / glibc 2.23（**这就是必须静态链接的原因**） |

### 3. 电脑侧网络：确认和板子同网段

板子的 `169.254.x.x` 是"链路本地地址"，电脑这块网卡只要也是 `169.254.x.x` 就能互通。
Windows 不插路由器时会自动分配（APIPA）；Linux 上可以手动加一个：

```bash
# Linux/WSL（把 ethX 换成插网线的那张网卡；169.254.195.149 只是举例，同网段即可）
sudo ip addr add 169.254.195.149/16 dev ethX
ip addr | grep 169.254            # 确认本机地址（Windows 用 ipconfig）
ping -c 3 169.254.193.77          # 能通就可以 SSH 了
```

> 不通的排查顺序：网线插紧（看网口灯）→ 电脑网卡是不是被设成了别的固定 IP →
> 板子是不是还在启动（屏幕亮了吗）→ 上串口看 `ifconfig`。

### 4. SSH 连接开发板

```bash
ssh root@169.254.193.77           # 密码 123456，首次连接输入 yes 确认指纹
```

在板子上确认一下环境和设备节点（这几条决定了程序能不能跑）：

```sh
uname -a                          # Linux GEC6818 3.4.39-gec armv7l
ls -l /dev/fb0                    # 屏幕：framebuffer，800x480x32bpp
ls -l /dev/input/event0           # 触摸屏（gslX680 电容屏）
cat /sys/class/input/event0/device/name
ls /dev/video*                    # 摄像头（没插就只有 video0..6 这些 SoC 内部设备）
df -h /                           # 剩余空间（约 240MB 可用）
free -m                           # 内存（约 780MB 可用）
```

顺手把板子时间设对（板子没有联网对时；断电后 RTC 可能回到 2015 年，
照片/视频的时间戳就是它）：

```sh
date -s "2026-09-21 11:05:00"
```

> 想免密登录：把电脑的公钥写进板子 `/root/.ssh/authorized_keys` 即可；
> 但注意板子 sshd 对 `authorized_keys` 的属主/权限很挑（`bad ownership or modes` 会忽略它）。

### 5. 编译并一键部署

在电脑上（本 README 所在目录，即仓库根目录）：

```bash
git clone https://github.com/chiffontopia/shixi-camera.git
cd shixi-camera

make                 # 交叉编译（工具链自动挑选，静态链接）
./run_board.sh start # 编译 + 推送 + 在板子上后台启动
./run_board.sh logs  # 看启动日志
```

`run_board.sh start` 会做四件事：`make` → 停掉板上旧进程 → scp 上传
（`shixi`、静态 `curl`、字体 `SimHei.ttf`、AI 桥接脚本）→ `setsid nohup` 后台启动。
板子地址/密码可用环境变量覆盖：`BOARD=root@1.2.3.4 PW=xxx ./run_board.sh start`。

> - `run_board.sh` 优先用 `sshpass`；**没装 sshpass 会自动改用 OpenSSH 的 `SSH_ASKPASS`**
>   （Ubuntu/WSL 上常见），不需要额外安装。
> - scp 必须带 `-O`：新版 OpenSSH 默认走 sftp，老板子的 sshd 不支持。

### 6. 在板子上用

程序启动后就是手机式桌面，直接**用手指点**（详见 [第三节](#三界面操作说明)）。
不用跑到板子跟前也能验证：

```bash
./run_board.sh shot        # 截屏并转成 PNG（需要 pillow）
./run_board.sh tap 400 430 # 注入一次点击（程序需以调试输入启动，run_board.sh 默认开着）
./run_board.sh stop        # 停止
```

### 7. 可选：接摄像头 / 开 AI 聊天

- **摄像头**：插上免驱 UVC 摄像头 → 进"拍照/录像"界面会在 2 秒内自动切到实时画面
  （热插拔，不用重启程序）。有问题先跑 `/root/shixi/camtest`（见第四节）。
- **AI 聊天**：板子自身没有外网，需要电脑上跑一个转发服务 —— 两条路线与配置见
  [第九节](#九ai-助手宿主机转发--桥接怎么让聊天能用)。

### 8. 可选：开机自启

```sh
# 在开发板上执行：把启动命令追加到 /etc/profile
echo 'cd /root/shixi && ./shixi &' >> /etc/profile
# 取消自启：编辑 /etc/profile 删掉这一行
```

### 9. 出问题先看这几条

| 现象 | 原因 / 处理 |
|---|---|
| `ssh: connect ... timed out` | 电脑网卡不在 `169.254.x.x`；或板子还没起来（看屏幕/串口） |
| `scp: ... Text file busy` | 板上程序还在跑 → 先 `./run_board.sh stop`，或让 `run_board.sh` 自己停 |
| 板上运行报 `GLIBC_2.3x not found` | 编译时没静态链接（板子 glibc 2.23，工具链是 2.39） |
| 程序起来但屏幕黑 / 报缺字体 | `SimHei.ttf` 没推上去（`run_board.sh` 会检查并报错） |
| 摄像头 `error -32` / `unable to enumerate` | 供电或线材；见第四节与第十一节 |
| 拍照/录像界面 3 秒后回演示画面 | 摄像头掉线了：供电不足或线材，程序会自动退回演示画面并重试 |
| 聊天一直"正在思考…" | 宿主机转发服务没开（relay 或 bridge），见第九节 |
| 照片时间戳是 2015 年 | 板子没对时：`date -s "..."` |

---

## 二、快速开始（速查）

已经连过板子、只想跑一遍的话，就是这四条：

```bash
cd shixi-camera       # 仓库根目录（本 README 所在目录）
make                 # 交叉编译到开发板（工具链自动挑选，静态链接）
./run_board.sh start # 编译 + 上传 + 在板子上后台启动
./run_board.sh shot  # 截屏并转成 PNG，看当前界面
```

板子默认地址 `root@169.254.193.77`（密码 `123456`），程序安装到 `/root/shixi/`。
可用 `BOARD=root@其它IP ./run_board.sh start` 覆盖。

`run_board.sh` 优先用 `sshpass`；**没装 sshpass 的机器会自动改用 OpenSSH 的
`SSH_ASKPASS`**（Ubuntu/WSL 上常见），不需要额外安装。`shot` 转 PNG 需要
`pillow`（`pip install pillow`，或 `pip install --user --break-system-packages pillow`）。

### 换工具链

Makefile 会自己挑：有 `arm-linux-gnueabi-gcc` 就用它，没有就退到粤嵌课程常见的
`arm-linux-gcc 5.4.0`（已实测可用：交叉编译通过、上板运行、/dev/video7 取景正常）。
强制指定：

```bash
make CROSS=arm-linux-                       # 用 arm-linux-gcc
make CROSS=arm-linux- ARCH_FLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp"
```

`arm-linux-gcc 5.4.0` 的内核头文件里没有 `V4L2_CAP_DEVICE_CAPS` / `device_caps`，
`core/v4l2compat.h` 的 `v4l2_device_caps()` 用宏判断兜住了，新旧工具链都能编。

**必须静态链接**：板子 rootfs 是 2016 年的 glibc 2.23，新工具链的动态链接程序在板上会报
`GLIBC_2.3x not found`；静态链接后是单文件、无依赖，直接跑。

### 板端小工具

```bash
make tools          # bin/fbshot（截屏）· touchsim（虚拟触摸屏）· camtest（摄像头诊断）
                    # · jpgbench（JPEG 编码基准）· aitest（AI 链路自检）
make curl-arm       # 生成 bin/curl-arm：静态、纯 HTTP 的 curl（AI 助手连宿主机用）
```

`bin/curl-arm` 由 `tools/build_curl_arm.sh` 生成（会自动下载源码、静态链接）。
板上是 glibc 2.23，动态链接的 ARM curl 跑不起来，所以必须是静态单文件；
`run_board.sh start` 会在它存在时一并推送到 `/root/shixi/curl`。

### 在开发板终端上直接运行

```sh
cd /root/shixi
./shixi                 # 前台运行，Ctrl+C 退出
```

---

## 三、界面操作说明

| 界面 | 操作 |
|---|---|
| 桌面 | 轻触图标进入应用；顶部显示时间/信号/电量，底部显示剩余存储 |
| 拍照 | 中间白色圆键拍照；点画面任意处出现对焦框；左下角缩略图进图库；左上角返回 |
| 录像 | 中间红键开始/停止；录制中显示 `● 00:12` 计时、帧数与已写入大小；最长 120 秒自动停止 |
| 图库 | 左右滑动或拖动翻页；顶部标签切换 全部/照片/视频；轻触缩略图看大图 |
| 图库·查看 | 左右滑动或点两侧箭头切换上一张/下一张；轻触画面隐藏/显示工具条；右上角垃圾桶删除（二次确认） |
| 图库·视频 | 中央按钮播放/暂停；底部进度条可点按跳转；左右滑动快退/快进 10% |
| AI 聊天 | 轻触输入框弹出键盘；**中文用拼音打字**（键面是 QWERTY，右下角「中/En」切换中英文）；输入框上方三颗预设话题可直接点；**发送后气泡区出现「正在思考…」，约 1 秒换成模型真回复**；请求中发送键变红「取消」；失败会在气泡里说清原因（连不上/超时/密钥/服务端）并给出顶栏「重试」；右上角「清空」清掉本次会话（也清远端上下文） |
| 打砖块 | **轻触游戏区发球**；手指按住哪儿挡板跟到哪儿（松手就不动；点顶栏「暂停/返回」不会把挡板吸走）；右上角「暂停/继续」；漏球弹出结算面板，「再来一局」或「返回桌面」；最佳分存 `$SHIXI_ROOT/brick_best.txt` |
| 音乐 | 中间圆键播放/暂停，两侧「上一首/下一首」，右侧「停止」；**轻触或拖动进度条松手后跳转**；轻触/拖动音量条即时调节（默认 60%）；离开界面**继续播放**（要停按「停止」） |

触摸屏是电容屏（gslX680），程序启动时用 `EVIOCGABS` 读取坐标范围自动标定到 800×480，
换屏或坐标反向时可用环境变量调整：

```sh
SHIXI_TOUCH_DEV=/dev/input/event0   # 指定触摸设备
SHIXI_TOUCH_SWAP=1                  # XY 交换
SHIXI_TOUCH_INVERT_X=1              # X 反向
SHIXI_TOUCH_INVERT_Y=1              # Y 反向
```

游戏与字体还有几个可调项：

```sh
BRICK_BEST=/tmp/b.txt      # 换个位置存最佳分（默认 $SHIXI_ROOT/brick_best.txt）
SHIXI_BRICK_DEMO=1         # 打砖块演示模式：自动跟球一直打（无人值守演示用）
SHIXI_BRICK_DEMO=15000     # 演示 15 秒后交还手动
MP_DIR=/root               # 音乐扫描目录（默认 $SHIXI_ROOT/music，没有就用 /root）
MP_FIFO=/tmp/mp_fifo       # 给 mplayer 发命令的命名管道（默认 $SHIXI_ROOT/tmp/mp_fifo）
SHIXI_LLM_URL=…            # 模型接口地址（默认读 $SHIXI_ROOT/relay_url；不配 = 演示模式）
SHIXI_LLM_KEY=…            # 接口密钥（默认读 $SHIXI_ROOT/relay_key，0600）
SHIXI_LLM_MODEL=…          # 模型名（默认读 $SHIXI_ROOT/ai.conf 的 model=，再默认 deepseek-flash）
SHIXI_CURL=/path/curl      # 换个 curl（默认 $SHIXI_ROOT/curl，主机上退回 PATH 里的 curl）
SHIXI_TTF=/path/font.ttf   # 换字体（默认 SimHei.ttf）
```

---

## 四、摄像头

程序启动时自动扫描 `/dev/video0..15`，挑选第一个支持视频采集的设备（UVC 摄像头），
优先协商 **MJPEG**（帧可直接存成照片/写入视频），不支持时退回 **YUYV** 并自行转 RGB 与编码 JPEG。

- **没插摄像头也能演示**：自动切换到动态演示画面（渐变天空 + 移动云 + 计时 + 走动的方块），
  各应用的完整功能（拍照、录像、图库、删除、播放，以及打砖块）都能正常使用。
- **支持热插拔**：程序在演示模式下每 2 秒扫一次设备，摄像头插上后进"拍照/录像"界面会自动切换过去，
  不用重启程序（切换时录像中不会切，避免文件尺寸变化）。
- 指定设备：`SHIXI_CAMERA=/dev/video7 ./shixi`
- 强制演示模式：`SHIXI_CAMERA=none ./shixi`
- 参考的课程 demo（`参考/demo2/录像`）用的是 `/dev/video7`，本程序不写死设备号，插上就能用。

---

## 五、文件与存储

```
/root/shixi/
├── shixi                 可执行文件（静态链接，无需任何动态库）
├── SimHei.ttf            运行时字体（stb_truetype 栅格化，任意中文都能显示）
├── photo/IMG_20260921_180318.jpg     照片（JPEG，640×480）
├── video/VID_20260921_180515.avi     视频（MJPEG AVI，可用电脑播放器直接播放）
├── brick_best.txt        打砖块最佳分（一行十进制数）
├── .cache/*.tmb          图库缩略图缓存（首次浏览生成，之后秒开）
└── tmp/                  录像临时文件（异常退出后下次启动自动清理）
```

- 根目录可用 `SHIXI_ROOT=/mnt/udisk/shixi ./shixi` 指到 U 盘等其它位置。
- **音乐不占这个目录**：默认扫 `/root` 下的 `*.mp3`（板子上现成就有 1/2/3.mp3），`MP_DIR` 可换；
  给 mplayer 发命令的管道在 `$SHIXI_ROOT/tmp/mp_fifo`。
- **AI 助手的配置就在这个目录**：`relay_url`（接口地址，一行）、`relay_key`（密钥，0600）、
  `ai.conf`（可选，`model=` / `max_tokens=` / `timeout=` / `history=`）。**没配 `relay_url` 就是演示模式**
  （回复走本地关键词表并在气泡里标明）。请求体/响应/HTTP 状态码会落在 `tmp/ai_*.json|txt` ——
  出错时可以直接看「到底发出去的是什么」，那份带密钥的 `tmp/ai_curl.cfg` 每次关界面都会删掉。
- 照片名与视频名都带时间戳，图库按修改时间倒序排列（最新的在最前）。
- 视频是标准 MJPEG AVI，`ffmpeg` / VLC / 暴风影音 / 板上的 `mplayer` 都能直接播放。

---

## 六、代码结构

```
src/
├── main.c                 程序入口、桌面（Launcher）、应用调度、切换动画、主机模拟器
├── core/
│   ├── gfx.[ch]           图形层：framebuffer 双缓冲（FBIOPAN_DISPLAY）、
│   │                      矩形/圆角/圆/三角形/渐变/缩放贴图（SDF 抗锯齿）
│   ├── font.[ch]          文字渲染：stb_truetype 运行时栅格化 + 字形 LRU 缓存、
│   │                      多行换行与中文禁则（缓存加锁：相机采集线程也会画字）
│   ├── pinyin.[ch]        拼音查表：精确查找 / 前缀判定 / 音节切分
│   ├── pinyin_dict.h      自动生成的候选表（tools/make_pinyin_dict.py，勿手改）
│   ├── image.[ch]         图片解码/编码（stb_image）、双线性缩放、缩略图
│   ├── input.[ch]         触摸输入：evdev 解析、坐标标定、点击/滑动/长按识别、调试注入
│   ├── ui.[ch]            控件与矢量图标：状态栏、顶栏、按钮、对话框、Toast、26 个图标
│   ├── media.[ch]         媒体库：扫描、命名、删除、缩略图磁盘缓存、空间统计
│   ├── camera.[ch]        V4L2 采集线程（MJPEG/YUYV）+ 演示信号源 + JPEG 供帧
│   ├── avi.[ch]           MJPEG AVI 封装（边录边写、结束回填索引）与解封装（回放）
│   ├── music_player.[ch]  音乐后端：mplayer -slave + 命名管道（原 LVGL 项目搬运，与 UI 无关）
│   ├── ai.[ch]            AI 后端：fork/exec curl + 临时文件 + 手写 JSON 解析（与 UI 无关）
│   └── util.h             时间、缓动、格式化等小工具
├── apps/
│   ├── apps.h             应用接口（enter/leave/frame/event）
│   ├── app_camera.c       拍照
│   ├── app_video.c        录像
│   ├── app_gallery.c      图库（网格 / 查看 / 播放 / 删除）
│   ├── app_chat.c         AI 聊天界面（气泡/键盘/拼音 + 等待·取消·重试·清空；没配 endpoint 时用本地预设回复）
│   ├── app_brick.c        打砖块（定步长物理 + 手绘界面 + 结算面板）
│   └── app_music.c        音乐界面（进度/音量/传输键，映射到 music_player 后端）
├── third_party/           stb_image.h、stb_image_write.h、stb_truetype.h（公共领域，单头文件）
└── tools/
    ├── fontprobe.c        主机端字体回归探针（新引擎 vs 归档图集：度量 + 观感）
    ├── make_pinyin_dict.py  生成 core/pinyin_dict.h（需 pypinyin，仅主机端生成用）
    ├── pinyin_dict_src.c    原手工候选表（保留作为常用度排序依据）
    ├── pinyin_test.c      主机端拼音逻辑自测（音节判定 / 切分 / 候选顺序）
    ├── fake_mplayer.py    主机端 mplayer 替身（`make mplayer-mock` 装成 bin/mplayer）
    ├── ai_parse_test.c    AI 回复解析/错误分类门禁（`make ai-parse-test`，用 testdata 的真实抓包）
    ├── testdata/ai_*.json 五份真实响应（正常/截断/带转义引号/401/400），给上面的门禁当输入
    ├── icontest.c         主机端把图标排成一张图，方便调美术
    ├── touchsim.c         板端：用 /dev/uinput 造虚拟触摸屏，自动点击测试
    ├── fbshot.c           板端：抓取当前可见画面（考虑双缓冲 yoffset）
    ├── fb2png.py          主机端：把 fbshot 的原始数据转 PNG
    └── SimHei.ttf         默认字体（部署时推到板上 /root/shixi/SimHei.ttf）
```

### 字体（不再是「预生成字库」）

文字由 `stb_truetype` **运行时栅格化**，所以**任意中文都能显示**。以前那套
「扫描源码生成位图图集」的做法只能显示预先收录的 538 个字（改文案还得重跑脚本），
已归档到 `archive/gen_font.py` + `archive/font_atlas.h`，不再参与构建。

- 默认字体：板上 `/root/shixi/SimHei.ttf`（部署脚本会一起推；主机模拟器用 `tools/SimHei.ttf`）
- 换字体：`SHIXI_TTF=/home/gec/simkai.ttf ./shixi`（板上自带楷体与宋体）。
  中易三兄弟（SimHei / simkai / simsun）**度量完全相同**，换字体不动版面；
  DroidSansFallback 不要用（advance 与这套界面不兼容，偏差最大 21px）
- 字体缺失会**红屏 + 错误日志**退出，不会花屏或静默空白
- 回归门禁：`make fontprobe` → 与归档图集逐字形比对 advance 与行高（应为 538/538 全一致）

### 多行排版

`font.h` 的 `text_wrap_split / text_wrap_height / text_wrap_draw` 支持按宽度换行：
汉字可任意断行、拉丁按词断行、带中文禁则（`、` 这类标点不会落到行首），
行距 = `font_height + line_gap`。字体没有的码点（emoji、U+9FA5 之外的生僻字）
画一个**空心方框**，不会静默吞掉。

---

## 七、调试与验证工具

### 0) 摄像头诊断（插上摄像头后先跑这个）

```sh
./camtest                # 列出所有 /dev/videoN 的能力、支持格式与分辨率
./camtest /dev/video7    # 对指定设备做采集测试，打印实际帧率并存一帧到 /tmp/cam_frame.jpg
```

若扫不到设备，按提示检查：① 插在 USB Host 口（不是 OTG 口）；② 板子用 5V 电源适配器供电
（摄像头瞬时电流大，供电不足会 `dmesg` 报 `error -32 / unable to enumerate`）；
③ 换短一点的 USB 线，避开延长线；④ 在电脑上确认摄像头是 UVC 免驱的。

### 1) 主机模拟器（不用板子就能调 UI）

把同一份代码编译成主机程序，用脚本驱动点击并输出 PNG：

```bash
make host
cat > /tmp/sim.txt <<'EOF'
frames 10
seed 9 2                 # 生成 9 张示例照片 + 2 段示例视频（用演示画面）
shot /tmp/shots/01_home.png
tap 154 232              # 点“拍照”
frames 40
shot /tmp/shots/02_camera.png
tap 400 400              # 按快门
frames 30
shot /tmp/shots/03_shot.png
quit
EOF
SHIXI_ROOT=/tmp/simroot SHIXI_SIM_SCRIPT=/tmp/sim.txt SHIXI_CAMERA=none ./bin/shixi_host
```

脚本命令：`tap X Y`、`swipe X0 Y0 X1 Y1`、`down/up/long X Y`、`frames N`、`wait MS`、
`shot PATH`、`seed NP NV`、`quit`。

### 1b) 用 LD_PRELOAD 假摄像头测真实 V4L2 代码路径（主机上）

板子暂时没摄像头时，可以在主机上伪造一个 UVC 摄像头，把 `camera.c` 的
V4L2 流程（能力查询 → 格式协商 → mmap 缓冲 → DQBUF/QBUF → MJPEG 解码）
完整跑一遍：

```bash
make host                       # 同时生成 bin/v4l2mock.so（不必手敲 gcc）
mkdir -p /tmp/mockframes && cp some/*.jpg /tmp/mockframes/     # 循环播放的 MJPEG 帧
MOCK_VIDEO_DIR=/tmp/mockframes MOCK_VIDEO_FPS=12 \
  LD_PRELOAD=./bin/v4l2mock.so SHIXI_SIM_SCRIPT=/tmp/sim.txt ./bin/shixi_host
# 可选：MOCK_VIDEO_FORCE_YUYV=1 模拟只支持 YUYV 的摄像头
#       MOCK_VIDEO_DELAY=6     模拟 6 秒后才插上（验证热插拔切换）
```

用这个方式验证过：MJPEG 与 YUYV 两条路径下的预览、拍照（640×480 JPEG）、
录像（AVI 用 ffmpeg 校验通过，帧率按实测写入）。

### 2) 板端无人值守测试

```sh
# 启动时打开调试通道，之后可以从别的终端注入触摸事件
SHIXI_DEBUG_INPUT=1 ./shixi &
echo 'tap 400 400' > /tmp/shixi_ctrl        # 模拟点击
echo 'swipe 700 240 100 240' > /tmp/shixi_ctrl  # 模拟滑动
```

### 3) 验证真实 evdev 输入链路（用 uinput 造虚拟触摸屏）

```sh
./touchsim serve &                          # 会打印命令管道 /tmp/touchsim.fifo
for i in 0 1 2 3 4 5; do echo "event$i: $(cat /sys/class/input/event$i/device/name)"; done
# 找到 shixi-touchsim 对应的 eventN，然后：
SHIXI_TOUCH_DEV=/dev/input/event5 ./shixi &
echo 'tap 154 232' > /tmp/touchsim.fifo     # 真的走一遍 evdev 解析与坐标标定
```

### 4) 截屏

```sh
/tmp/fbshot /tmp/shot.raw     # 板端抓图（自动处理双缓冲 yoffset）
python3 tools/fb2png.py /tmp/shot.raw   # 主机端转 PNG
```

### 5) 字体与拼音的回归自测

```sh
make fontprobe     # 与归档图集逐字形比对 advance 与行高（应 538/538 全一致）
make pinyin-test   # 音节判定 / 音节切分 / 候选顺序（含 e 块与 ü->v）
```

---

## 八、实现要点（可能对报告/答辩有用）

1. **显示**：`/dev/fb0` 是 800×1440 的虚拟缓冲（3 屏），用 `FBIOPAN_DISPLAY` 做双缓冲翻页，
   画面无撕裂；像素格式 `0x00RRGGBB`。
2. **字体**：`stb_truetype` 运行时栅格化 + 每字号 1024 项字形 LRU 缓存（8bit 覆盖率、
   开放寻址哈希、同链淘汰），逐像素 alpha 混合，支持 UTF-8、截断省略、裁剪区、多行换行与中文禁则。
   度量刻意与原位图图集对齐（ascent/descent 沿用图集常数、advance 同样四舍五入取整），
   实测 **538/538 字形 advance 完全一致** → 换引擎不动任何版面（`make fontprobe` 可复验）。
3. **抗锯齿**：圆角矩形/圆用 SDF 覆盖率，三角形用 3×3 超采样，图标离屏缓存一次后直接贴图。
4. **透明贴图**：离屏画布用 `0xFF000000` 作为"未绘制"键值，贴图时跳过，实现图标透明背景。
5. **输入**：常开 evdev fd + `poll` 非阻塞读，状态机识别 点击/长按/四方向滑动/拖动，
   坐标由 `EVIOCGABS` 动态标定。
6. **摄像头**：`VIDIOC_S_FMT` 先试 MJPEG 再试 YUYV；采集线程负责解码/转码，
   界面线程只做缩放贴图；缓冲区处理完才 `QBUF`，避免被驱动覆盖。
7. **视频封装**：自己实现最小 MJPEG AVI（RIFF/avih/strh/strf/movi/idx1），
   边录边写，结束时回填头部与索引；帧率按**实测**写入，播放速度才正确。
8. **图库性能**：缩略图解码后落盘缓存（186×140，带 mtime+size 校验），二次进入秒开；
   内存里再缓存最近 24 张。
9. **踩过的坑：`struct v4l2_buffer` 的 ABI 不匹配（很关键）**
   板子内核 3.4 里 `struct v4l2_buffer` 是 **68 字节**（时间戳是 32 位 timeval，8 字节），
   而新工具链的内核头文件里时间戳变成 16 字节、结构体是 **80 字节**。
   `VIDIOC_QUERYBUF/QBUF/DQBUF` 的 ioctl 号用 `_IOWR` 把结构体大小编进了命令码
   （`0xc0445609` → `0xc0505609`），老内核认不出 → 直接返回 `ENOTTY`。
   现象很迷惑人：能力查询、格式协商（S_FMT）都正常，一到申请缓冲区就失败，
   换个摄像头也没用。解决见 `core/v4l2compat.h`：显式定义 3.4 的布局，
   并在打开设备时用 `QUERYBUF` 探测该用哪套 ABI（新老内核都能跑）。
   ⚠️ 用 `LD_PRELOAD` 假摄像头测不出这个问题——同进程里结构体当然是一致的，
   所以必须上真机验证。

10. **拼音输入法**：QWERTY 键盘当拼音键盘用。打出的字母进 `py[]` 缓冲、**不上屏**，
    选中的字才进输入框 —— 所以发出去的内容永远不会夹生拼音。候选表 410 音节 / 26731 字，
    按「原手工表常用度 → GB2312 一级常用字 → CJK 基本区 → 扩展A」排序，于是「你 / 好 / 中 / 旅」
    这些第一个候选就是想要的字（空格直接取第一个）。支持连续打多音节（`nihao` 先出「你」，
    选完自动接着出「好」的候选）、按宽度铺满不翻页、拼音不成音节就丢弃该次按键、
    退格先退拼音再退正文且按**完整字符**退（中文不会被截成半个）。自测：`make pinyin-test`。

11. **打砖块：游戏循环与代价**。`main.c` 已按 30fps 调 `frame(t_ms)`，游戏内部再用
    **8ms 固定子步 + 累加器**（每帧约 4 个子步），物理与帧率解耦：掉帧不改变球速，
    卡顿时最多补 100ms 就不追了（防「死亡螺旋」）。球速上限 600 px/s → 每子步 4.8px，
    远小于砖厚 26px 与球宽 18px，**不会穿透**；每次挡板反弹只改方向不加速，所以不会失控。
    渲染走本栈的老路（每帧整屏重画 + 翻页），不引入第二套机制，**每帧约 55 万像素绘制
    + 38.4 万像素翻页拷贝（1.5MB）**，30fps 约 74 MB/s 内存带宽 —— 与拍照/录像界面同量级。
    跟手不用事件流：`input.c` 是事件式的（没有「当前按压点」查询），程序自己记住手指最后位置，
    每帧把挡板贴过去；手指不动就不发事件、挡板也不动，效果与「绝对跟手」一致。
    游戏区判定用 `y >= 58`（顶栏高度），所以点顶栏按钮不会把挡板吸走。

12. **音乐：后端与界面解耦 + 后台播放**。播放能力来自 `core/music_player.c`（mplayer 1.0rc2 的
    `-slave` + 命名管道，与 UI 无关，从原 LVGL 项目原样搬来），界面 `apps/app_music.c` 只做映射。
    `MP_Poll()` 挂在**主循环**上（200ms 节流）而不是界面里，所以切到别的应用后仍在收 mplayer 的
    `ANS_` 应答、播完自动下一首；离开界面不打断播放，要停按「停止」。暂停用 SIGSTOP/SIGCONT
    （rc2 的 pause 命令只能单向停住），音量用绝对设置 `volume N 1`，跳转用 `seek N 2`。
    板上实测：mplayer 的 fd 指向 `/dev/dsp`（声音真的送到 ALC5623），CPU 约 1.2%；
    `killall shixi`（SIGTERM）后**不留 mplayer 孤儿**。
13. **AI 助手：curl 子进程 + 文件当管道**。板端传输在 `core/ai.c`，界面只管画：
    `fork/exec` 板上的静态 curl，**请求体写文件后用 `--data-binary @file`**（JSON 不进命令行：转义地狱 + 参数长度），
    响应也落文件（没有半截 JSON 的问题）；**密钥写进 curl 的 `-K` 配置文件**（0600），argv 里只有文件名 ——
    `ps` 看不到密钥。主循环里 `ai_poll()` 用 `waitpid(WNOHANG)` 收结果，**界面全程不阻塞**；
    超时、点「取消」、直接按返回离开界面都会 `kill` 子进程（不留孤儿 curl）。`max_tokens` 默认 **800**：
    这两个模型是**推理模型**，先烧 50~110 个 token 想，额度给少了 `content` 会是空的（实测 64 时空回复）。
    只保留最近 **3 轮**进上下文；**失败的轮次不写历史**，所以「重试」不会把同一句话记两遍。
    解析器是手写的（板上没有 JSON 库），用 `make ai-parse-test` 拿**真实抓包**钉住 —— 它上线第一天就抓到
    「忘了跨过 key 后面的冒号」这个静默错误。
14. **触控容错（这块电容屏"点不动"的根因）**
   实测这块 gslX680 电容屏 + 真手指有三个坑，都会表现成"点了没反应"：
   - **按下坐标可能是上一次触摸的旧值**：按下包里 BTN_TOUCH 与坐标的先后顺序不固定，
     个别驱动坐标还排在后面。输入层现在**等本次触摸的坐标到齐才上报 DOWN**
     （`pending_down` + `coords_seen`，60ms 兜底），上层不会拿旧坐标做命中判定。
   - **点击一律按"手指落下的位置"判定**：真手指按下去常滑 20~70px，
     若按抬起位置判定，手指滑出按钮就会点不动。滑动/拖动仍用当前位置。
   - **每帧只取一个事件会让队列积压**：手指一抖就刷出大量 MOVE，
     抬起事件要等好几帧才轮到 -> 手感发木。现在每帧排空事件（上限 32 个/帧），
     且相邻两次坐标没变化就不再上报 MOVE。
   点击判定也放宽了：净位移 <46px 一定是点击；按住 ≥250ms 且位移 <100px 也算点击
   （慢按、抖动都不丢）；只有真正的快速划动才算滑动。
   排查用 `SHIXI_TOUCH_DEBUG=1 ./shixi`，日志会打印每次手势的起点/终点/位移/时长/判定。

15. **不用任何第三方 GUI/图像运行时**：只有 stb 三个单头文件，静态链接后单文件部署，
   避免板子 glibc 2.23 与新工具链 glibc 2.39 的版本冲突（这正是必须静态链接的原因）。

---

## 九、AI 助手：宿主机转发 / 桥接（怎么让聊天能用）

板端 `core/ai.c` 只管「拼请求、发出去、把 JSON 解析回来」；真正连云端的两条路都在电脑上，
**云端密钥只留在电脑里**（板子根本不需要知道它）。

### 路线 A：宿主机 relay（板子能连到电脑时用，推荐）

```bash
cp tools/ai_secret.conf.example tools/ai_secret.conf   # 填 endpoint / api_key / relay_key
python3 tools/ai_relay.py                              # 或 make relay；监听 0.0.0.0:3688
```

板子侧（`$SHIXI_ROOT/`）：

```sh
echo 'http://<电脑IP>:3688/v1/chat/completions' > relay_url
printf '%s' '<与 ai_secret.conf 里 relay_key 相同的口令>' > relay_key && chmod 600 relay_key
```

链路：板端静态 curl → 本机 relay（校验 relay_key）→ 带真密钥请求云端。
注意电脑防火墙要放通 3688（WSL 还要把端口转发进虚拟机）。
`ai_relay.py` 转发时伪装成 curl 的 User-Agent —— 默认的 `Python-urllib/x.y` 会被某些
CDN/WAF 直接 403（实测 `error code: 1010`），这个坑已经在脚本里踩过了。

### 路线 B：宿主机桥接（板子连不到电脑时用）

有些环境板子根本打不到 relay（Windows 防火墙挡入站、WSL 端口转发没配）。这条只借用
**电脑 → 板子**这个已经通的 SSH 方向，不需要放通任何端口：

```bash
./tools/ai_bridge.sh          # 或 make bridge；另开一个终端挂着，读同一份 ai_secret.conf
```

板子侧 `ai.conf` 里加一行（`run_board.sh` 会自动把该脚本推到板上）：

```ini
transport=/root/shixi/ai_bridge_board.sh
```

链路：板子写 `tmp/ai_bridge_req.json` → 电脑轮询取走（`mv` 原子取，不会重复处理）→
电脑调云端 → 写回 `tmp/ai_bridge_resp.txt` → 板子搬进 ai.c 的响应文件并按 HTTP 码判断成败。

两条路线可以随时切换：配了 `transport` 就忽略 `relay_url`，反之亦然；
`SHIXI_LLM_TRANSPORT` 环境变量可临时覆盖。

### 自检与排查

```sh
./aitest "你好"                      # 板端：不开界面验证整条链路（成功 / 失败 / 超时三态）
cat $SHIXI_ROOT/tmp/ai_req.json      # 发出去的请求体（长什么样一眼看到）
cat $SHIXI_ROOT/tmp/ai_status.txt    # HTTP 状态码
cat $SHIXI_ROOT/tmp/ai_err.txt       # 传输命令的 stderr
```

### 密钥卫生

- `tools/ai_secret.conf`、`ai.conf`、`relay_url`、`relay_key` 都在 `.gitignore` 里，不会被提交；
  模板见 `ai.conf.example` 与 `tools/ai_secret.conf.example`。
- 板子只持有 `relay_key`（本机口令，可随时换），云端密钥只写在电脑上。
- 万一密钥曾进过版本库，光删除不够——请到控制台**轮换**该密钥。

---

## 十、音频说明（重要）

- **本项目的摄像头不带 USB 音频**：这类便宜 UVC 模块通常只有视频接口
  （`class 0x0e`），没有音频接口（`class 0x01`），所以 Linux 下看不到它的麦克风。
- **板子内核没有 USB 音频驱动**：`/lib/modules/*/modules.builtin` 里没有
  `snd-usb-audio`，板上也没有任何可加载模块 → 插 USB 麦克风/声卡也用不了。
- **板子自带音频输入可用**（红色 3.5mm 口，ALC5623 codec，节点 `/dev/snd/pcmC0D0c`），
  但板上没有录音工具（`ffmpeg` 只编译了 ALSA 输出，没有输入；没有 `arecord`）。
  要录音需要自己写一个小 ALSA 采集程序（直接 ioctl，不依赖 libasound）。
- 因此当前**录像没有声音**。

## 十一、已知限制

- **AI 聊天要有宿主机服务才能答**：板子只可能打到它 ARP 表里那个邻居（宿主机 USB 网卡的自分配地址，
  形如 `169.254.x.x:3688`），**没有默认网关、连不到 WSL 的地址**。宿主机上的 CCX 转发服务没开、
  或者地址变了，界面会明确说「连不上宿主机（…）」并给「重试」，不会假装回答。
  没配 `relay_url` 时退回本地预设文案（气泡里标注「演示回复」）。
- 回复以**非流式**整段返回（服务支持 SSE，本轮没用）：首字要等 0.6~1.0 秒（`deepseek-flash`），
  期间显示「正在思考…」。超过 `ChatMsg.text` 上限的长回复按 UTF-8 边界截断并标注。
- 录像**没有声音**，原因见第十节。
- 视频帧率取决于摄像头：MJPEG 摄像头可到 15–30fps；YUYV 摄像头需要板上编码 JPEG，
  实测约 6–10fps（录制时按实测帧率写头，播放速度正常）。
- 演示画面模式下 JPEG 编码占用较高，预览约 17fps；接真实摄像头会更流畅。
- 板子没有联网对时，时间来自 RTC；`./run_board.sh` 里可用 `date -s` 手动校准。
- 板载存储只有约 240MB 可用，录像最长限制 120 秒，空间不足会自动停止并保存。
- 字体不含 emoji：回复里的 emoji 会显示成空心方框（字体覆盖 28522 个码点，日常中文全有）。
