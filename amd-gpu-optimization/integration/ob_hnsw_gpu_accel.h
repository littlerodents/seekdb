// ob_hnsw_gpu_accel.h — seekdb HNSW 构建链路 GPU 加速桥（hipVS / ROCm）
//
// 题目要求：结合 AMD Radeon GPU 与 hipVS 优化 seekdb 的 HNSW 索引构建链路。
// 本模块是 seekdb HNSW builder 与 hipVS (AMD GPU 向量检索库, API 兼容 cuVS)
// 之间的桥接层：批量构建/预计算通道走 GPU，逐点插入保留 CPU，混合调度可回退。
//
// 依赖：hipVS >= 1.0 (libcuvs, C API), ROCm >= 7.2, gfx90a/gfx94x/gfx1100+
// 编译期探测：CMake find_package(cuvs CONFIG) 未命中时自动编译 CPU-only 回退。

#pragma once

#include <cstdint>
#include <cstddef>

namespace oceanbase
{
namespace seekdb
{
namespace vecindex
{

// GPU 加速器状态：常驻设备内存池 + hipVS resources。
// 进程级单例；构造失败进入 DISABLED（静默回退 CPU，保证非 ROCm 环境零回归）。
enum class GpuAccelState { DISABLED = 0, READY = 1, FALLBACK = 2 };

struct GpuAccelStats {
  int64_t base_vectors_on_device = 0;   // 常驻基向量数
  int64_t gpu_build_calls = 0;           // GPU 构建调用次数
  int64_t cpu_fallback_calls = 0;       // 回退 CPU 次数
  double  last_build_seconds = 0.0;      // 最近一次 GPU 构建耗时
};

class ObHnswGpuAccel final
{
public:
  // 探测 gfx 架构与可用显存；成功则预建 hipVS resources。幂等。
  static ObHnswGpuAccel &instance();

  // 批量构建通道：把 n 条 dim 维 float 基向量一次性构建为 GPU HNSW 索引。
  // out_index 由调用方持久化（GPU 建索引 + CPU/磁盘部署 = hipVS 官方推荐路径）。
  // 返回 0 成功；非 0 时调用方必须回退 CPU 构建路径。
  int build_index_gpu(const float *vectors, int64_t n, int32_t dim,
                     int32_t m, int32_t ef_construction,
                     void **out_index /* cuvsHnswIndex_t* */);

  // bulk 构建期预取：为待插批次预计算每条向量的 top-k 初始近邻（GPU 一次 launch）。
  // 用于加速 CPU 侧图连接启发式；k 建议 = 2*M。
  int bulk_prefetch_knn(const float *queries, int64_t nq,
                        const float *base, int64_t nb, int32_t dim, int32_t k,
                        int64_t *out_indices, float *out_distances);

  GpuAccelState state() const { return state_; }
  const GpuAccelStats &stats() const { return stats_; }
  const char *device_name() const { return device_name_; }

private:
  ObHnswGpuAccel() = default;
  ~ObHnswGpuAccel();
  ObHnswGpuAccel(const ObHnswGpuAccel &) = delete;
  ObHnswGpuAccel &operator=(const ObHnswGpuAccel &) = delete;

  GpuAccelState state_ = GpuAccelState::DISABLED;
  GpuAccelStats stats_{};
  char device_name_[64] = {'\0'};
  void *resources_ = nullptr;   // cuvsResources_t
  void *device_pool_ = nullptr; // 常驻基向量设备缓冲
};

}  // namespace vecindex
}  // namespace seekdb
}  // namespace oceanbase
