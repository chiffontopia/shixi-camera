#!/bin/sh
# ============================================================
#  run_board.sh — 一键推送到开发板并运行
#
#  用法:
#    ./run_board.sh            编译 + 推送 + 启动（后台运行，开调试通道）
#    ./run_board.sh stop       停止开发板上的程序
#    ./run_board.sh logs       查看运行日志
#    ./run_board.sh shot       截屏并转成 PNG（需要本机有 python3+PIL）
#    ./run_board.sh tap X Y    模拟点击（需要程序以调试模式启动）
#
#  环境变量: BOARD=root@IP  BOARD_DIR=/root/shixi  PW=密码
#  依赖: sshpass 或 OpenSSH 的 askpass（本机没装 sshpass 会自动用后者）
# ============================================================
set -e
BOARD="${BOARD:-root@169.254.193.77}"
BOARD_DIR="${BOARD_DIR:-/root/shixi}"
PW="${PW:-123456}"
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
HERE="$(cd "$(dirname "$0")" && pwd)"

# 本机装了 sshpass 就用它；没装就退到 OpenSSH 自带的 askpass（Ubuntu/WSL 上常见）
# 注意：scp 必须带 -O（新版 openssh 默认 sftp 协议，老开发板不支持）
if command -v sshpass >/dev/null 2>&1; then
    SSH="sshpass -p $PW ssh $SSHOPT $BOARD"
    SCP="sshpass -p $PW scp -O $SSHOPT"
else
    ASKPASS="$(mktemp /tmp/shixi_askpass.XXXXXX)"
    printf '#!/bin/sh\necho %s\n' "$PW" > "$ASKPASS"
    chmod +x "$ASKPASS"
    trap 'rm -f "$ASKPASS"' EXIT
    export SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force
    SSH="setsid -w ssh $SSHOPT $BOARD"
    SCP="setsid -w scp -O $SSHOPT"
fi

case "${1:-start}" in
stop)
    $SSH 'P=$(pidof shixi); [ -n "$P" ] && kill $P; true; rm -f /tmp/shixi.lock; echo 已停止' 
    ;;
logs)
    $SSH "tail -30 /tmp/shixi.log"
    ;;
shot)
    NAME="${2:-/tmp/board_shot}"
    $SSH "FBSHOT=$BOARD_DIR/fbshot; [ -x \$FBSHOT ] || FBSHOT=/tmp/fbshot; \$FBSHOT $NAME.raw >/dev/null && echo 已抓取 $NAME.raw"
    $SCP "$BOARD:$NAME.raw" "$NAME.raw" >/dev/null
    if command -v python3 >/dev/null 2>&1; then
        python3 "$HERE/tools/fb2png.py" "$NAME.raw"
    fi
    ;;
tap)
    $SSH "echo 'tap $2 $3' > /tmp/shixi_ctrl; echo 已注入 tap $2 $3"
    ;;
start)
    echo "==> 编译"
    make -C "$HERE"
    echo "==> 停掉板子上正在运行的程序"
    # 用 -x 精确匹配进程名：用 -f 匹配路径会连自己这条 ssh 会话一起杀掉
    $SSH "P=\$(pidof shixi); [ -n \"\$P\" ] && kill \$P; true; rm -f /tmp/shixi.lock; mkdir -p $BOARD_DIR" >/dev/null
    sleep 1
    echo "==> 上传到 $BOARD:$BOARD_DIR"
    $SCP "$HERE/bin/shixi" "$BOARD:$BOARD_DIR/" >/dev/null
    # AI 助手要用板上的静态 curl 连宿主机；没有就先用 tools/build_curl_arm.sh 生成
    if [ -f "$HERE/bin/curl-arm" ]; then
        $SCP "$HERE/bin/curl-arm" "$BOARD:$BOARD_DIR/curl" >/dev/null
        echo "    已附带 curl（$(du -h "$HERE/bin/curl-arm" | cut -f1)）"
    else
        echo "    提示：bin/curl-arm 不存在，AI 助手所需的 curl 未推送（见 tools/build_curl_arm.sh）"
    fi
    # 字体：运行时用 stb_truetype 栅格化，板上缺了就是红屏退出，所以必须一起推并校验
    if [ -f "$HERE/tools/SimHei.ttf" ]; then
        $SCP "$HERE/tools/SimHei.ttf" "$BOARD:$BOARD_DIR/SimHei.ttf" >/dev/null
        echo "    已附带字体 SimHei.ttf（$(du -h "$HERE/tools/SimHei.ttf" | cut -f1)）"
    else
        echo "    错误：tools/SimHei.ttf 不存在，板上程序会因缺字体红屏" >&2
        exit 1
    fi
    $SSH "test -f $BOARD_DIR/SimHei.ttf" || { echo "    错误：字体未就位（$BOARD_DIR/SimHei.ttf）" >&2; exit 1; }
    echo "==> 启动（后台，带调试输入通道）"
    $SSH "cd $BOARD_DIR && chmod +x shixi && SHIXI_DEBUG_INPUT=1 setsid nohup ./shixi > /tmp/shixi.log 2>&1 < /dev/null & sleep 2; cat /tmp/shixi.log"
    echo "==> 完成。日志: ./run_board.sh logs   截屏: ./run_board.sh shot   模拟点击: ./run_board.sh tap 400 430"
    ;;
*)
    echo "用法: $0 [start|stop|logs|shot|tap X Y]"
    ;;
esac
