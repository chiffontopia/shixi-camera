# ============================================================
#  石溪相机 —— GEC6818 手机式 UI 应用
#
#  make            交叉编译到开发板 (arm-linux-gnueabi, 静态链接)
#  make host       编译主机模拟器（输出 PNG 预览，便于调 UI）
#  字体            运行时用 stb_truetype 栅格化 tools/SimHei.ttf，任意中文都能显示
#  make deploy     编译并推送到开发板 /root/shixi/
#  make run        推送后在开发板上运行（需要串口/ssh 前台）
#  make clean
# ============================================================

.DEFAULT_GOAL := all

# 工具链自动挑选：有 arm-linux-gnueabi- 就用它（合作者环境），没有就退到 arm-linux-（粤嵌课程自带）
# 强制指定：make CROSS=arm-linux-
CROSS   ?= $(shell command -v arm-linux-gnueabi-gcc >/dev/null 2>&1 && echo arm-linux-gnueabi- || echo arm-linux-)
CC_BOARD = $(CROSS)gcc

# 板子：S5P6818 = 8x Cortex-A53，内核 3.4 + glibc 2.23(armel 软浮点 ABI)
# 静态链接避免 glibc 版本不匹配；用硬件浮点/NEON 指令 + 软浮点 ABI 调用约定提速
# 换用其它工具链：make CROSS=arm-linux-   （粤嵌课程常用的 arm-linux-gcc 5.4.0 也可以）
# 若工具链太老不认 -mcpu=cortex-a53：make ARCH_FLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp"
ARCH_FLAGS ?= -mcpu=cortex-a53 -mfpu=neon -mfloat-abi=softfp
BOARD_CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter \
               $(ARCH_FLAGS) \
               -ffast-math -fno-math-errno \
               -I core -I apps -I third_party

BOARD_LDFLAGS = -static -lpthread -lm

HOST_CFLAGS  = -O2 -g -Wall -Wextra -Wno-unused-parameter -DSHIXI_HOST \
               -I core -I apps -I third_party
HOST_LDFLAGS = -lpthread -lm

SRCS = main.c \
       core/gfx.c core/font.c core/pinyin.c core/image.c core/input.c core/ui.c \
       core/media.c core/camera.c core/avi.c core/music_player.c core/ai.c \
       apps/app_camera.c apps/app_video.c apps/app_gallery.c apps/app_chat.c \
       apps/app_brick.c apps/app_music.c \
       third_party/stb_impl.c

BOARD_BIN = bin/shixi
HOST_BIN  = bin/shixi_host

BOARD ?= root@169.254.193.77
BOARD_DIR ?= /root/shixi

.PHONY: all host deploy run clean tools v4l2mock curl-arm fontprobe pinyin-test mplayer-mock ai-parse-test

all: $(BOARD_BIN)

$(BOARD_BIN): $(SRCS)
	@mkdir -p bin
	$(CC_BOARD) $(BOARD_CFLAGS) $(SRCS) -o $@ $(BOARD_LDFLAGS)
	@echo "==> 板子程序: $@"
	@ls -la $@

host: $(HOST_BIN) bin/v4l2mock.so

# 主机假摄像头（LD_PRELOAD 用），无需板子即可跑通 camera.c 的真实 V4L2 流程
bin/v4l2mock.so: tools/v4l2mock.c
	@mkdir -p bin
	$(CC) -O2 -fPIC -shared -o $@ $< -ldl

$(HOST_BIN): $(SRCS)
	@mkdir -p bin
	$(CC) $(HOST_CFLAGS) $(SRCS) -o $@ $(HOST_LDFLAGS)
	@echo "==> 主机模拟器: $@"

deploy: $(BOARD_BIN)
	sshpass -p 123456 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $(BOARD) 'P=$$(pidof shixi); [ -n "$$P" ] && kill $$P; true; rm -f /tmp/shixi.lock; mkdir -p $(BOARD_DIR)'
	sshpass -p 123456 scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $(BOARD_BIN) $(BOARD):$(BOARD_DIR)/
	@echo "==> 已推送到 $(BOARD):$(BOARD_DIR)/shixi（运行: ./run_board.sh start）"

run:
	sshpass -p 123456 ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $(BOARD) 'cd $(BOARD_DIR) && ./shixi'

# 板端调试小工具（截屏 / 虚拟触摸）
tools:
	@mkdir -p bin
	$(CC_BOARD) -static -O2 -o bin/fbshot tools/fbshot.c
	$(CC_BOARD) -static -O2 -o bin/touchsim tools/touchsim.c
	$(CC_BOARD) -static -O2 -I core -o bin/camtest tools/camtest.c
	$(CC_BOARD) $(BOARD_CFLAGS) -o bin/jpgbench tools/jpgbench.c core/gfx.c core/image.c third_party/stb_impl.c $(BOARD_LDFLAGS)
	$(CC_BOARD) $(BOARD_CFLAGS) -o bin/aitest tools/aitest.c core/ai.c core/media.c core/image.c core/avi.c core/gfx.c third_party/stb_impl.c $(BOARD_LDFLAGS)
	@echo "==> bin/fbshot  bin/touchsim  bin/camtest  bin/jpgbench  bin/aitest"

# 静态 curl（AI 助手连宿主机用）→ bin/curl-arm；需要联网，见 tools/build_curl_arm.sh
curl-arm:
	./tools/build_curl_arm.sh

# AI 助手：宿主机转发服务（板子能连到本机时用；另开一个终端跑，别关）
#   密钥放 tools/ai_secret.conf（已 gitignore），板子侧写 relay_url / relay_key
relay:
	python3 tools/ai_relay.py

# AI 助手：宿主机桥接（板子连不到本机时用；另开一个终端跑，别关）
#   板子侧 ai.conf 写 transport=/root/shixi/ai_bridge_board.sh
bridge:
	./tools/ai_bridge.sh

# 字体回归探针（主机程序）：新引擎 vs 归档图集的度量与观感对照
# archive/font_atlas.h 只作为「版面基准」存在，不再参与板上构建
fontprobe:
	@mkdir -p bin
	$(CC) $(HOST_CFLAGS) -Iarchive -o bin/fontprobe tools/fontprobe.c core/gfx.c core/font.c \
	    third_party/stb_impl.c $(HOST_LDFLAGS)
	@echo "==> bin/fontprobe  （用法：./bin/fontprobe [TTF路径]）"

# AI 回复解析门禁（主机程序，fixtures 在 tools/testdata/，真实抓包）
ai-parse-test:
	@mkdir -p bin
	$(CC) $(HOST_CFLAGS) tools/ai_parse_test.c -o bin/ai_parse_test
	@./bin/ai_parse_test

# 主机模拟器用的 mplayer 替身（说 -slave 协议，见 tools/fake_mplayer.py）
mplayer-mock:
	@mkdir -p bin
	cp tools/fake_mplayer.py bin/mplayer
	chmod +x bin/mplayer
	@echo "==> bin/mplayer（宿主模拟器用的 mplayer 替身；把它所在目录放进 PATH 再跑模拟器）"

# 拼音输入法逻辑自测（主机程序）：音节判定、音节切分、候选顺序
pinyin-test:
	@mkdir -p bin
	$(CC) -O2 -Wall -Wextra -I core -o bin/pinyin_test tools/pinyin_test.c core/pinyin.c
	@./bin/pinyin_test

clean:
	rm -f $(BOARD_BIN) $(HOST_BIN) bin/fbshot bin/touchsim bin/camtest bin/jpgbench bin/v4l2mock.so bin/fontprobe bin/pinyin_test bin/mplayer bin/ai_parse_test bin/aitest
