#!/bin/sh
# ============================================================
#  ai_bridge.sh — 在电脑上运行：把开发板的 AI 请求转发到云端模型
#
#  什么时候用它：
#    · 板子能直接访问宿主机 relay（tools/ai_relay.py）时，用 relay 更省事；
#    · 但有些环境板子根本连不到电脑（例如 Windows 防火墙挡了入站、WSL2 里
#      端口转发没配），这时用本脚本：只借用「电脑 -> 板子」这个已经通的 SSH 方向。
#
#  板子侧配置（$SHIXI_ROOT/ai.conf）：
#      transport=/root/shixi/ai_bridge_board.sh
#  本机侧配置（tools/ai_secret.conf，**已在 .gitignore 里**）：
#      endpoint=https://api.example.com/v1/chat/completions
#      api_key=<云端密钥>
#      model=<模型名>            # 可选，覆盖板子传来的 model
#  也可用环境变量：SHIXI_API_ENDPOINT / SHIXI_API_KEY / SHIXI_MODEL
#
#  用法（开一个终端挂着，别关）：
#      ./tools/ai_bridge.sh
#  可选：BOARD=root@1.2.3.4  PW=123456  BOARD_DIR=/root/shixi  POLL=0.5
# ============================================================
set -u

BOARD="${BOARD:-root@169.254.193.77}"
PW="${PW:-123456}"
BOARD_DIR="${BOARD_DIR:-/root/shixi}"
POLL="${POLL:-0.5}"
HERE=$(cd "$(dirname "$0")" && pwd)
CONF="${SHIXI_RELAY_CONF:-$HERE/ai_secret.conf}"

REQ="$BOARD_DIR/tmp/ai_bridge_req.json"     # 板子 -> 电脑
RESP="$BOARD_DIR/tmp/ai_bridge_resp.txt"    # 电脑 -> 板子

# --- SSH：优先 sshpass，没有就用 OpenSSH 的 SSH_ASKPASS（与 run_board.sh 同策略）---
if command -v sshpass >/dev/null 2>&1; then
    SSH="sshpass -p $PW ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 $BOARD"
else
    ASKPASS=$(mktemp)
    printf '#!/bin/sh\necho "%s"\n' "$PW" > "$ASKPASS"
    chmod +x "$ASKPASS"
    SSH="env SSH_ASKPASS=$ASKPASS SSH_ASKPASS_REQUIRE=force setsid -w ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 $BOARD"
fi

# --- 配置 ---
API_URL="${SHIXI_API_ENDPOINT:-}"
API_KEY="${SHIXI_API_KEY:-}"
API_MODEL="${SHIXI_MODEL:-}"
if [ -f "$CONF" ]; then
    [ -n "$API_URL" ]   || API_URL=$(sed -n 's/^endpoint=//p' "$CONF" | head -1)
    [ -n "$API_KEY" ]   || API_KEY=$(sed -n 's/^api_key=//p'  "$CONF" | head -1)
    [ -n "$API_MODEL" ] || API_MODEL=$(sed -n 's/^model=//p'  "$CONF" | head -1)
fi

if [ -z "$API_KEY" ] || [ -z "$API_URL" ]; then
    echo "错误：没有配置云端接口。请在 $CONF 里写" >&2
    echo "      endpoint=https://.../v1/chat/completions" >&2
    echo "      api_key=<你的密钥>" >&2
    echo "      （该文件已在 .gitignore 中，不会被提交）" >&2
    exit 1
fi

echo "AI 桥接已启动"
echo "  开发板:   $BOARD"
echo "  转发到:   $API_URL"
echo "  模型:     ${API_MODEL:-（用板子请求里的）}"
echo "  轮询:     ${REQ}"
echo "保持这个终端开着；板子上发消息就会自动转发。Ctrl+C 退出。"
echo

COUNT=0
while :; do
    # 一次 SSH：有请求就原子取走（mv 成 .proc 再 cat，避免重复处理）
    BODY=$($SSH "if [ -s $REQ ]; then mv $REQ $REQ.proc 2>/dev/null && cat $REQ.proc; fi" 2>/dev/null)
    if [ -n "${BODY:-}" ]; then
        COUNT=$((COUNT + 1))
        NBYTES=$(printf '%s' "$BODY" | wc -c)
        # 可选：覆盖模型名。
        # 注意模型名里带 /（deepseek/xxx），sed 的 s/// 分隔符必须换成 |，
        # 否则 sed 会报错并把请求体清空（表现为云端说 "not valid JSON"）。
        if [ -n "$API_MODEL" ]; then
            M=$(printf '%s' "$API_MODEL" | tr -d '|')
            NEWBODY=$(printf '%s' "$BODY" | sed "s|\"model\":\"[^\"]*\"|\"model\":\"$M\"|")
            if [ -n "$NEWBODY" ]; then BODY="$NEWBODY"
            else echo "    !! 模型名替换失败，按板子请求里的模型发出" >&2; fi
        fi
        printf '%s' "$BODY" > /tmp/ai_bridge_last_req.json 2>/dev/null
        echo "[$COUNT] 收到请求（${NBYTES} 字节），调用模型…（已存 /tmp/ai_bridge_last_req.json）"
        START=$(date +%s)
        JSON=$(printf '%s' "$BODY" | curl -s -m 120 -X POST "$API_URL" \
                 -H "Authorization: Bearer $API_KEY" \
                 -H "Content-Type: application/json" \
                 --data-binary @- 2>/dev/null)
        END=$(date +%s)
        [ -z "$JSON" ] && JSON='{"error":{"message":"curl 转发失败（电脑网络不通？）"}}'
        printf '%s' "$JSON" | $SSH "cat > $RESP.tmp && mv $RESP.tmp $RESP; rm -f $REQ.proc" 2>/dev/null
        ANSWER=$(printf '%s' "$JSON" | sed -n 's/.*"content":"\([^"]*\)".*/\1/p' | head -c 100)
        echo "    完成（$((END - START)) 秒）：${ANSWER:-（无 content 或出错）}"
    fi
    sleep "$POLL"
done
