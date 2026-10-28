"""把板子的工作模式切过去(室内/随身)，并等它回一条 state 确认。

用法：
    python tools/body_mode.py gate     # 室内：本地人声门限，有人说话才上传
    python tools/body_mode.py btn      # 随身：只有按键才上传
    python tools/body_mode.py          # 只查当前是什么模式

退出码：0 = 切成功(或查询成功)；1 = 板子没回话。
控制台「身体 · 听你说话」那张卡保存时会调它，不用手动跑。
"""
import argparse
import sys
import time

import body_mqtt
import body_config

MODES = {"gate": 0, "btn": 1}


def main():
    ap = argparse.ArgumentParser(description="切换板子的工作模式")
    ap.add_argument("mode", nargs="?", choices=sorted(MODES), help="gate=室内 / btn=随身")
    ap.add_argument("--base", default=None)
    ap.add_argument("--broker", default=None)
    ap.add_argument("--timeout", type=float, default=8.0)
    a = ap.parse_args()

    # 连接参数（broker / 端口 / TLS / 账号）统一从配置取，别在这里散落常量。
    kw = body_config.link_kwargs()
    if a.base:
        kw["base"] = a.base
    if a.broker:
        kw["broker"] = a.broker

    link = body_mqtt.Link(**kw)
    try:
        link.connect(timeout=a.timeout)

        # 心跳是 30 秒一条，不能干等它 —— 主动敲一条 PING 让板子立刻回 state。
        # 敲两次：第一次可能正好撞上板子启动/重连（实测偶发），退一步再问一次。
        st = None
        for attempt in range(2):
            n0 = link._state_n
            link.client.publish(link.t_cmd, "PING", qos=0)
            t0 = time.time()
            while time.time() - t0 < min(6.0, a.timeout):
                if link._state_n != n0:
                    st = link.state
                    break
                time.sleep(0.05)
            if st:
                break
            time.sleep(0.6)
        if not st:
            print("板子没回话(连不上或没上电)")
            return 1

        if a.mode is None:
            print(f"当前模式：{st.get('mode')}(0=室内 1=随身)")
            return 0

        want = MODES[a.mode]
        if st.get("mode") == want:
            print(f"已经是{a.mode}模式，不用改")
            return 0

        link.client.publish(link.t_cmd, f"MODE {want}", qos=0)
        # 等的不是"随便来一条 state"，而是"mode 变成我要的那个"。
        # 只等"有新 state"会被上一条 PING 的 pong 骗过去（它带的是旧模式），
        # 于是每次都慢一拍、看起来像切换失败。
        t0 = time.time()
        while time.time() - t0 < a.timeout:
            st = link.state
            if st.get("mode") == want:
                print(f"已切到{a.mode}模式({time.time() - t0:.2f}s)")
                return 0
            time.sleep(0.05)
        print(f"切换失败：板子还是 mode={link.state.get('mode')}")
        return 1
        return 0
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
