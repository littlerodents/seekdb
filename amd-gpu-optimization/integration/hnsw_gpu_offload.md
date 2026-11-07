# seekdb HNSW 构建链路 GPU 卸载集成方案

## 热路径定位

seekdb 的 HNSW 索引构建（`CREATE INDEX ... WITH (TYPE=HNSW)`）在每个节点插入时执行：

1. 从入口点做贪心图下降（逐层选最近邻）；
2. 在选中层对邻居候选集做**批量 L2 距离评估**；
3. 对评估结果做 **top-M 选择**（M 为 HNSW 最大度），启发式连接。

其中步骤 2/3 是 O(N·M·D) 的距离计算 + 选择，占构建耗时的大头（实测见 bench/）。当向量基数大、维度高（D≥512）时，该原语可整体卸载到 AMD GPU。

## GPU 侧原语（本仓库 bench/）

- `l2_kernel`：单查询 × 全量基向量 L2（对应逐点插入时序）。
- `l2_batched`：全查询 × 全基向量单 launch 批量距离（对应 bulk 预计算/批量插入时序）。

## 桥接接口（seekdb 侧新增，C ABI，不侵入现有内核路径）

```cpp
// src/lib/obvector/ob_hnsw_gpu_accel.h
int  ob_hnsw_gpu_init(int64_t device_id, int dim);              // 常驻设备内存池 + 预热 kernel
int  ob_hnsw_gpu_load_base(const float *base, int64_t n);       // 基向量上载（构建期一次）
int  ob_hnsw_gpu_eval_candidates(const float *q, int64_t *cand,  // 单点插入：候选集距离评估
                                 int ncand, float *out);
int  ob_hnsw_gpu_bulk_knn(const float *q, int64_t nq, int k,    // 批量：全量距离 + 每行 top-k
                          int64_t *out_idx, float *out_val);
void ob_hnsw_gpu_fini();
```

## 接入策略（按收益排序）

1. **批量构建通道**（收益最大）：`ObHNSWIndexBuilder` 在 `bulk_load` 路径把待插向量按 4K 一批送
   `ob_hnsw_gpu_bulk_knn` 预取每批的初始近邻，CPU 只做图连接启发式。批内一次 launch，实测
   d=1024 时距离核 1.53×（内核化 tiling + GPU top-k 后预期 5-10×）。
2. **逐点插入加速**（受 launch 开销制约）：仅当候选集 ≥ 阈值（如 ≥4096）才走 GPU，否则维持 CPU
   路径；GPU 内存池常驻避免 hipMalloc 抖动。
3. **混合调度**：`ob_hnsw_gpu_init` 探测 gfx 架构（gfx90a/gfx94x/gfx1100+）与可用显存，
   失败静默回落 CPU——保证非 ROCm 环境零回归。

## 落地工程要点

- 设备内存：基向量池上限 min(显存 60%, N·D·4B)，超限分片多轮 batch；
- 时序：逐点模式 kernel 往返 ≈70µs/次，单点候选评估在 128 核 EPYC 上不划算（实测 0.74s vs
  0.24s/5000 点），因此逐点路径保留 CPU；批量路径才启用 GPU；
- 正确性：GPU 结果与 CPU 路径在构建期抽样比对（checksum 门禁，见 bench 校验和一致的设计）；
- 后续：l2_batched 增加 shared-memory tiling（理论带宽余量 ~10×）与 GPU 侧 per-row top-k
  （当前端到端 1.19×，距离核 1.53×，目标是把 top-k 也移出 CPU）。

## 部署证据

- Radeon Cloud 实例 `rgapi-one-click-29008-ea57ae9f`（seekdb-hipvs 模板），gfx1100 48GB。
- seekdb v1.4.0.0 部署 UUID（telemetry `content.id`）：`f131c844-3672-87d7-86f4-16ce5030aae4`
  （2026-11-06 同型实例 SQL 验证：`5.7.25-OceanBase seekdb-v1.4.0.0`，observer ready）。
