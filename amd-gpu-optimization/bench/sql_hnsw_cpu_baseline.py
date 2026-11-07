"""seekdb SQL 侧 CPU 基线：HNSW 构建链路时序（导入/CREATE INDEX/检索）。

前置：seekdb observer 已部署（observer ready，端口 2881）。
GPU 侧对比数字来自 hipvs_hnsw_build.py（同数据、同参数、召回对齐后）。

用法：python3 sql_hnsw_cpu_baseline.py
"""
import json
import random
import subprocess
import time

N = 5000
DIM = 128
BATCH = 500


def rows(n):
    random.seed(42)
    out = []
    for _ in range(n):
        v = ",".join(f"{random.random():.6f}" for _ in range(DIM))
        out.append("('p', '[" + v + "]')")
    return out


def run_sql(sql):
    r = subprocess.run(
        ["bash", "-lc", f"mysql -h127.0.0.1 -P2881 -uroot -N -e {json.dumps(sql)}"],
        capture_output=True, text=True, timeout=600)
    return r.stdout.strip(), r.stderr.strip()[:300]


def main():
    out = {}
    o, e = run_sql("CREATE DATABASE IF NOT EXISTS bench")
    out["db"] = e or "ok"
    o, e = run_sql(
        "DROP TABLE IF EXISTS bench.vec_bench; "
        "CREATE TABLE bench.vec_bench (id INT PRIMARY KEY AUTO_INCREMENT, "
        "payload VARCHAR(64), v VECTOR(128))")
    out["ddl"] = e or "ok"

    t0 = time.time()
    for _ in range(0, N, BATCH):
        o, e = run_sql(
            "INSERT INTO bench.vec_bench(payload, v) VALUES " + ",".join(rows(BATCH)))
        if e:
            out["ins_err"] = e
            break
    out["insert_sec"] = round(time.time() - t0, 2)

    t0 = time.time()
    o, e = run_sql(
        "CREATE INDEX vidx ON bench.vec_bench(v) WITH (DISTANCE=L2, TYPE=HNSW)")
    out["hnsw_build_sec"] = round(time.time() - t0, 2)   # <- 构建链路核心指标
    out["hnsw_err"] = e or "ok"

    q = "[" + ",".join(["0.5"] * DIM) + "]"
    t0 = time.time()
    o, e = run_sql(
        f"SELECT id FROM bench.vec_bench "
        f"ORDER BY l2_distance(v, '{q}') APPROXIMATE LIMIT 5")
    out["search_sec"] = round(time.time() - t0, 2)
    out["search_out"] = o[:100]
    out["search_err"] = e or "ok"

    print("<<<BENCH>>>" + json.dumps(out, ensure_ascii=False))


if __name__ == "__main__":
    main()
