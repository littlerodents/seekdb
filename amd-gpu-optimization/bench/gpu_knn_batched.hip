#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>

#define CHECK(x) do { hipError_t e = (x); if (e != hipSuccess) { \
  printf("HIP error %s at %d\n", hipGetErrorString(e), __LINE__); exit(1); } } while (0)

__global__ void l2_batched(const float* __restrict__ base, const float* __restrict__ q,
                           float* __restrict__ dist, int n, int d) {
  int qi = blockIdx.y;
  int bi = blockIdx.x * blockDim.x + threadIdx.x;
  if (bi >= n) return;
  const float* __restrict__ b = base + (size_t)bi * d;
  const float* __restrict__ qq = q + (size_t)qi * d;
  float s = 0.f;
  for (int k = 0; k < d; k++) {
    float t = b[k] - qq[k];
    s += t * t;
  }
  dist[(size_t)qi * n + bi] = s;
}

int main(int argc, char** argv) {
  int n = argc > 1 ? atoi(argv[1]) : 5000;
  int d = argc > 2 ? atoi(argv[2]) : 1024;
  int topk = argc > 3 ? atoi(argv[3]) : 32;
  srand(42);
  std::vector<float> base((size_t)n * d), queries((size_t)n * d);
  for (auto& v : base) v = (float)rand() / (float)RAND_MAX;
  for (auto& v : queries) v = (float)rand() / (float)RAND_MAX;

  float *d_base, *d_q, *d_dist;
  CHECK(hipMalloc(&d_base, base.size() * 4));
  CHECK(hipMalloc(&d_q, queries.size() * 4));
  CHECK(hipMalloc(&d_dist, (size_t)n * n * 4));

  auto t0 = std::chrono::high_resolution_clock::now();
  CHECK(hipMemcpy(d_base, base.data(), base.size() * 4, hipMemcpyHostToDevice));
  CHECK(hipMemcpy(d_q, queries.data(), queries.size() * 4, hipMemcpyHostToDevice));
  auto t1 = std::chrono::high_resolution_clock::now();

  int threads = 256;
  dim3 block((n + threads - 1) / threads, n);
  hipLaunchKernelGGL(l2_batched, block, dim3(threads), 0, 0, d_base, d_q, d_dist, n, d);
  CHECK(hipGetLastError());
  CHECK(hipDeviceSynchronize());
  auto t2 = std::chrono::high_resolution_clock::now();

  std::vector<float> dists((size_t)n * n);
  CHECK(hipMemcpy(dists.data(), d_dist, (size_t)n * n * 4, hipMemcpyDeviceToHost));
  auto t3 = std::chrono::high_resolution_clock::now();

  std::vector<std::pair<float, int>> tmp(n);
  double checksum = 0;
  auto t4 = std::chrono::high_resolution_clock::now();
  for (int qi = 0; qi < n; qi++) {
    const float* row = dists.data() + (size_t)qi * n;
    for (int bi = 0; bi < n; bi++) tmp[bi] = {row[bi] * (bi == qi ? 0.f : 1.f), bi};
    for (int i = 0; i < topk; i++) {
      for (int j = i + 1; j < n; j++) {
        if (tmp[j].first < tmp[i].first) std::swap(tmp[i], tmp[j]);
      }
    }
    for (int i = 0; i < topk; i++) checksum += tmp[i].first;
  }
  auto t5 = std::chrono::high_resolution_clock::now();

  double h2d = std::chrono::duration<double>(t1 - t0).count();
  double kern = std::chrono::duration<double>(t2 - t1).count();
  double d2h = std::chrono::duration<double>(t3 - t2).count();
  double tk = std::chrono::duration<double>(t5 - t4).count();
  printf("GPU_BATCHED n=%d d=%d topk=%d h2d=%.4f kernel=%.4f d2h=%.4f topk_cpu=%.4f total=%.4f checksum=%.2f\n",
         n, d, topk, h2d, kern, d2h, tk, h2d + kern + d2h + tk, checksum);
  return 0;
}
