# 申请一个 MQTT Broker（公网可访问）

> 一句话：我要在公网放一个 **MQTT broker**，让一台嵌入式设备（ESP32-S3）和一台电脑
> 各自**主动出站**连上去，互相收发实时音频。不需要写任何业务接口，就是个现成的服务。

---

## 1. 我要什么

| 项 | 需求 |
|---|---|
| 协议 | **MQTT over TCP**（原生 MQTT，**不要 WebSocket**） |
| 端口 | 默认 **1883**；能上 **8883 + TLS** 最好 |
| 认证 | 一对**用户名 / 密码**，关闭匿名访问 |
| 权限 | 只放开我指定的 topic 前缀，其他全拒 |
| 公网 | 从任意网络（含 4G）能连得上 |
| 持久化 | **不需要**。纯实时转发，不落库 |

## 2. 用量（很小）

| 指标 | 值 |
|---|---|
| 并发连接 | 2（以后最多 3~5） |
| 上行流量 | 峰值 32 KB/s（256 kbps） |
| 下行流量 | 峰值 32 KB/s（256 kbps） |
| 月流量 | 常态几百 MB，上限约 1 GB |
| 消息频率 | 每 60 ms 一条，payload 1921 字节 |
| QoS | 只用 **0**（不用 1/2，不需要重传保证） |
| 保留消息 / 遗嘱 | 不用 |

> 说明：不是常开推流，只在"有人说话"的那几秒才传。一轮对话上下行合计约 200~300 KB。

## 3. 为什么是这个，不是 HTTP

- 需要**双向、持续、低延迟**的音频流（60ms 一包，实时播）；
- 两端都在 NAT 后面，IP 会变 —— MQTT 两边都是出站连接，天然穿透；
- HTTP 一问一答的模型不适合这条路；写 WebSocket 网关也可以，但没必要，多一层自研代码。

---

## 4. 推荐实现（docker，10 分钟）

如果你们没有现成的 MQTT 服务，直接起一个 **mosquitto** 就行：

```yaml
# /opt/mqtt/mosquitto.conf
listener 8883
allow_anonymous false
password_file /mosquitto/passwd
certfile /mosquitto/certs/fullchain.pem
keyfile  /mosquitto/certs/privkey.pem
```

```bash
# 1) 建账号（容器里跑一次，会提示输入密码）
docker run --rm -it -v /opt/mqtt:/m eclipse-mosquitto:2 \
  mosquitto_passwd -c /m/passwd whale

# 2) 起服务
docker run -d --name mqtt --restart=always \
  -p 8883:8883 \
  -v /opt/mqtt/mosquitto.conf:/mosquitto/config/mosquitto.conf \
  -v /opt/mqtt/passwd:/mosquitto/passwd \
  -v /opt/mqtt/certs:/mosquitto/certs \
  eclipse-mosquitto:2
```

### 关于证书

- **首选 Let's Encrypt**（正规 CA 签发）。设备端用的是系统根证书束，
  公共 CA 的证书**不用额外配置**就能校验通过。
- **自签证书不推荐**：要么把证书烧进固件（麻烦），要么关掉校验（等于没加密）。
- **实在嫌麻烦，1883 明文也先跑通**，我们后面再补 TLS。这条路能用，只是内容不加密。

### 关于 ACL（可选，但建议）

如果方便，只给他这几个 topic，其他全拒：

```
topic readwrite whale-1bb62897/#
```

（前缀 `whale-1bb62897` 是**我这边定的**，你照着配就行，不用改名。）

---

## 5. 自测（起来之后验一下）

```bash
# 订阅
mosquitto_sub -h HOST -p 8883 -u USER -P PASS -t 'whale-1bb62897/#' -v

# 另开一个终端发布
mosquitto_pub -h HOST -p 8883 -u USER -P PASS -t 'whale-1bb62897/ping' -m hello
```

Python 一行验证（如果装了 paho-mqtt）：

```bash
python -c "import paho.mqtt.client as m;c=m.Client();c.username_pw_set('USER','PASS');c.connect('HOST',8883);print('ok')"
```

> 如果 `telnet HOST 8883` 从公网都连不上，说明防火墙 / NAT 端口映射还没通。

---

## 6. 起来之后请把这些信息给我

```
host     :  （域名或 IP）
port     :  1883 或 8883
TLS      :  有 / 无（有的话是哪个 CA 签的）
username :
password :
topic    :  whale-1bb62897/#   ← 前缀我定了，直接按这个配 ACL
```

---

## 7. 如果你们已经有现成平台

**阿里云 IoT / EMQX Cloud / 华为云 IoT** 这些也行，但要注意：
平台通常强制"设备三元组 + 自定义 topic 规则"，会多一层改造。**能直接给一个
标准 MQTT broker（mosquitto / EMQX 社区版）最省事。**

---

## 8. 安全上的说明

- 这条链路传的是**麦克风采集的音频**和**合成语音**，属于私人对话内容。
- 所以：**关匿名 + 账号密码**是底线；能上 TLS 就上。
- 不接公网 web 端口，不做任何 HTTP 暴露。
- 只在 broker 内部转发，**不落盘、不进日志正文**。
