# G38 HPC — Parallel Password Recovery
## EE7218 / EC7207 | Pabasara · Panditha · Pathirathna

---

## Files

| File | Phase | Method |
|---|---|---|
| `serialProcessing.c` | 1 | Serial baseline (your original) |
| `omp_crack.c` | 2 | OpenMP shared memory |
| `mpi_crack.c` | 3 | MPI distributed memory |
| `hybrid_crack.cu` | 4 | CUDA + OpenMP hybrid |

---

## Compile & Run

### Phase 1 — Serial
```bash
gcc -O2 -lssl -lcrypto -o serial_crack serialProcessing.c
./serial_crack
```

### Phase 2 — OpenMP
```bash
gcc -O2 -fopenmp -lssl -lcrypto -o omp_crack omp_crack.c
./omp_crack                        # auto-detects cores
OMP_NUM_THREADS=8 ./omp_crack      # force 8 threads
```

### Phase 3 — MPI
```bash
# Single machine (4 simulated nodes):
mpicc -O2 -lssl -lcrypto -o mpi_crack mpi_crack.c
mpirun -np 4 ./mpi_crack

# Real cluster (edit hosts file first):
mpirun -np 4 --hostfile hosts ./mpi_crack
```

**hosts file example:**
```
192.168.1.10 slots=1
192.168.1.11 slots=1
192.168.1.12 slots=1
192.168.1.13 slots=1
```

### Phase 4 — Hybrid CUDA + OpenMP
```bash
# Adjust sm_86 to match your GPU architecture:
#   sm_75 = Turing  (RTX 20xx)
#   sm_86 = Ampere  (RTX 30xx)
#   sm_89 = Ada     (RTX 40xx)
nvcc -Xcompiler -fopenmp -O3 -arch=sm_86 -o hybrid hybrid_crack.cu
./hybrid
```

---

## Expected Performance (1M word dictionary)

| Method | Time | Speedup | Efficiency |
|---|---|---|---|
| Serial | ~12.5 s | 1× | 100% |
| OpenMP (12 threads) | ~1.3 s | ~9.6× | ~80% |
| MPI (4 nodes) | ~2.1 s | ~5.9× | ~60–70% |
| Hybrid GPU (RTX 4090) | ~0.015 s | ~833× | GPU-bound |

---

## Evaluation Metrics (from proposal)

**Speedup:**   S = T_serial / T_parallel

**Efficiency:** E = S / P   (P = processors/threads/nodes)

**RMSE (Accuracy):** All methods must find the exact same password.
  - Run each method 10 times, record execution time.
  - RMSE = sqrt( mean( (t_i - t_mean)^2 ) )  — measures stability.
