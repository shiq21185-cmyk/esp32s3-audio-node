#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""身体的可调参数：一份 JSON，控制台和 body_talk.py 共用。

为什么要单独一个文件：
  控制台（qq-bridge-deploy 的网页）负责让人点点鼠标就能改音量、改停顿判定；
  body_talk.py 是另一个进程，改完要**当场生效**，不能要求重启。

落在哪：
  默认 `qq-bridge-deploy/state/body-config.json`（和控制台同一份 state）。
  换位置用环境变量 BODY_CONFIG 指过去。

读的规矩：
  · 只读文件，读不到/坏了就用默认值，**绝不抛异常把耳朵搞挂**；
  · 带 mtime 缓存，但每轮开始会 `load(fresh=True)`，所以控制台改完下一轮就生效。
"""
import json
import os
import sys
import time
from pathlib import Path

BRIDGE = Path(r"C:\Users\Administrator\Desktop\QQ bot\qq-bridge-deploy")

DEFAULT_PATH = Path(os.environ.get("BODY_CONFIG") or (BRIDGE / "state" / "body-config.json"))

# 默认值要跟控制台里那份"重置为默认"完全一致，改一边就得改另一边。
DEFAULTS = {
    "volume": 0.85,        # 0.0~1.5，1.0 = 原始音量（超过 1 会被削顶，听感会硬）
    "silenceSec": 2.0,     # 停顿多久算你说完了
    "maxSec": 25.0,        # 单句硬上限（从你开口那一刻起算）
    "waitSec": 120.0,      # 一直没人说话，等这么久就先停（--loop 时会重新开）
    "cue": True,           # 开口前那声"叮"
    # 声音走公网 MQTT 之后，定位板子靠的是"主题前缀 + broker"，不再是 IP。
    # base 必须和固件 config.h 的 TOPIC_BASE 一模一样，否则两边各说各话。
    "base": "whale-1bb62897",
    "broker": "i1e78188.ala.cn-hangzhou.emqxsl.cn",
    "brokerPort": 8883.0,
    "brokerTls": True,
    # ⚠️ MQTT 账号密码：**空字符串表示"没有"**（公共 broker 就留空）。
    # 它们只落在本机的 state/body-config.json（不进 git、不写进任何 .py/.h 源码），
    # 因为 rot/tools 与固件源码都可能被贴出去或推到公开仓库。
    # 也可以改用环境变量 BODY_MQTT_USER / BODY_MQTT_PASS 覆盖（优先级更高）。
    "mqttUser": "",
    "mqttPass": "",
    # 板子的工作模式（这个值 python 不直接用，是控制台存下来、bridge 推给板子的）：
    #   gate = 室内，本地人声门限，有人说话才上传
    #   btn  = 随身，只有按键才上传（外面吵的时候门限不可靠，交给手指）
    "boardMode": "gate",
}

_cache = {"path": None, "mtime": 0.0, "data": None}


def path():
    return DEFAULT_PATH


def _coerce(raw):
    """把 JSON 里的东西掰成对的类型；有问题的字段退回默认值。"""
    out = dict(DEFAULTS)
    if not isinstance(raw, dict):
        return out
    for key, default in DEFAULTS.items():
        if key not in raw:
            continue
        val = raw[key]
        try:
            if isinstance(default, bool):
                if isinstance(val, bool):
                    out[key] = val
                elif isinstance(val, (int, float)):
                    out[key] = bool(val)
                elif isinstance(val, str):
                    out[key] = val.strip().lower() in ("1", "true", "on", "yes")
            elif isinstance(default, float):
                out[key] = float(val)
            elif isinstance(default, str):
                out[key] = str(val).strip() or default
        except (TypeError, ValueError):
            pass
    #  sane 化：这些值越界会让体验直接崩，钳一下比报错好
    out["volume"] = min(max(out["volume"], 0.0), 1.5)
    out["silenceSec"] = min(max(out["silenceSec"], 0.5), 8.0)
    out["maxSec"] = min(max(out["maxSec"], 3.0), 120.0)
    out["waitSec"] = min(max(out["waitSec"], 5.0), 600.0)
    out["boardMode"] = "btn" if str(out.get("boardMode", "gate")).strip().lower() == "btn" else "gate"
    return out


def load(fresh=False):
    """读配置。永远返回一份完整可用的 dict。"""
    p = DEFAULT_PATH
    try:
        mtime = p.stat().st_mtime
    except OSError:
        return dict(DEFAULTS)
    if not fresh and _cache["data"] is not None and _cache["path"] == str(p) \
            and _cache["mtime"] == mtime:
        return dict(_cache["data"])
    try:
        with open(p, "r", encoding="utf-8") as f:
            data = _coerce(json.load(f))
    except Exception as exc:
        print(f"（身体配置读不了，用默认值：{exc}）", file=sys.stderr)
        return dict(DEFAULTS)
    _cache.update({"path": str(p), "mtime": mtime, "data": data})
    return dict(data)


def save(cfg):
    """控制台用不到（它自己写），留在这儿方便命令行调试。原子写。"""
    p = DEFAULT_PATH
    p.parent.mkdir(parents=True, exist_ok=True)
    data = _coerce(cfg)
    tmp = p.with_suffix(p.suffix + f".tmp{os.getpid()}")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.replace(tmp, p)
    _cache.update({"path": str(p), "mtime": p.stat().st_mtime, "data": data})
    return data


def credentials(fresh=True):
    """返回 (user, password)，没有账号时两者都是 None。

    为什么单独一个函数、且优先级是这样：
      · 环境变量最高（临时调试、CI 用，不落盘）；
      · 其次 state/body-config.json 的 mqttUser / mqttPass（控制台存的本机文件）；
      · 都没有 ⇒ (None, None)，也就是匿名连公共 broker 的老路子。
    绝不要把默认账号密码写进源码：rot/tools 与 esp32s3-audio-node 都可能被推公开。
    """
    u = os.environ.get("BODY_MQTT_USER")
    pw = os.environ.get("BODY_MQTT_PASS")
    if u:
        return u, (pw or "")
    cfg = load(fresh=fresh)
    u = str(cfg.get("mqttUser") or "").strip()
    pw = str(cfg.get("mqttPass") or "")
    if u:
        return u, pw
    return None, None


def link_kwargs(fresh=True):
    """给 body_mqtt.Link 打包一套连接参数（broker / 端口 / TLS / 账号）。

    所有工具都走这里，换 broker 就只改一份配置 —— 别再散落在各处。
    """
    cfg = load(fresh=fresh)
    user, password = credentials(fresh=fresh)
    kw = {
        "base": cfg["base"],
        "broker": cfg["broker"],
        "port": int(cfg["brokerPort"]),
        "use_tls": bool(cfg["brokerTls"]),
    }
    if user:
        kw["user"] = user
        kw["password"] = password
    return kw


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "reset":
        print(json.dumps(save(DEFAULTS), ensure_ascii=False, indent=2))
    else:
        print(f"配置文件：{DEFAULT_PATH}")
        print(json.dumps(load(fresh=True), ensure_ascii=False, indent=2))
