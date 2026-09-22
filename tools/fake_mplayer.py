#!/usr/bin/env python3
"""
fake_mplayer.py — 主机模拟器用的 mplayer 替身（**只给主机跑，不参与板上构建**）

板上有真的 `/bin/mplayer`，主机上没有；而宿主模拟器要验证音乐应用的
播放/暂停/切歌/进度条状态机，就需要一个能说 `-slave` 协议的对端。
这个替身只实现 `core/music_player.c` 用到的那几条：

  get_time_pos       -> ANS_TIME_POSITION=<秒>
  get_time_length    -> ANS_LENGTH=<秒>
  volume N 1         -> 忽略（真 mplayer 会设 mixer）
  seek N 2           -> 跳到第 N 秒（绝对）
  quit               -> 退出

暂停是 SIGSTOP/SIGCONT（真 mplayer 1.0rc2 只能这么暂停）；收到 SIGCONT 时
把计时基准重置，所以停住的那段不会算进播放进度 —— 与真实行为一致。

用法（宿主模拟器）：
    cp tools/fake_mplayer.py bin/mplayer && chmod +x bin/mplayer
    PATH=$PWD/bin:$PATH MP_DIR=/tmp/mus ./bin/shixi_host
"""
import os
import signal
import sys
import time

LENGTH = 185.0          # 每首固定 185 秒，够看进度条走动
resync = False          # SIGCONT 置位：主循环把计时基准挪到"现在"


def on_cont(_sig, _frm):
    """SIGCONT：告诉主循环重设基准，暂停的那段不算进进度"""
    global resync
    resync = True


def main():
    global resync
    args = sys.argv[1:]
    fifo = None
    track = None
    for i, a in enumerate(args):
        if a == "-input" and i + 1 < len(args):
            opt = args[i + 1]
            if opt.startswith("file="):
                fifo = opt[5:]
        elif not a.startswith("-"):
            track = a

    if not fifo or not track:
        sys.stderr.write("fake_mplayer: 需要 -input file=<fifo> 和曲目路径\n")
        return 2
    if not os.path.exists(track):
        sys.stderr.write("fake_mplayer: 找不到曲目 %s\n" % track)
        return 2

    signal.signal(signal.SIGCONT, on_cont)
    # 后端以 O_RDWR 持有管道，这里只读打开不会阻塞
    fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    os.set_blocking(fd, False)

    sys.stdout.write("fake_mplayer: 播放 %s（%g 秒）\n" % (track, LENGTH))
    sys.stdout.flush()

    pos = 0.0
    last = time.monotonic()
    buf = b""
    while True:
        if resync:               # 刚从暂停恢复：不计暂停时长
            resync = False
            last = time.monotonic()
        now = time.monotonic()
        step = now - last
        last = now
        if step > 0.25:          # 万一漏了 SIGCONT，也不许一次跳太多
            step = 0.25
        pos += step
        if pos >= LENGTH:        # 播完自己退出（真 mplayer 不带 -idle 就这样）
            sys.stdout.write("fake_mplayer: 播放结束\n")
            sys.stdout.flush()
            return 0

        try:
            data = os.read(fd, 512)
        except BlockingIOError:
            data = b""
        if data:
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.decode("utf-8", "replace").strip()
                if cmd == "quit":
                    sys.stdout.write("fake_mplayer: 收到 quit\n")
                    sys.stdout.flush()
                    return 0
                if cmd.startswith("get_time_pos"):
                    sys.stdout.write("ANS_TIME_POSITION=%.2f\n" % pos)
                    sys.stdout.flush()
                elif cmd.startswith("get_time_length"):
                    sys.stdout.write("ANS_LENGTH=%.2f\n" % LENGTH)
                    sys.stdout.flush()
                elif cmd.startswith("seek"):
                    parts = cmd.split()
                    if len(parts) >= 2:
                        try:
                            pos = float(parts[1])
                        except ValueError:
                            pass
        time.sleep(0.05)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
