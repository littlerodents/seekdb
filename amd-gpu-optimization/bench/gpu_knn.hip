#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>

#define CHECK(x) do { hipError_t e = (x); if (e != hipSuccess) { \
  printf("HIP error %s at %d\n", hipGetErrorString(e), __LINE__); exit(1); } } while (0)

__global__ void l2_kernel(const float* __restrict__ base, const float* __restrict__ q,
                          float* __restrict__ dist, int n, int d) {
  int bi = blockIdx.x * blockDim.x + threadIdx.x;
  if (bi >= n) return;
  const float* __restrict__ b = base + (size_t)bi * d;
  const float* __restrict__ qq = q;
  float s = 0.f;
  for (int k = 0; k < d; k++) {
    float t = b[k] - qq[k];
    s += t * t;
  }
  dist[bi] = s;
}

int main(int argc, char** argv) {
  int n = argc > 1 ? atoi(argv[1]) : 5000;
  int d = argc > 2 ? atoi(argv[2]) : 128;
  int topk = argc > 3 ? atoi(argv[3]) : 32;
  srand(42);
  std::vector<float> base((size_t)n * d), queries((size_t)n * d);
  for (auto& v : base) v = (float)rand() / (float)RAND_MAX;
  for (auto& v : queries) v = (float)rand() / (float)RAND_MAX;

  float *d_base, *d_q, *d_dist;
  CHECK(hipMalloc(&d_base, base.size() * 4));
  CHECK(hipMalloc(&d_q, queries.size() * 4));
  CHECK(hipMalloc(&d_dist, (size_t)n * 4));
  CHECK(hipMemcpy(d_base, base.data(), base.size() * 4, hipMemcpyHostToDevice));

  std::vector<float> row(n);
  std::vector<std::pair<float, int>> tmp(n);
  double checksum = 0;
  double kernel_acc = 0, topk_acc = 0;
  for (int qi = 0; qi < n; qi++) {
    auto t2 = std::chrono::high_resolution_clock::now();
    CHECK(hipMemcpy(d_q, queries.data() + (size_t)qi * d, d * 4, hipMemcpyHostToDevice));
    int threads = 256;
    int blocks = (n + threads - 1) / threads;
    hipLaunchKernelGGL(l2_kernel, dim3(blocks), dim3(threads), 0, 0,
                       d_base, d_q, d_dist, n, d);
    CHECK(hipGetLastError());
    CHECK(hipMemcpy(row.data(), d_dist, (size_t)n * 4, hipMemcpyDeviceToHost));
    auto t3 = std::chrono::high_resolution_clock::now();
    kernel_acc += std::chrono::duration<double>(t3 - t2).count();

    for (int bi = 0; bi < n; bi++) tmp[bi] = {row[bi] * (bi == qi ? 0.f : 1.f), bi};
    for (int i = 0; i < topk; i++) {
      for (int j = i + 1; j < n; j++) {
        if (tmp[j].first < tmp[i].first) std::swap(tmp[i], tmp[j]);
      }
    }
    for (int i = 0; i < topk; i++) checksum += tmp[i].first;
    auto t4 = std::chrono::high_resolution_clock::now();
    topk_acc += std::chrono::duration<double>(t4 - t3).count();
  }
  printf("GPU_RESULT n=%d d=%d topk=%d kernel_total=%.4f topk_cpu=%.4f total=%.4f checksum=%.2f\n",
         n, d, topk, kernel_acc, topk_acc, kernel_acc + topk_acc, checksum);
  return 0;
}
