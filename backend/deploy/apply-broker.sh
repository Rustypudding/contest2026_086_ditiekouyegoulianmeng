#!/bin/sh
# 把后端导出的设备口令写进 mosquitto 的 password_file，并让 broker 重新加载。
# 用法（在服务器上，root）：
#   ./apply-broker.sh /path/to/broker-out [/etc/mosquitto/passwd] [/etc/mosquitto/acl.conf]
#
# 已存在的用户不会被删（老的手动演示账号还在），新设备追加进去。
# 设备被删除或换密码后，重新跑一次即可。

set -eu

SRC=${1:?用法: apply-broker.sh <broker-out 目录> [passwd 路径] [acl 路径]}
PW=${2:-/etc/mosquitto/passwd}
ACL=${3:-/etc/mosquitto/acl.conf}

PLAIN="$SRC/passwd_plain.txt"
ACLSRC="$SRC/acl.conf"

[ -f "$PLAIN" ] || { echo "缺少 $PLAIN"; exit 1; }
[ -f "$ACLSRC" ] || { echo "缺少 $ACLSRC"; exit 1; }
command -v mosquitto_passwd >/dev/null || { echo "没装 mosquitto_passwd"; exit 1; }

first=1
while IFS=: read -r user pass; do
    [ -n "$user" ] || continue
    if [ "$first" = 1 ] && [ ! -f "$PW" ]; then
        mosquitto_passwd -c -b "$PW" "$user" "$pass"
    else
        mosquitto_passwd -b "$PW" "$user" "$pass"
    fi
    first=0
done < "$PLAIN"
chmod 600 "$PW"

install -m 644 "$ACLSRC" "$ACL"

# 注意：这里的 acl 只放行设备 topic（kb/<用户名>/in|out）。
# 如果还要保留比赛演示用的 kb086/in、kb086/out，在那个文件末尾追加：
#   user kidbuddy
#   topic readwrite kb086/in
#   topic readwrite kb086/out

if pidof mosquitto >/dev/null 2>&1; then
    kill -HUP "$(pidof mosquitto)"
    echo "已重载 mosquitto"
else
    echo "mosquitto 没在跑，文件已更新，启动时生效"
fi
