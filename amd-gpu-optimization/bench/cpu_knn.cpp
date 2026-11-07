#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <chrono>

int main(int argc, char** argv) {
  int n = argc > 1 ? atoi(argv[1]) : 5000;
  int d = argc > 2 ? atoi(argv[2]) : 128;
  int topk = argc > 3 ? atoi(argv[3]) : 32;
  srand(42);
  std::vector<float> base((size_t)n * d), queries((size_t)n * d);
  for (auto& v : base) v = (float)rand() / (float)RAND_MAX;
  for (auto& v : queries) v = (float)rand() / (float)RAND_MAX;

  double checksum = 0;
  auto t0 = std::chrono::high_resolution_clock::now();
  #pragma omp parallel
  {
    std::vector<float> dist(n);
    std::vector<std::pair<float, int>> tmp(n);
    #pragma omp for schedule(dynamic, 8) reduction(+:checksum)
    for (int qi = 0; qi < n; qi++) {
      const float* q = queries.data() + (size_t)qi * d;
      for (int bi = 0; bi < n; bi++) {
        const float* b = base.data() + (size_t)bi * d;
        float s = 0.f;
        for (int k = 0; k < d; k++) { float t = b[k] - q[k]; s += t * t; }
        dist[bi] = s;
      }
      for (int bi = 0; bi < n; bi++) tmp[bi] = {dist[bi] * (bi == qi ? 0.f : 1.f), bi};
      for (int i = 0; i < topk; i++) {
        for (int j = i + 1; j < n; j++) {
          if (tmp[j].first < tmp[i].first) std::swap(tmp[i], tmp[j]);
        }
      }
      for (int i = 0; i < topk; i++) checksum += tmp[i].first;
    }
  }
  auto t1 = std::chrono::high_resolution_clock::now();
  double total = std::chrono::duration<double>(t1 - t0).count();
  printf("CPU_RESULT n=%d d=%d topk=%d total=%.4f checksum=%.2f\n", n, d, topk, total, checksum);
  return 0;
}
