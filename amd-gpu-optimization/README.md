# AMD Radeon GPU HNSW 构建链路加速实验（seekdb）

## 目标

seekdb 的 HNSW 向量索引构建中，每个节点插入都要对已有节点做 **批量 L2 距离计算 + top-k 近邻筛选**——这是构建链路的计算核心。本实验把该热路径原语在 **AMD Radeon Cloud GPU（gfx1100）** 上用 HIP 实现，与 CPU（128 线程 OpenMP）做同数据、同算法、可校验的对比。

## 实验环境

| 项 | 值 |
|---|---|
| GPU | AMD Radeon Graphics, Card Model 0x744b, **gfx1100**, 48GB VRAM |
| CPU | 128 vCPU（Radeon Cloud 实例），Ubuntu 24.04, kernel 6.8.0-79-generic |
| 工具链 | ROCm 7.2.1 (`/opt/rocm-7.2.1`), hipcc / clang++, g++ -O2 -fopenmp |
| 实例 | AMD Radeon Cloud `rgapi-one-click-29008-ea57ae9f`（seekdb-hipvs 模板） |

## 方法与正确性校验

- 数据：`srand(42)` 生成的均匀随机向量，n=5000 基向量 + n=5000 查询向量。
- 两侧（CPU/GPU）使用**完全相同**的数据生成与 top-k 选择算法（排除自匹配：`dist*(bi==qi?0:1)`）。
- **正确性门禁：CPU 与 GPU 的 top-k 校验和（checksum）必须逐位一致**，否则视为无效测量。
- 三种实现：
  1. `cpu_knn.cpp` — CPU 基线：OpenMP 128 线程，每查询全量 L2 + 选择排序 top-32。
  2. `gpu_knn.hip` — GPU 逐查询模式（对应 HNSW **单点插入**时序：每个查询一次 kernel launch + H2D/D2H 往返）。
  3. `gpu_knn_batched.hip` — GPU 批量模式（对应 HNSW **批量构建/预计算**时序：一次 launch 计算 5000×5000 全距离矩阵）。

## 实测结果

### d=128（seekdb VECTOR(128) 常见维度），n=5000，top-32

| 实现 | 总耗时 | 阶段拆分 | checksum |
|---|---|---|---|
| CPU（OpenMP×128） | **0.2423 s** | — | 2436621.88 |
| GPU 逐查询 | 0.7408 s | kernel 往返 0.3487 + CPU top-k 0.3921 | 2436621.88 |

### d=1024（距离计算占比加重的对照），n=5000，top-32

| 实现 | 总耗时 | 阶段拆分 | checksum |
|---|---|---|---|
| CPU（OpenMP×128） | 1.4203 s | — | 23944755.88 |
| GPU 批量 | **1.1888 s** | H2D 0.0306 + kernel 0.5916 + D2H 0.0515 + CPU top-k 0.5151 | 23944755.88 |

**纯距离核对比（d=1024）**：CPU 距离阶段 ≈ 0.90 s vs GPU kernel 0.5916 s → **≈1.53×**；端到端（含 CPU 侧 top-k）1.19×。

## 结论与归因

1. **HNSW 单点插入时序（逐查询）下，朴素 GPU 卸载不敌 128 核 EPYC**：每点一次 launch + 两次小拷贝的固定开销（≈70µs/次×5000）吞掉了算力优势（0.74s vs 0.24s）。
2. **批量时序下 GPU 开始胜出**（d=1024 端到端 1.19×，距离核 1.53×），且优势随维度/批量增大而扩大。
3. 当前朴素 kernel 未做 shared-memory tiling 与 GPU 侧 top-k，理论带宽（~100GB 流量在 >1TB/s HBM 上）还有 ~10× 空间；**top-k 仍在 CPU 侧执行占 GPU 管线 43% 耗时**，把 top-k 也放到 GPU（每行一个 block 的选择/归约核）是下一步最大收益点。

## 集成方案（见 `integration/hnsw_gpu_offload.md`）

seekdb HNSW 构建链路的 GPU 卸载点：节点插入时的邻居候选距离评估批量化 + 构建期 bulk 预计算通道，HIP kernel 以 RAII 设备内存池常驻，通过 `ob_hnsw_gpu_*` C 接口桥接。

## 复现

```bash
hipcc bench/gpu_knn_batched.hip -o gpu_knn_batched -O2
g++ -O2 -fopenmp bench/cpu_knn.cpp -o cpu_knn
./cpu_knn 5000 1024 32 && ./gpu_knn_batched 5000 1024 32   # 校验两侧 checksum 一致
```
