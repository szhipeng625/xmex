# 通用 HTTP 客户端设计

`http.get(url)` 能打**任意** URL，供接口测试用。

## 一、为什么要有它

`cluster.cpp` 里已有一份 HTTP 客户端（GET/POST/超时/Bearer/TLS），但它是
`static` 私有函数，绑死了 cluster 协议：要 `secret`、要先 `cluster.connect`
注册节点。测第三方接口时这条路走不通。

所以把它抽成独立模块。**不是重写** —— `tcp_connect` / `parse_url` /
请求构建 / 响应解析的逻辑照搬 `cluster.cpp`，那部分已在生产里跑过。

相对 cluster 版做了三处扩展：

| | cluster 版 | http 版 |
|---|---|---|
| chunked 传输编码 | 不支持 | **支持**（见 2.1） |
| 自定义请求头 | 硬编码 Content-Type 与 Bearer | 可传（见 2.2） |
| 目标 | 只能是已注册的 cluster 节点 | 任意 URL |

## 二、实现要点

### 2.1 chunked 解码（cluster 版缺的能力）

cluster 版靠 `Connection: close` + 读到 socket 关闭来判定响应结束。遇到
chunked 响应时，`"<hex>\r\n<data>\r\n0\r\n\r\n"` 这一整段协议文本会被当成
body 返回 —— 调用方一解析就炸，还以为是接口返回错了。

现在按 `Transfer-Encoding: chunked` 解析。实测（`http_test` 用例 2）：
未解码时 body 是 `5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n`，解码后是
`hello world`。

### 2.2 自定义请求头

```python
http.request("GET", url, "", 5000, "token", {"X-Request-Id": "req-001"})
http.request("GET", url, "", 5000, "token", [["Cookie", "a=1"], ["Cookie", "b=2"]])
```

dict 与 `[[k,v],...]` 两种形态都接受 —— 后者允许同名头出现多次
（多个 `Cookie`），dict 做不到。

同名头后者覆盖前者（常用于把 `Content-Type` 改成 `x-www-form-urlencoded`）。

### 2.3 超时：两处都要管

**连接阶段**用非阻塞 connect + `select`；**读响应阶段**用 `select` 等可读
+ 非阻塞 recv。

⚠️ **不能靠 `SO_RCVTIMEO`。** 实测 Windows 上它对 `recv()` **完全无效** ——
设成 800ms、对端 3 秒后才回，`recv` 照样阻塞满 3 秒（用独立最小 C 程序验证
过，不是本项目的问题）。POSIX 上它是生效的，所以跨平台代码不能依赖它。

超时后如果已经收到部分响应（比如服务端回了 500 又卡住），照样把状态行与
body 交给调用方 —— 那往往正是问题所在。

### 2.4 参数补齐：irgen 的架构限制

⚠️ 这条影响所有「可选参数」的内置函数，改动要小心。

`irgen` 按**调用点的实际实参个数**生成 IR（`genCall` 里 `argTypes` 逐个
push），而 `abi_.getOrDeclare` **明确拒绝"同符号两种签名"**
（宁可编译期炸，也不要链接通过、运行时段错误）。

于是 `http.request(m,u)` 与 `http.request(m,u,b,t,k,h)` 会生成两种函数原型，
后者直接 `report_fatal_error`。

解法分两步：

1. **C 侧用可变参数**（`...`）—— LLVM 层无法表达 C 变参，但至少让所有调用
   落在同一个 C 符号上
2. **irgen 侧按固定元数补齐** —— `builtinOptionalArgs(rtFn)` 查表拿到该函数
   的元数，缺失的尾参数补 `None`。C 侧 `va_arg` 取到 0 后按默认值处理

新加带可选参数的内置函数时，要同时改 `builtinOptionalArgs` 与 C 侧签名。

## 三、用法

```python
r = http.get(url)                      # {"ok":true,"status":200,...}
r = http.get(url, 3000)                # 3 秒超时
r = http.post(url, body, 3000)
r = http.head(url, 2000)               # 探活:有状态码与头,无 body
r = http.request("DELETE", url, "", 3000)
r = http.request("GET", url, "", 5000, "token", {"X-Api-Key": "k"})
```

返回 JSON 字符串：

```json
{"ok":true,"status":200,"elapsed_ms":12,"content_length":18,
 "body":"...","headers":"HTTP/1.1 200 OK\r\n...","error":""}
```

`headers` 是原始文本（含状态行）—— 调试时要看 `Date`、`X-Request-Id`
这类非标准头，解析成结构化反而丢信息。

### 与故障注入配合

这才是本项目做 HTTP 的目的：

```python
kv.container_add("api", "nginx:alpine", "", "0.5", "128m")
kv.reconcile_start(1000)

print(http.get("http://127.0.0.1:8080/ping"))   # 200
kv.container_pause("api")
print(http.get("http://127.0.0.1:8080/ping"))   # 超时(服务冻结了)
kv.container_unpause("api")
kv.container_crash("api")
sleep_ms(3000)
print(http.get("http://127.0.0.1:8080/ping"))   # 调谐重启后恢复 200
```

## 四、刻意不做

| 不做 | 原因 |
|---|---|
| 连接池 | 测试脚本是一次性的，池化省不下什么 |
| 异步 | 接口测试是脚本驱动的，同步更直观 |
| **自动重试** | 会把刚注入的故障冲掉 —— 测的正是"重试前"的行为 |
| **重定向跟随** | 会把"接口返回了 302"这个事实藏起来 |
| multipart/form-data | 少有人用；要的话自己拼 body 并设 Content-Type |
| Cookie jar | 测试不该有隐式状态 |

原则：**每一步都要看得见**。所有"聪明"的行为都会让失败难以归因。

## 五、测试

`tests/http_test.cpp`（26 项断言）**在测试进程里起一个最小 HTTP 服务端**
（裸 socket 几十行），然后真打它 —— 真 TCP、真 HTTP 报文、真 chunked 解码，
不是 mock。

覆盖：GET/POST/chunked 解码/错误状态码（404/500，且 500 的 body 照样取得到）/
连接失败/超时/URL 解析/JSON 转义。

`examples/http_demo.pys` + `examples/http_demo_main.cpp` 是端到端用例：
C++ 宿主 + 编译后的 `.o`，验证 `PyLite 源码 → 编译 → 链接 → 真实 HTTP` 整条链路。

### 测试抓到的三个 bug

**1. 端口没传给 `getaddrinfo`（跨平台 bug，最严重）。**

```cpp
getaddrinfo(host.c_str(), nullptr, &hints, &res);   // ✗ 端口丢失
```

返回的 `sockaddr_in.sin_port` 是 0，于是 `connect` 连的是 `host:0`。Linux 上
是 `ECONNREFUSED`，Windows 上是 `WSAEADDRNOTAVAIL(10049)`。

这个 bug 把我引向了完全错误的方向 —— 我一度以为是「Windows 上非阻塞
connect 到 loopback 的平台差异」，为此专门写了最小 C 程序去验证非阻塞行为，
甚至改了 loopback 的连接策略。**症状（都是"连不上"）掩盖了真实原因。**
教训：跨平台错误码差异很容易成为烟雾弹，遇到"只在某平台失败"要先确认
错误码本身指向什么，别急着归因到平台特性。

**2. `SO_RCVTIMEO` 在 Windows 上对 `recv()` 无效。** 见 2.3，改用 `select`。

**3. `SO_RCVTIMEO` 之外还漏了 Winsock 初始化。** `cluster.cpp` 靠调用方做
`WSAStartup`（它在 Linux-only 段，那边不需要），但 `http.cpp` 是独立模块不能
指望外部 —— 漏掉的话 `socket()` 直接返回 `INVALID_SOCKET`，所有请求都
`connection failed`。现在 `tcp_connect` 里有 `ensure_socket_init()`。