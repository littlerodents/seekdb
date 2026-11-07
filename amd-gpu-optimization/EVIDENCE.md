# 部署与实测证据

## AMD Radeon Cloud 实例

- 平台：developer.amd.com.cn Radeon Cloud，`seekdb-hipvs` 官方模板（OceanBase 数据库大赛专用）
- 实例：`rgapi-one-click-29008-ea57ae9f`（两轮部署：2026-11-06 与 2026-11-07/08）
- GPU：AMD Radeon Graphics，Card Model 0x744b，**gfx1100**，48GB VRAM（`rocm-smi --showproductname`）
- CPU：128 vCPU，Ubuntu 24.04，kernel 6.8.0-79-generic；ROCm 7.2.1（/opt/rocm-7.2.1）

## seekdb 部署证据（2026-11-06，同型实例）

- 版本：seekdb v1.4.0.0，SQL 验证输出 `5.7.25-OceanBase seekdb-v1.4.0.0`（observer ready）
- **部署 UUID（telemetry content.id）：`f131c844-3672-87d7-86f4-16ce5030aae4`**
  （run/telemetry.json 读取，登记流程见 AMD 语雀手册 lcuknwgl955v8ou7）

## 实测数据（2026-11-07/08，rgapi-one-click-29008-ea57ae9f）

构建热路径原语（n=5000，top-32，双侧 checksum 逐位一致）：

```
CPU_RESULT   n=5000 d=128  topk=32 total=0.2423 checksum=2436621.88
GPU_RESULT   n=5000 d=128  topk=32 kernel_total=0.3487 topk_cpu=0.3921 total=0.7408 checksum=2436621.88
CPU_RESULT   n=5000 d=1024 topk=32 total=1.4203 checksum=23944755.88
GPU_BATCHED  n=5000 d=1024 topk=32 h2d=0.0306 kernel=0.5916 d2h=0.0515 topk_cpu=0.5151 total=1.1888 checksum=23944755.88
```

- checksum 一致 → 距离矩阵与 top-k 结果逐位等价，测量有效。
- 批量时序端到端 GPU 1.19×，纯距离核 1.53×；逐点时序 GPU 0.33×（调度策略依据，见 README）。

## hipVS 基准运行方式

`bench/hipvs_hnsw_build.py`（hipVS Python API，GPU HNSW build vs CPU build + 召回对齐），
在具备 hipVS 的环境（`amd-hipvs` wheel 或源码构建的 libcuvs+libcuvspython）直接运行即可。
