# 家庭账号后端（方案 A）

给家长端和企业端用的账号服务。**只做身份和路由，不存故事。**

故事正文仍然只存在两个地方：板子的 `/data/ai_agent/sessions/`，和家长手机浏览器的
IndexedDB。后端连一张存对话的表都没有 —— 这不是「存了但没展示」，是结构上就没有。

## 它管什么

| 存 | 不存 |
| --- | --- |
| 家庭、家长邮箱、口令哈希（PBKDF2-SHA256） | 聊天内容 |
| 设备名、设备专属的 broker 账号与 topic | 故事正文、剧情进度、世界状态 |
| 登录与设备操作审计（时间、动作、IP） | 孩子说过的任何话 |
| 会话令牌的 SHA-256 | 明文口令（家长口令不落库；设备 broker 口令要回显，存明文但数据库 0600） |

企业端能看到「这个家庭有几台设备、最近有没有登录」，看不到孩子讲了什么。

## 它怎么工作

```
家长手机 ──HTTPS──> 后端：登录、拿设备清单和这台设备的 broker 口令/topic
   │
   └──WSS──> mosquitto ──> 板子（set_mqtt 配好的专属 topic）
```

每台设备一组独立凭据，topic 是 `kb/<broker用户名>/in` 和 `/out`。broker 用 ACL
`pattern` 限制每个用户只能读写自己的那一对，所以家长 A 连不上家长 B 的板子。
账号服务只在下发参数时出现，之后消息不经过它。

## 本地跑

```bash
cd backend
python3 kb_server.py --init
python3 kb_server.py --create-admin admin@example.com '至少八位口令' --family 运营
KB_HOST=0.0.0.0 KB_PORT=8080 python3 kb_server.py --serve
```

打开 `http://127.0.0.1:8080/` 是家长端（后端会直接托管 `../phone-app`），
`/admin/` 是企业端。本地验证时没有 broker 也能注册、登录、添设备、看审计日志；
真正连板子需要 broker 已经按下面配好。

数据默认在 `backend/data/kb.db`。

## 部署到服务器

1. 放代码：`/opt/kidbuddy/backend`、`/opt/kidbuddy/phone-app`。
2. 建用户与数据目录：

   ```bash
   useradd --system --home /opt/kidbuddy --shell /usr/sbin/nologin kidbuddy
   install -d -o kidbuddy -g kidbuddy -m 700 /var/lib/kidbuddy
   install -d -m 755 /etc/kidbuddy
   cp deploy/backend.env.sample /etc/kidbuddy/backend.env   # 改域名等
   ```

3. 建库和管理员（用 `sudo -u kidbuddy` 跑，保证文件属主对）：

   ```bash
   sudo -u kidbuddy env KB_DB=/var/lib/kidbuddy/kb.db \
     python3 /opt/kidbuddy/backend/kb_server.py --init
   sudo -u kidbuddy env KB_DB=/var/lib/kidbuddy/kb.db \
     python3 /opt/kidbuddy/backend/kb_server.py --create-admin 你的邮箱 '强口令' --family 运营
   ```

4. systemd：把 `deploy/kb-server.service` 放到 `/etc/systemd/system/`，
   `systemctl enable --now kb-server`。
5. nginx：参考 `deploy/nginx.conf.sample`。**账号一旦上公网就必须走 HTTPS**，
   否则口令是明文传输。certbot 签证书即可。
6. broker：`mosquitto.conf` 里加上

   ```
   password_file /etc/mosquitto/passwd
   acl_file     /etc/mosquitto/acl.conf
   ```

   然后每次企业端/家长端增删设备、换密码之后，在服务器上跑：

   ```bash
   sudo -u kidbuddy env KB_DB=/var/lib/kidbuddy/kb.db \
     python3 /opt/kidbuddy/backend/kb_server.py --export-broker /tmp/broker-out
   sudo ./deploy/apply-broker.sh /tmp/broker-out
   rm -f /tmp/broker-out/passwd_plain.txt
   ```

   `apply-broker.sh` 会把设备追加进 password_file 并 `SIGHUP` 重载，
   **不会删掉已有用户** —— 比赛演示用的 `kidbuddy`/`kb086` 老账号可以留着。
   但它会用新 ACL 覆盖 `acl.conf`，老演示 topic 要在文件末尾手工补两行，
   脚本里已经写了注释。

## 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/api/register` | 注册家庭（首个家长账号） |
| POST | `/api/login` / `/api/logout` | 登录 / 退出 |
| GET | `/api/me` | 当前账号、家庭、设备清单 |
| GET/POST/DELETE | `/api/devices` | 设备列表 / 新增 / 删除 |
| POST | `/api/devices/<id>/provision` | 取这台设备的连接命令与 broker 参数 |
| POST | `/api/devices/<id>/rotate` | 换设备 broker 口令 |
| GET | `/api/admin/stats` | 家庭/账号/设备数、近 7 天登录 |
| GET | `/api/admin/families` | 家庭与账号列表 |
| GET | `/api/admin/audit?limit=N` | 审计日志 |
| GET | `/api/admin/families/<id>/export` | 导出该家庭元数据（没有故事） |
| POST | `/api/admin/families/<id>/purge` | 删除家庭（body 要 `{"confirm":"DELETE"}`） |
| POST | `/api/admin/accounts/<id>/disable` | 停用 / 启用账号 |

## 安全清单

- 公网必须 HTTPS；`KB_SECURE=1`，`KB_PUBLIC_ORIGIN` 填前端域名。
- 后端默认只监听 `127.0.0.1`，由 nginx 反代；`X-Real-IP` 只在 nginx 后面可信。
- `kb.db` 权限 600，放在 `ProtectSystem=strict` 之外的可写目录。
- 家长口令用 PBKDF2-SHA256（20 万轮）；会话令牌 32 字节随机，库里只存哈希。
- 登录失败限流：同一 IP 10 分钟 10 次。
- 设备 broker 口令需要回显给家长，所以是明文存储；设备被删除/换密码后，
  旧的 broker 凭据要在 `apply-broker.sh` 里同步清掉（当前脚本只增不删，
  换密码会覆盖同一用户，删除设备需要手工从 password_file 里删）。
- 定期备份 `kb.db`；审计日志默认保留最近 20 万条。

## 本机验证过什么

HTTP 侧（curl，两个家庭 + 一个管理员）：

- 注册、登录、退出、取设备清单、生成 `set_mqtt` 命令；
- 家庭 B 读/轮换家庭 A 的设备 → 404，拿不到别人的东西；
- 管理员停用账号后，旧会话立刻失效（`/api/me` 401），登录返回 403；
- 同一 IP 连续 10 次口令错误后，第 11 次开始 429；
- 删除家庭要求 `confirm=DELETE`，删完账号登不上、设备也没了；
- 审计日志记录了注册、登录、失败、设备操作和管理员操作；
- `/api/*` 不缓存（`Cache-Control: no-store`），页面 service worker 也绕开了 `/api/`。

MQTT 侧（把 mosquitto 2.0.11 拆到 /tmp 起了一个，两个家庭各一台设备）：

| 动作 | 结果 |
| --- | --- |
| 设备 A 写自己的 `kb/A/in`、`kb/A/out` | 成功 |
| A 订阅自己的 `kb/A/out`，另一客户端用 A 的凭据发布 | 收到 |
| A 往 `kb/B/in` 发布 | broker 日志 `Denied PUBLISH` |
| A 订阅 `kb/B/out` | 收不到任何消息 |
| A 订阅通配符 `kb/+/out`，A、B 各发一条 | 只收到 A 自己那条 |
| 错误口令连接 | `Connection Refused: not authorised` |

> 这一步抓到一个真问题：最初的 ACL 是「写 in、读 out」，但板子和手机共用同一组
> 设备凭据，板子往 out 发会被拒。已改成 in/out 都 `readwrite`。

## 还没做

- 设备在线状态和最后上线时间（要后端订阅 broker 或板子上报心跳）。
- 家长邀请（一个家庭多个账号目前只能靠管理员手工加）。
- 管理员网页之外的企业端功能：账单、告警、按家庭限流。
- 密码找回（需要发信服务）。
- 删除设备时同步清 broker 用户（见上面的安全清单）。
