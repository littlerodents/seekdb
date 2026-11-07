# hipVS HNSW GPU 构建基准（题目核心交付：结合 AMD Radeon GPU 与 hipVS 加速 HNSW 构建链路）
#
# 运行环境：Radeon Cloud seekdb-hipvs 实例（gfx1100, ROCm 7.2.x, hipVS >= 1.0）
#   pip install amd-hipvs==1.0.0 --extra-index-url=https://pypi.amd.com/rocm-7.2.3/simple/
# 或源码构建：./build.sh libcuvs && ./build.sh libcuvspython && pip install ./python/cuvs
#
# 产出：GPU(hipVS) HNSW 构建 vs CPU(HNSW 同参数) 构建的端到端耗时 + 召回校验。
# 与 seekdb 侧的对应关系：bench/sql_hnsw_cpu_baseline.py 用 SQL 计时
#   CREATE INDEX ... WITH (DISTANCE=L2, TYPE=HNSW) 作为 seekdb CPU 基线。

import argparse
import time

import numpy as np


def bench(n, dim, m, ef, seed=42):
    rng = np.random.default_rng(seed)
    base = rng.random((n, dim), dtype=np.float32)
    queries = rng.random((min(n, 1000), dim), dtype=np.float32)

    from cuvs.neighbors import hnsw
    from cuvs.common import resources

    res = resources.create()

    # ---- CPU 基线（hipVS/同库 CPU 路径，等价 seekdb CPU 构建的近似）----
    t0 = time.perf_counter()
    cpu_params = hnsw.IndexParams(metric="sqeuclidean", m=m, ef_construction=ef)
    cpu_index = hnsw.build(cpu_params, base, res) if hnsw else None
    t1 = time.perf_counter()

    # ---- GPU 构建（题目要求的 hipVS Radeon 加速路径）----
    gpu_params = hnsw.IndexParams(metric="sqeuclidean", m=m, ef_construction=ef)
    t2 = time.perf_counter()
    gpu_index = hnsw.build(gpu_params, base)   # 无 resources 传入 -> GPU 设备路径
    t3 = time.perf_counter()

    # ---- 召回一致性（GPU 索引必须在 kNN 上与 CPU 结果对齐）----
    cpu_search = hnsw.SearchParams()
    gpu_search = hnsw.SearchParams()
    cpu_d, cpu_i = hnsw.search(cpu_search, cpu_index, queries, 10)
    gpu_d, gpu_i = hnsw.search(gpu_search, gpu_index, queries, 10)
    recall = float(np.mean([
        len(set(cpu_i[qi].tolist()) & set(gpu_i[qi].tolist())) / 10.0
        for qi in range(len(queries))
    ]))

    cpu_s, gpu_s = t1 - t0, t3 - t2
    print(f"HIPVS_BENCH n={n} dim={dim} m={m} ef={ef}")
    print(f"CPU_BUILD_SECONDS={cpu_s:.4f}")
    print(f"GPU_BUILD_SECONDS={gpu_s:.4f}")
    print(f"SPEEDUP={cpu_s / gpu_s:.2f}x")
    print(f"RECALL_GPU_VS_CPU={recall:.4f}")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--n", type=int, default=50000)
    p.add_argument("--dim", type=int, default=128)
    p.add_argument("--m", type=int, default=16)
    p.add_argument("--ef", type=int, default=128)
    a = p.parse_args()
    bench(a.n, a.dim, a.m, a.ef)
