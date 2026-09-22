#!/bin/sh
# ============================================================
#  ai_bridge_board.sh — 板端「传输命令」：把请求文件交给宿主机，等回包
#
#  由 core/ai.c 通过 ai.conf 的 transport= 调用（见 README「AI 助手」）：
#      transport=/root/shixi/ai_bridge_board.sh
#  ai.c 会通过环境变量告诉它文件在哪：
#      $SHIXI_AI_REQ    请求体 JSON（板子写好的，ai.c 负责生成）
#      $SHIXI_AI_RESP   响应体要写到这里
#      $SHIXI_AI_TIMEOUT 超时秒数
#
#  工作方式：把请求搬到 tmp/ai_bridge_req.json（宿主机脚本会来取），
#  等 tmp/ai_bridge_resp.txt 出现，把它搬到 $SHIXI_AI_RESP，
#  最后往 stdout 打一个 HTTP 状态码（ai.c 靠它判断成功/失败）。
#
#  这条路用在「板子连不到宿主机 relay」的场合（例如 Windows 防火墙挡住入站），
#  只用电脑 -> 板子这个方向的 SSH，不需要任何端口放通。
# ============================================================
ROOT="${SHIXI_ROOT:-/root/shixi}"
REQ_OUT="$ROOT/tmp/ai_bridge_req.json"
RESP_IN="$ROOT/tmp/ai_bridge_resp.txt"
TIMEOUT="${SHIXI_AI_TIMEOUT:-20}"

if [ -z "${SHIXI_AI_REQ:-}" ] || [ -z "${SHIXI_AI_RESP:-}" ]; then
    echo "ai_bridge_board: 缺少 SHIXI_AI_REQ / SHIXI_AI_RESP" >&2
    echo 500
    exit 0
fi

mkdir -p "$ROOT/tmp"
rm -f "$RESP_IN"

# 1. 把请求交给宿主机（宿主机脚本轮询这个文件）
cp "$SHIXI_AI_REQ" "$REQ_OUT" || { echo "ai_bridge_board: 无法写 $REQ_OUT" >&2; echo 500; exit 0; }

# 2. 等回包（0.5 秒一轮）
i=0
while [ "$i" -lt "$((TIMEOUT * 2))" ]; do
    [ -s "$RESP_IN" ] && break
    sleep 0.5
    i=$((i + 1))
done

if [ ! -s "$RESP_IN" ]; then
    echo "ai_bridge_board: 等不到宿主机回包（电脑上 ai_bridge.sh 在跑吗？）" >&2
    echo 504
    exit 0
fi

# 3. 搬给 ai.c
cp "$RESP_IN" "$SHIXI_AI_RESP" || { echo "ai_bridge_board: 无法写 $SHIXI_AI_RESP" >&2; echo 500; exit 0; }
echo 200
