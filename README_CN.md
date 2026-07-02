# Consul Registrator

[English](README.md) | 中文

轻量级 Docker 服务注册器，使用 C 语言编写。监听 Docker 容器启停事件，自动向 Consul Agent 注册/注销服务。

基于 Alpine Linux + libcurl + cJSON 构建，单二进制文件，极小资源占用。

> 本仓库还附带一个可选的 **OpenResty 网关**（`openresty/`），消费注册器发布的
> 服务：一个由 Consul KV 驱动的动态路由器，支持子域名与路径两种路由形态。
> 参见 [网关（OpenResty）](#网关openresty)。

## 特性

- **Opt-in 模式** — 仅设置了 `CONSUL_LISTEN_ENABLE=true` 的容器才会被注册
- **双通道配置** — 优先读取容器环境变量，其次读取 labels
- **自动默认端口选择** — `CONSUL_SERVICE_PORT` > 最小 HostPort 映射容器端口 > 最小 Docker 已知端口 > 最小 EXPOSE 端口
- **TCP 健康检查** — 自动生成 Consul TCP 检查，目标为 `容器IP:容器端口`
- **智能地址解析** — 有端口映射的容器使用宿主机 IP，bridge/自定义网络容器使用容器 IP
- **POD_IP 模式** — 强制使用容器 IP + 内部端口注册（支持全局或按端口配置）
- **按端口配置服务** — 显式声明的 `CONSUL_SERVICE_<port>_*` 端口始终会与默认端口合并注册
- **变更日志** — 全量同步时仅在状态变化时输出日志，减少噪音
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
      - ADVERTISE_ADDR=${ADVERTISE_ADDR:-}
      - LOG_LEVEL=${LOG_LEVEL:-info}
      - HOSTNAME_OVERRIDE=${HOSTNAME_OVERRIDE:-}
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
| `CONSUL_SERVICE_PORT` | 覆盖自动检测的端口（内部/容器端口） | 自动检测 |
| `CONSUL_SERVICE_CHECK_INTERVAL` | TCP 健康检查间隔 | `10s` |
| `CONSUL_SERVICE_POD_IP` | 设为 `false` 则对端口映射容器使用宿主机 IP + 宿主机端口 | `true` |
| `CONSUL_SERVICE_<port>_NAME` | 声明并命名一个额外的容器端口服务 | 同上 |
| `CONSUL_SERVICE_<port>_TAGS` | 为已声明的容器端口设置标签 | 同上 |
| `CONSUL_SERVICE_<port>_POD_IP` | 为已声明的容器端口覆盖 POD_IP 设置 | 继承 `CONSUL_SERVICE_POD_IP` |

### 端口选择

最终注册的服务端口由两部分组成：

1. 显式 `CONSUL_SERVICE_<port>_*` labels / 环境变量。这些配置会直接声明对应容器端口，并始终合并进最终服务集合。
2. 一个默认端口，按以下优先级选择：
   1. `CONSUL_SERVICE_PORT` 环境变量 / label
   2. 最小 Docker 端口映射（`HostPort`）对应的容器端口
   3. 最小 `NetworkSettings.Ports` key（捕获 compose `expose:` 等 Docker 已知端口）
   4. 最小 Dockerfile `EXPOSE` 端口

如果显式声明的端口本身存在 HostPort 映射，那么在 `CONSUL_SERVICE_<port>_POD_IP=false` 时，仍会优先使用宿主机映射信息。

没有声明端口且也无法检测出默认端口的容器将被**跳过**（不注册）。对于没有显式端口声明的 docker-compose 服务，可在 compose 文件中添加 `expose:`、在 Dockerfile 中添加 `EXPOSE`，或直接使用 per-port labels（如 `CONSUL_SERVICE_8080_NAME=myapp`）声明端口。

### 健康检查

系统为每个注册的服务自动创建 TCP 健康检查：
- 优先目标为 `容器IP:容器端口`（首选，因为 Consul agent 可通过 Docker bridge 网络访问容器）
- 否则回退到 `服务地址:服务端口`
- 默认间隔：`10s`（可通过 `CONSUL_SERVICE_CHECK_INTERVAL` 配置）

### 地址解析

注册到 Consul 的服务地址自动确定：

| 场景 | 地址 | 端口 |
|------|------|------|
| `network_mode: host` 的容器 | 宿主机 IP | 容器端口 |
| 非 host 网络容器（默认，`POD_IP=true`） | 容器 IP | 容器端口 |
| 非 host + `CONSUL_SERVICE_POD_IP=false` + 有端口映射 | 宿主机 IP | 宿主机端口 |
| 非 host + `CONSUL_SERVICE_POD_IP=false` + 仅 EXPOSE | 容器 IP | 容器端口 |

宿主机 IP 检测优先级：`ADVERTISE_ADDR` 环境变量 > `getaddrinfo(hostname)` > UDP 探测出口 IP > hostname 字符串回退。

## Registrator 自身配置

Registrator 容器的环境变量：

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `REGISTRATOR_TOKEN` | Consul ACL token | _（无）_ |
| `CONSUL_ADDR` | Consul HTTP 地址 | `http://localhost:8500` |
| `DOCKER_SOCK` | Docker Engine socket 路径 | `/var/run/docker.sock` |
| `RESYNC_INTERVAL` | 全量同步间隔（秒） | `30` |
| `ADVERTISE_ADDR` | 覆盖自动检测的宿主机 IP | 自动检测 |
| `HOSTNAME_OVERRIDE` | 覆盖系统主机名（用于 service ID） | 系统主机名 |
| `LOG_LEVEL` | 设为 `debug` 输出详细日志 | `info` |

## 架构

### 线程模型

注册器运行三个互不阻塞的线程：

```
┌─────────────────────────────────────────────────────────────────────┐
│                     registrator 容器 (host 网络)                     │
│                                                                     │
│  ┌──────────────┐    ┌──────────────┐    ┌───────────────────────┐  │
│  │ 主线程        │    │ 事件处理线程  │    │ 全量同步线程           │  │
│  │              │    │              │    │                       │  │
│  │ curl SSE     │    │              │    │ sleep(N)              │  │
│  │ /events ─────┼──► │ 出队 ──────► │    │   │                   │  │
│  │ (长连接)      │ 队列│ 注册 / 注销  │    │   ▼                   │  │
│  │              │    │              │    │ sync_all()            │  │
│  └──────┬───────┘    └──────┬───────┘    │  ├─ register_container│  │
│         │                   │            │  └─ cleanup_orphans   │  │
│         │                   │            └───────────┬───────────┘  │
│         │ unix sock         │ HTTP                   │ HTTP         │
└─────────┼───────────────────┼───────────────────────┼──────────────┘
          │                   │                       │
          ▼                   ▼                       ▼
   Docker daemon         Consul agent           Consul agent
   /events 事件流        /v1/agent/service/*     /v1/agent/service/*
```

- **主线程** — 维持到 Docker `/events` 的 SSE 长连接。curl 回调中解析 JSON 行，推入事件队列。此线程不做任何 Consul I/O。
- **事件处理线程** — 等待事件队列（`pthread_cond_wait`）。`start` → `register_container()`；`stop`/`die` → `deregister_container()`。
- **全量同步线程** — 每隔 `RESYNC_INTERVAL` 秒执行 `sync_all()`：重新注册所有 enabled 容器，清理容器已不存在的孤立服务。

### 启动流程

```
main()
 ├─ 加载环境变量配置 (CONSUL_ADDR, REGISTRATOR_TOKEN, ...)
 ├─ resolve_host_ip() 解析宿主机 IP
 │    ├─ 1. ADVERTISE_ADDR 环境变量
 │    ├─ 2. getaddrinfo(hostname)，跳过 127.x.x.x
 │    ├─ 3. UDP connect trick（通过内核路由获取出口 IP）
 │    └─ 4. 回退到 hostname 字符串
 ├─ preflight_check() 预检
 │    ├─ Docker API: GET /info
 │    └─ Consul API: GET /v1/status/leader
 ├─ sync_all()  ← 首次全量同步
 ├─ 启动 resync_thread
 ├─ 启动 event_processor_thread
 └─ watch_events()  ← 主线程阻塞
```

### 注册流程

对每个设置了 `CONSUL_LISTEN_ENABLE=true` 的容器：

```
register_container(container_id)
 │
 ├─ Docker API: GET /containers/{id}/json
 │
 ├─ 门控: CONSUL_LISTEN_ENABLE != "true" → 跳过 (return 0)
 │
 ├─ collect_declared_ports() + detect_default_port()
 │    ├─ 显式 CONSUL_SERVICE_<port>_* labels / 环境变量 → 始终合并
 │    ├─ 默认端口: CONSUL_SERVICE_PORT → 反查 HostPort 映射
 │    ├─ 默认端口: 最小 NetworkSettings.Ports HostPort 绑定
 │    ├─ 默认端口: 最小 NetworkSettings.Ports key
 │    ├─ 默认端口: 最小 Config.ExposedPorts (EXPOSE)
 │    └─ 没有声明端口且没有默认端口 → port_count=0，跳过注册
 │
 ├─ 门控: port_count == 0 → 跳过 (return 1，仍计入 enabled)
 │
 ├─ 检测网络模式
 │    └─ container_ip 为空 → is_host_network = true
 │
 └─ 逐端口注册:
      │
      ├─ 读取按端口配置 (CONSUL_SERVICE_{port}_NAME / _TAGS / _POD_IP)
      │
      ├─ 地址解析（四级优先级）:
      │    ├─ host 网络          → host_ip + 容器端口      (POD_IP 无效)
      │    ├─ POD_IP=true (默认) → container_ip + 容器端口
      │    ├─ POD_IP=false + 有映射 → host_ip + 宿主机端口
      │    └─ POD_IP=false + 仅 EXPOSE → container_ip + 容器端口
      │
      ├─ TCP 健康检查目标:
      │    ├─ 有 container_ip → container_ip:容器端口
      │    └─ 否则           → 服务地址:服务端口
      │
      └─ PUT /v1/agent/service/register
           { ID, Name, Address, Port, Tags, Meta, Check }
```

### 注销与孤儿清理

```
deregister_container(container_id)         cleanup_orphans(valid_ids)
 │                                          │
 ├─ GET /v1/agent/services                  ├─ GET /v1/agent/services
 │                                          │
 ├─ 匹配策略:                                ├─ 过滤: Meta.registrator == "self-hosted"
 │   1. Meta.container_id                   │
 │   2. service_id 前缀（回退）               ├─ Meta.container_id 不在 valid_ids 中？
 │                                          │   → 容器已不存在，注销服务
 └─ PUT /v1/agent/service/deregister/{id}   │
                                            └─ PUT /v1/agent/service/deregister/{id}
```

## 网关（OpenResty）

`openresty/` 目录附带一个可选的 OpenResty 网关，把注册器发布的服务变成一个由
Consul 驱动的动态路由器。它轮询**本地** Consul agent，将路由表保存在共享内存中，
并按主机名或路径把每个请求反向代理到健康的后端。路由配置存放在 Consul KV 中，
因此可以在运行时重新分组域名，无需重启网关。

### 域名分组（Consul KV）

配置是**按节点**的：网关先解析自己的 Consul 节点名（来自本地 agent 的
`GET /v1/agent/self`，或用 `NODE_NAME` 环境变量覆盖），再读取
`gateway/<node>/configuration`——一个 JSON 数组。每个分组把一组域名后缀绑定到
一组标签：

```bash
# <node> 是本网关的 Consul 节点名（例如通过 `consul members` 查看）
consul kv put gateway/<node>/configuration '[
  {"name":"web",  "suffixes":["svc.local"],                  "tags":["web","prometheus"]},
  {"name":"data", "suffixes":["data.local","data.internal"], "tags":["data"]}
]'
```

- 服务只有携带某个分组的标签之一，才能通过该分组路由。命中多个分组的服务会在
  **每个**分组里都注册（多归属），并可通过每个命中分组的后缀访问。
- 配置每 `POLL_INTERVAL` 秒拉取一次。KV 读取失败时保留上一次成功的配置；若从未
  成功加载过，则所有请求返回 `404`。没有全局默认、也没有环境变量兜底——每个节点
  必须有自己的 `gateway/<node>/configuration` 键（纯 per-node KV）。

### 访问形态

每个后缀都接受两种等价的请求形态：

| 形态 | 示例 | 后端收到 |
|------|------|----------|
| 子域名 | `orders.svc.local/api` | `/api` |
| 路径 | `svc.local/orders/api` | `/api`（`/orders` 前缀被剥掉） |

两者都会到达服务 `orders`（前提是 `orders` 携带了拥有 `svc.local` 的分组的标签）。
路径形态正是让一个“主域名”承载多个服务的方式，例如 `node.cen/<service>/...`。

### 网关配置

网关容器的环境变量（`openresty/docker-compose.yaml`）：

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `CONSUL_ADDR` | 本地 Consul agent HTTP 地址 | `http://127.0.0.1:8500` |
| `CONSUL_TOKEN` | Consul ACL token（需 agent + service + KV 读权限） | _（空）_ |
| `POLL_INTERVAL` | KV 与服务的轮询间隔（秒） | `5` |
| `NODE_NAME` | 覆盖 Consul 节点名（否则从 agent/self 读取） | _（自动）_ |

token 需要对 agent（读节点名）、服务目录、以及 `gateway/` KV 前缀的读权限——可直接
应用的 policy 见 [`consul/gateway-policy.hcl`](consul/gateway-policy.hcl)。

### 节点范围（重要）

**网关是节点本地的。** 它通过本地 agent 的 `GET /v1/agent/services` 发现服务，
而该接口只返回**网关所在的同一个 Consul 节点上注册的服务**——也就是本节点的
注册器所注册的那些容器。它看不到其他节点上的服务，也不做按节点锁定。

- 要路由某个节点的服务，就在**那个节点上**运行一个网关（与它的注册器、agent 同机）。
  集群场景下，每个节点各部署一个网关。
- `node.cen` 是你配置的**字面**后缀，**不是** Consul 节点名。按节点寻址
  （`<service>.<node>.cen`）**不支持**；那需要改用 catalog/health 视图，而非
  当前的 agent-local 视图。
- Prometheus `consul_sd` 是**集群级**发现（catalog），而网关是节点本地的。若某个
  目标 `<service>.cen` 解析到的网关并不在该服务所在的节点上，抓取就会 `404`。
  请让 `*.cen` DNS 指向与目标服务同机的网关（单节点，或每节点一个网关 + 按节点
  解析的 DNS）。

## 许可证

MIT
