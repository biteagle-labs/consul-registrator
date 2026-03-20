# Consul Registrator

[English](README.md) | 中文

轻量级 Docker 服务注册器，使用 C 语言编写。监听 Docker 容器启停事件，自动向 Consul Agent 注册/注销服务。

基于 Alpine Linux + libcurl + cJSON 构建，单二进制文件，极小资源占用。

## 特性

- **Opt-in 模式** — 仅设置了 `CONSUL_LISTEN_ENABLE=true` 的容器才会被注册
- **双通道配置** — 优先读取容器环境变量，其次读取 labels
- **自动端口检测** — `CONSUL_SERVICE_PORT` > HostPort 端口映射 > EXPOSE > 默认 8080
- **智能地址解析** — 有端口映射的容器使用宿主机 IP，bridge/自定义网络容器使用容器 IP
- **按端口配置服务** — 通过 `CONSUL_SERVICE_<port>_NAME` 为每个容器注册多个服务
- **定时全量同步** — 按间隔全量同步，捕获遗漏事件并清理孤立服务
- **三线程架构** — 事件流 + 事件处理器 + 全量同步，互不阻塞

## 快速开始

### docker-compose.yaml（推荐）

```yaml
services:
  registrator:
    image: biteagle/consul-registrator:latest
    container_name: registrator
    network_mode: host
    restart: always
    volumes:
      - /var/run/docker.sock:/var/run/docker.sock:ro
    environment:
      - REGISTRATOR_TOKEN=${REGISTRATOR_TOKEN}
      - CONSUL_ADDR=${CONSUL_ADDR:-http://localhost:8500}
      - RESYNC_INTERVAL=${RESYNC_INTERVAL:-30}
      - DEFAULT_PORT=${DEFAULT_PORT:-8080}
      - ADVERTISE_ADDR=${ADVERTISE_ADDR:-}
    logging:
      driver: json-file
      options:
        max-size: "10m"
        max-file: "3"
```

```bash
# 设置 Consul ACL token
export REGISTRATOR_TOKEN=your-consul-acl-token

# 启动
docker compose up -d

# 查看日志
docker compose logs -f registrator
```

### docker run

```bash
docker run -d \
  --name registrator \
  --network host \
  --restart always \
  -v /var/run/docker.sock:/var/run/docker.sock:ro \
  -e REGISTRATOR_TOKEN=your-consul-acl-token \
  -e CONSUL_ADDR=http://localhost:8500 \
  biteagle/consul-registrator:latest
```

## 注册服务

通过环境变量或 label 为容器添加 `CONSUL_LISTEN_ENABLE=true`：

```bash
# 通过环境变量
docker run -d -p 8080:80 \
  -e CONSUL_LISTEN_ENABLE=true \
  -e CONSUL_SERVICE_NAME=my-web \
  -e CONSUL_SERVICE_TAGS=v1,production \
  --name my-web nginx:alpine

# 通过 labels
docker run -d -p 8080:80 \
  -l CONSUL_LISTEN_ENABLE=true \
  -l CONSUL_SERVICE_NAME=my-web \
  -l CONSUL_SERVICE_TAGS=v1,production \
  --name my-web nginx:alpine
```

**未设置** `CONSUL_LISTEN_ENABLE=true` 的容器将被忽略。

## 容器配置项

通过容器环境变量（`-e`）或 labels（`-l`）设置，环境变量优先。

| 键 | 说明 | 默认值 |
|---|------|--------|
| `CONSUL_LISTEN_ENABLE` | 设置为 `true` 启用注册（必须设置） | `false` |
| `CONSUL_SERVICE_NAME` | Consul 中的服务名 | 镜像名 |
| `CONSUL_SERVICE_TAGS` | 逗号分隔的标签 | _（空）_ |
| `CONSUL_SERVICE_PORT` | 覆盖自动检测的端口 | 自动检测 |
| `CONSUL_SERVICE_<port>_NAME` | 指定端口的服务名 | 同上 |
| `CONSUL_SERVICE_<port>_TAGS` | 指定端口的标签 | 同上 |

### 端口检测优先级

1. `CONSUL_SERVICE_PORT` 环境变量 / label
2. Docker 端口映射（`HostPort`）
3. Dockerfile 中的 `EXPOSE` 指令
4. 默认端口（8080）

### 地址解析

注册到 Consul 的服务地址自动确定：

| 场景 | 地址 |
|------|------|
| 有端口映射的容器（`-p`） | 宿主机 IP（自动检测或 `ADVERTISE_ADDR`） |
| bridge/自定义网络容器（无端口映射） | 容器 IP |
| `network_mode: host` 的容器 | 宿主机 IP |

宿主机 IP 检测优先级：`ADVERTISE_ADDR` 环境变量 > `getaddrinfo(hostname)` > UDP 探测出口 IP。

## Registrator 自身配置

Registrator 容器的环境变量：

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `REGISTRATOR_TOKEN` | Consul ACL token | _（无）_ |
| `CONSUL_ADDR` | Consul HTTP 地址 | `http://localhost:8500` |
| `RESYNC_INTERVAL` | 全量同步间隔（秒） | `30` |
| `DEFAULT_PORT` | 无法检测端口时的默认值 | `8080` |
| `ADVERTISE_ADDR` | 覆盖自动检测的宿主机 IP | 自动检测 |

## 架构

```
Docker daemon
    │
    ├─ /var/run/docker.sock ──► registrator 容器 (host 网络)
    │                              │
    │   ┌──────────────────────────┘
    │   │
    │   ├─ libcurl unix-socket → Docker API (/events 流式, /containers)
    │   └─ libcurl HTTP → Consul HTTP API (/v1/agent/service/register|deregister)
    │
    └─ 业务容器 (仅 CONSUL_LISTEN_ENABLE=true 的容器会被注册)
```

## 许可证

MIT
