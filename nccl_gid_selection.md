# NCCL GID 选择算法（IB/RoCE）

记录 NCCL 在 IB/RoCE handshake 阶段挑选**本地 GID idx** 的逻辑。Mooncake 的 `findBestGidIndex` 改进版以这套算法为对齐目标。

## 源码位置

NVIDIA/nccl 主仓 `master` 分支：

- 入口函数：`ncclIbGetGidIndex`
  https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib/connect.cc#L238-L266
- 比较器：`ncclUpdateGidIndex`
  https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib/connect.cc#L199-L237
- 三个 helper：`configuredGid` / `linkLocalGid` / `validGid`
  https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib/connect.cc#L156-L174
- 地址族判断：`getGidAddrFamily`
  https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib/connect.cc#L105-L109
- 子网匹配：`matchGidAddrPrefix`
  https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib/connect.cc#L111-L138
- IB FLID 提取：`ncclIbExtractFlid`
  https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib/connect.cc#L100-L102
- RoCE 版本读 sysfs：`ncclIbRoceGetVersionNum`
  https://github.com/NVIDIA/nccl/blob/master/src/transport/net_ib/connect.cc#L176-L207

注意：旧版（NCCL ≤ 2.18）在 `src/transport/net_ib.cc` 单文件里，新版拆到 `src/transport/net_ib/` 子目录。本文档基于当前 master。

## 涉及的环境变量

| Env 变量 | 默认值 | 含义 |
|---|---|---|
| `NCCL_IB_GID_INDEX` | -1 | 直接指定 idx；≥ 0 时绕过自动选择 |
| `NCCL_IB_ROUTABLE_FLID_GID_INDEX` | 1 | IB 链路上 FLID-routable GID 的 idx；找不到回退 idx=0 |
| `NCCL_IB_ROCE_VERSION_NUM` | 2 | 偏好的 RoCE 版本（1 或 2）|
| `NCCL_IB_ADDR_FAMILY` | `AF_INET` | 偏好 IPv4 还是 IPv6 |
| `NCCL_IB_ADDR_RANGE` | (未设)| 单个 CIDR 子网过滤，例如 `192.168.1.0/24` |

注意 `NCCL_IB_ADDR_RANGE` **只支持单个 CIDR**，不支持逗号分隔的列表（Mooncake 的 `MC_GID_PREFER_SUBNETS` 才是列表）。

## 算法分两条路

### 路径 A：IB 链路（不是 RoCE）

`port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND` 时：

1. 读 `NCCL_IB_ROUTABLE_FLID_GID_INDEX`（默认 1）。
2. 查这个 idx 对应的 GID。
3. 如果它的 FLID（GID raw 字节 4-5 解析为 16-bit 整数）非零 → 选它。
4. 否则 → 选 idx=0。

**只看一个 idx，不扫表，不调用 `ncclUpdateGidIndex`。** 因为 IB fabric 的 SM/SA 已经替你做了路由决定，本机能选的余地很小。

### 路径 B：RoCE 链路（典型场景）

1. 如果 `NCCL_IB_GID_INDEX` ≥ 0 → 直接用这个值，**不扫表**。
2. 读用户偏好：`userAddrFamily`、`userRoceVersion`、`prefix/prefixlen`（CIDR）。
3. 初始化 `*gidIndex = 0`。
4. 从 `gidIndexNext = 1` 扫到 `gidTblLen - 1`，每次调一次 `ncclUpdateGidIndex(currentBest, candidate)`，让比较器决定要不要换 best。
5. 扫完返回 `*gidIndex`。

**特点**：扫完整张表才决定（best-fit），中间某个 idx 查询失败也只会 `continue`（不 break）。

## `ncclUpdateGidIndex`：比较器优先级

这是 NCCL 选 GID 的真正"评判规则"。展开比较逻辑：

```
查 current = gid[*gidIndex]
查 candidate = gid[gidIndexCandidate]
计算 currentFam   = getGidAddrFamily(current)
     candidateFam = getGidAddrFamily(candidate)
     candidateMatchSubnet = matchGidAddrPrefix(prefix, candidate)
```

### 第 1 档：candidate 的地址族更"对"

```
if (candidateFam != currentFam     ← 两者地址族不同
 && candidateFam == userFam        ← candidate 是用户想要的家族
 && candidateMatchSubnet)          ← 且在用户配的子网里
{
    *gidIndex = gidIndexCandidate;
}
```

意思：当前 best 的家族跟用户偏好不一致、candidate 的一致**且**子网命中 → 换 candidate。这是"地址家族升档"。

### 第 2 档：地址族一致时，看 RoCE 版本是否更对

```
else if (candidateFam == userFam && validGid(candidate) && candidateMatchSubnet) {
    读 currentRoceVer  = sysfs gid_attrs/types/<current>
    读 candidateRoceVer = sysfs gid_attrs/types/<candidate>
    if ((currentRoceVer != candidateRoceVer || !validGid(current))
        && candidateRoceVer == userRoceVer)
    {
        *gidIndex = gidIndexCandidate;
    }
}
```

意思：candidate 也是用户偏好的家族、合法、子网也命中。再细分：
- 当前 best 不合法（NULL GID 或 link-local）且 candidate 是用户想要的 RoCE 版本 → 换 candidate
- 或当前 best 跟 candidate RoCE 版本不同、且 candidate 是用户想要的版本 → 换 candidate

否则保持当前 best 不变。

### 不进入两档的情况

candidate **不**在用户子网里、或地址族不对、或不 valid → 直接 return（不更新 best）。

## 优先级总结（从高到低）

按 `ncclUpdateGidIndex` 比较的隐含次序，等价于这个优先级表：

| 优先级 | 条件 | 是否决定性 |
|---|---|---|
| 1 | `validGid` —— 不是 NULL GID、不是 fe80:: link-local | **必要条件**，不满足直接淘汰 |
| 2 | 地址族 == `NCCL_IB_ADDR_FAMILY`（默认 AF_INET）| 强偏好 |
| 3 | 在 `NCCL_IB_ADDR_RANGE` CIDR 内（如有设置）| 强偏好（不在子网内的 candidate 直接 return）|
| 4 | RoCE 版本 == `NCCL_IB_ROCE_VERSION_NUM`（默认 2）| 在前 3 项打平时的 tie-breaker |
| 5 | idx 数值小者优先（隐式）| 初始 best=0，扫描方向 1→N，没换就保留小的 |

## `validGid` 把哪些 GID 直接淘汰

两层：

### `configuredGid` 拒绝两种"明显空"的 GID

```c
trailer = a[1] | a[2] | a[3]
if ((a[0] | trailer) == 0)             // 全 0 GID（驱动分配但未填）
    || ((a[0] == 0xfe800000)            // fe80::/96 但 trailer 全 0
        && trailer == 0)
{
    return false;
}
```

也就是淘汰：
- 完全 0 的 GID（GID 表里的"空槽"，已分配但驱动还没填进 IP）
- `fe80::` 的 96-bit 前缀 + 后 32 位全 0（这是驱动占位符，不是真 link-local）

### `linkLocalGid` 拒绝真 link-local

```c
if (a[0] == 0xfe800000 && a[1] == 0)
    return true;   // 是 link-local
```

`fe80::xxxx` 这种**正经的 IPv6 link-local 地址**——只在 L2 段内可路由，跨机不行。NCCL 一律拒。

`validGid = configuredGid && !linkLocalGid` —— 两层都过才算合法。

## 对齐到 Mooncake 的映射

| NCCL | Mooncake（new/ 改进版） |
|---|---|
| `NCCL_IB_GID_INDEX` | `MC_GID_INDEX`（现有）|
| `NCCL_IB_ADDR_FAMILY` | `MC_GID_ADDR_FAMILY`（new/ 加的，已加进 config.h）|
| `NCCL_IB_ADDR_RANGE`（单 CIDR）| `MC_GID_PREFER_SUBNETS`（多 CIDR 列表，扩展）|
| —（NCCL 没有按 NIC 分 CIDR）| `MC_GID_PREFER_SUBNETS_PER_DEV`（Mooncake 加的）|
| `NCCL_IB_ROCE_VERSION_NUM` | **不暴露**（Mooncake hardcode v2）|
| `NCCL_IB_ROUTABLE_FLID_GID_INDEX` | **不暴露**（Mooncake 主流是 RoCE）|
| `validGid` (checked configuredGid + !linkLocalGid) | 应在 Mooncake `findBestGidIndex` 里加同等过滤 |

Mooncake 当前的 `findBestGidIndex` 比 NCCL 这套缺了：

1. **best-fit 而不是 first-fit** —— Mooncake 扫到第一个 v4+v2+ndev 立刻 return，不看后面的更优
2. **link-local 拒绝** —— Mooncake fallback 路径会选到 fe80:: GID，NCCL 直接淘汰
3. **NULL GID 拒绝** —— Mooncake 的 break 把空洞当尾，跳过后面所有 idx；NCCL 用 continue + validGid 过滤
4. **子网偏好** —— Mooncake 没有，跨子网必须靠 `MC_GID_INDEX` 钉死

## 一些非显然的细节

### NCCL 的 RoCE 版本检查比 Mooncake 想得复杂

`ncclIbRoceGetVersionNum` 读 `/sys/class/infiniband/<dev>/ports/<port>/gid_attrs/types/<idx>` 这个文本文件（"IB/RoCE v1" / "RoCE v1" / "RoCE v2" 三种字符串），**只在比较器内部用**，不是预过滤。

容器内这个 sysfs 文件**可能读不到**（read 返回 -1 / EINVAL）—— NCCL 的实现是返回 ncclSuccess 但不更新 `*version`，**当作 "version 未知"**。

### `ncclIbExtractFlid` 是从 GID 头部抽 16 bit

```c
return ntohs(*((uint16_t*)(gid->raw + 4)));
```

GID 是 128-bit。字节 0-3 是 subnet prefix；**字节 4-5 是 FLID**；后面是 GUID。FLID 由 fabric admin 配，零表示"该 fabric 不用 FLID 路由"。

### `ncclUpdateGidIndex` 第 1 档存在的原因

如果用户偏好 IPv6（`NCCL_IB_ADDR_FAMILY=AF_INET6`）但表里 idx=0 是个 IPv4 GID，初始 best=0 是错家族。第 1 档专门处理这种场景：扫到第一个 IPv6 GID 就直接换掉 IPv4 的 best。

### 为什么用 idx=0 当初始 best 不会"卡住"

如果 idx=0 是 NULL GID 或 link-local，`validGid(current)` 会返回 false，第 2 档的内层 if 会无条件换掉 best。所以 idx=0 即便很差也不会困住选择过程。

## 参考

- 当前文档基于 NVIDIA/nccl 的 `master` 分支（约 NCCL 2.21+）
- 对应 commit 可在 https://github.com/NVIDIA/nccl/commits/master 查
- 该算法自 NCCL 2.12（约 2022 年）引入 ECE 协商时开始稳定，之后变化不大

---

## 三方对比：NCCL vs Mooncake 新版 vs Mooncake 旧版

| 维度 | NCCL | 新 v2（best-fit） | 旧版（first-fit） |
|---|---|---|---|
| 选择策略 | best-fit，扫完全表挑评分最高 | best-fit，扫完全表挑评分最高 | first-fit，命中第一个 v4+ndev 立刻 return |
| 比较方式 | 嵌套 if-else（`ncclUpdateGidIndex`） | `Score` 元组字典序比较 | 4 档 if-else fallback |
| `ibv_query_gid_ex` 失败 | continue | continue | **break**（错过后面 idx） |
| NULL GID 拒绝 | ✅（`configuredGid`） | ✅（`isNullGid`） | ❌ 不拒，可能被 fallback 选中 |
| Link-local GID 拒绝 | ✅（`linkLocalGid`） | ✅（`isLinkLocalGid`） | ❌ 不拒，可能被 fallback 选中 |
| 地址族偏好 | `NCCL_IB_ADDR_FAMILY` | `MC_GID_ADDR_FAMILY` | hardcode 优先 v4 |
| 子网 CIDR 过滤 | 单 CIDR `NCCL_IB_ADDR_RANGE` | 单 CIDR `MC_GID_PREFER_SUBNET` | 无 |
| RoCE v1/v2 区分 | ✅ 读 sysfs 选用户偏好版本 | ❌ hardcode 跳 v1 | ❌ hardcode 跳 v1 |
| Netdev 检查 | ❌ 不查 | ✅ 查名字非空（弱） | ✅ 查名字非空（弱） |
| IB 链路特殊处理 | ✅ 入口分支 + FLID 优化 | ❌ | ❌ |
| 选错时诊断日志 | 无 | 每次 LOG `[GID-select] picked idx=N score=(...)` | 仅 device 级"GID is NULL"等 |

## 为什么新 v2 跟 NCCL 有差异

### 1. RoCE v1/v2 区分 —— 砍掉

NCCL 读 sysfs 让用户选 RoCE v1 或 v2，新版 hardcode 只接受 v2。

**原因**：Mooncake 下游 QP 建连只对 v2 测过；保留 v1 偏好等于给用户一个不工作的旋钮。RoCE v1 自 2014 IBTA deprecated，没保留必要。

### 2. IB 链路 + FLID 优化 —— 砍掉

NCCL 入口先看 `port_attr.link_layer`，IB 走独立路径选 FLID-routable GID。新版完全没这分支。

**原因**：Mooncake 主战场是 RoCE 数据中心，IB SuperPOD 用户不用 Mooncake。代码层面"承认"IB GID 但没系统测试过；专门加 FLID 字段会误导用户以为 IB 能跑。等真有 IB 用户再补也来得及。

### 3. Netdev 检查 —— 多一层

NCCL 不查 GID 关联的 netdev 是否存在；新版查 sysfs `gid_attrs/ndevs/<idx>` 字段非空。

**原因**：NCCL 有 multi-rail 兜底（一个 NIC 不行换一个），Mooncake 是单设备 transport，选错就废。GID 表里"分配但没绑 netdev"的占位项拿去做 RoCE 跨机寻址必死，Mooncake 加这层保险拦在前面。

### 4. Netdev operstate=up 检查 —— 不做

NCCL 不查；旧 PoC 加过；最终新版砍了。

**原因**：bond slave、IB unknown state 这种边角场景下 operstate 不是 up 但 RDMA 仍可用，强查会误伤；下游 NIC 工作状态会自然反应（`ibv_post_send` 立即报错），不是 silent 失败。NCCL 的取舍是对的。

### 5. 比较方式 —— `Score` struct 而不是 NCCL 的嵌套 if-else

NCCL 用 `ncclUpdateGidIndex` 函数嵌套两层 if 实现 ranking；新版用 `Score` struct + `operator<` 字典序比较。

**原因**：表达力等价但可读性更好。加新维度只要往 `Score` 加一个字段，不用改循环结构。NCCL 那套是历史代码，新写没必要复刻它的层次。

### 6. 子网 CIDR 偏好 —— 行为对齐 NCCL，env 名保留 Mooncake 风格

NCCL 用 `NCCL_IB_ADDR_RANGE`，新版用 `MC_GID_PREFER_SUBNET`，都只支持单 CIDR。语义完全一致。

**原因**：Mooncake 自己的 env 命名空间是 `MC_*`，跟现有的 `MC_GID_INDEX`、`MC_PKEY_INDEX` 等保持一致。改成 NCCL 命名会让 Mooncake 自己的用户混淆。

### 7. 诊断日志 —— 加了 `[GID-select]` 前缀

NCCL 几乎不打 GID 选择相关日志；新版每次都打 `picked idx=N score=(...)`。

**原因**：Mooncake 出过"选错 idx 导致整个 transport 废"的事故（你今天遇到的 idx=1 link-local 事故），加诊断日志是 Mooncake 自己运维需要，不是要复刻 NCCL。

## 核心要点

> NCCL 是 GPU 集合通信库，假设运维干净 + 有 multi-rail 兜底，所以选择策略**激进而宽松**。
>
> Mooncake 是 KV cache transfer 库，单设备 transport + 不假设运维干净，所以选择策略**保守而严格**：宁可拒一些 NCCL 会接受的边角 GID（如 ndev 字段空的占位项），也别让用户在生产里选出来个跨不了机的 idx 后链路报错。
>
> 同时 Mooncake 的子集（不支持 v1、不支持 IB）让算法可以比 NCCL 砍掉两个维度，代码更简单。
