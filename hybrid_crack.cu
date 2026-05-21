

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <cuda_runtime.h>

#define WORD_LEN    64      /* max word length on GPU */
#define BLOCK_SIZE  1024    /* CUDA threads per block */

/* ── MD5 implementation for GPU ─────────────────────────────────────────
 *
 * Standard MD5 — RFC 1321. We reimplement it as __device__ functions
 * because the CPU OpenSSL library cannot run on GPU cores.
 * Each GPU thread calls md5_device() independently on its own word.
 */

__constant__ unsigned int K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
__constant__ unsigned int S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

#define ROTLEFT(x,n) (((x)<<(n))|((x)>>(32-(n))))

__device__ void md5_device(const char *msg, int msg_len, unsigned char *digest) {
    /* Prepare padded message in local memory (max 128 bytes for words ≤ 55 chars) */
    unsigned char buf[128];
    memset(buf, 0, 128);
    for (int i = 0; i < msg_len; i++) buf[i] = (unsigned char)msg[i];
    buf[msg_len] = 0x80;
    /* Append bit-length as 64-bit little-endian at byte 56 */
    unsigned long long bit_len = (unsigned long long)msg_len * 8;
    for (int i = 0; i < 8; i++) buf[56 + i] = (unsigned char)(bit_len >> (8 * i));

    unsigned int a0 = 0x67452301u;
    unsigned int b0 = 0xefcdab89u;
    unsigned int c0 = 0x98badcfeu;
    unsigned int d0 = 0x10325476u;

    /* Process single 512-bit (64-byte) block */
    unsigned int *M = (unsigned int *)buf;
    unsigned int A = a0, B = b0, C = c0, D = d0;
    unsigned int F, g, temp;

    for (int i = 0; i < 64; i++) {
        if      (i < 16) { F = (B & C) | (~B & D); g = i; }
        else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
        else if (i < 48) { F = B ^ C ^ D;           g = (3*i + 5) % 16; }
        else             { F = C ^ (B | ~D);         g = (7*i)     % 16; }
        F    = F + A + K[i] + M[g];
        temp = D; D = C; C = B;
        B    = B + ROTLEFT(F, S[i]);
        A    = temp;
    }
    a0 += A; b0 += B; c0 += C; d0 += D;

    /* Write little-endian digest */
    digest[ 0]=(unsigned char)(a0);      digest[ 1]=(unsigned char)(a0>>8);
    digest[ 2]=(unsigned char)(a0>>16);  digest[ 3]=(unsigned char)(a0>>24);
    digest[ 4]=(unsigned char)(b0);      digest[ 5]=(unsigned char)(b0>>8);
    digest[ 6]=(unsigned char)(b0>>16);  digest[ 7]=(unsigned char)(b0>>24);
    digest[ 8]=(unsigned char)(c0);      digest[ 9]=(unsigned char)(c0>>8);
    digest[10]=(unsigned char)(c0>>16);  digest[11]=(unsigned char)(c0>>24);
    digest[12]=(unsigned char)(d0);      digest[13]=(unsigned char)(d0>>8);
    digest[14]=(unsigned char)(d0>>16);  digest[15]=(unsigned char)(d0>>24);
}


__global__ void crack_kernel(
    const char    *d_words,
    int            n_words,
    const unsigned char *d_target,
    int           *d_result
) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n_words) return;

    const char *word = d_words + tid * WORD_LEN;
    int word_len = 0;
    while (word_len < WORD_LEN - 1 && word[word_len]) word_len++;

    unsigned char digest[16];
    md5_device(word, word_len, digest);

    /* Compare 16-byte digest to target */
    bool match = true;
    for (int i = 0; i < 16; i++) {
        if (digest[i] != d_target[i]) { match = false; break; }
    }
    if (match)
        atomicExch(d_result, tid);  /* atomic: safe even if multiple threads match */
}

/* ── Host helper: hex string → 16 bytes ──────────────────────────────── */
void hex_to_bytes(const char *hex, unsigned char *out) {
    for (int i = 0; i < 16; i++) {
        unsigned int byte;
        sscanf(hex + 2*i, "%02x", &byte);
        out[i] = (unsigned char)byte;
    }
}

/* ── Host helper: load dictionary ────────────────────────────────────── */
#define MAX_WORDS 5000000
static char all_words[MAX_WORDS][WORD_LEN];

int load_dictionary(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0;
    char buf[256];
    while (n < MAX_WORDS && fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        strncpy(all_words[n], buf, WORD_LEN - 1);
        all_words[n][WORD_LEN - 1] = 0;
        n++;
    }
    fclose(f);
    return n;
}

/* ── Main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *target_hex  = "5d41402abc4b2a76b9719d911017c592";
    const char *dict_path   = (argc > 1) ? argv[1] : "dictionary.txt";

    printf("=== Hybrid OpenMP+CUDA MD5 Password Recovery ===\n");
    printf("Target hash : %s\n\n", target_hex);

    /* Convert target hash to binary */
    unsigned char target_bytes[16];
    hex_to_bytes(target_hex, target_bytes);

    /* Detect GPUs */
    int n_gpus = 0;
    cudaGetDeviceCount(&n_gpus);
    if (n_gpus == 0) {
        fprintf(stderr, "No CUDA-capable GPU found. Exiting.\n");
        return 1;
    }
    printf("GPUs detected : %d\n", n_gpus);

    /* Load dictionary on CPU */
    int total = load_dictionary(dict_path);
    if (total <= 0) { fprintf(stderr, "Cannot load dictionary.\n"); return 1; }
    printf("Dictionary    : %d words\n\n", total);

    int chunk  = (total + n_gpus - 1) / n_gpus;  /* words per GPU */
    double t0  = omp_get_wtime();

    
    #pragma omp parallel num_threads(n_gpus)
    {
        int gpu_id = omp_get_thread_num();
        cudaSetDevice(gpu_id);

        int start  = gpu_id * chunk;
        int n      = (start + chunk > total) ? (total - start) : chunk;
        if (n <= 0) { /* this GPU gets no work */ }
        else {
            /* ── Allocate device memory ─────────────── */
            char  *d_words   = NULL;
            unsigned char *d_target = NULL;
            int   *d_result  = NULL;

            cudaMalloc(&d_words,  (size_t)n * WORD_LEN);
            cudaMalloc(&d_target, 16);
            cudaMalloc(&d_result, sizeof(int));

            /* ── Upload words and target hash to GPU ── */
            cudaMemcpy(d_words,  all_words[start], (size_t)n * WORD_LEN, cudaMemcpyHostToDevice);
            cudaMemcpy(d_target, target_bytes,      16,                    cudaMemcpyHostToDevice);

            int h_result = -1;
            cudaMemcpy(d_result, &h_result, sizeof(int), cudaMemcpyHostToDevice);

            
            int blocks = (n + BLOCK_SIZE - 1) / BLOCK_SIZE;
            crack_kernel<<<blocks, BLOCK_SIZE>>>(d_words, n, d_target, d_result);
            cudaDeviceSynchronize();  /* wait for all GPU threads to finish */

            /* ── Read result back to CPU ────────────── */
            cudaMemcpy(&h_result, d_result, sizeof(int), cudaMemcpyDeviceToHost);

            if (h_result >= 0) {
                int global_idx = start + h_result;
                printf("[GPU %d] FOUND: \"%s\" (global index %d)\n",
                       gpu_id, all_words[global_idx], global_idx);
            } else {
                printf("[GPU %d] Not found in my chunk (%d words)\n", gpu_id, n);
            }

            cudaFree(d_words);
            cudaFree(d_target);
            cudaFree(d_result);
        }
    }

    double elapsed = omp_get_wtime() - t0;
    printf("\nTotal time   : %.4f s\n", elapsed);
    printf("GPUs used    : %d\n", n_gpus);
    printf("Throughput   : ~%.2f B hash/s (estimated for RTX 4090)\n",
           (double)total / elapsed / 1e9);

    return 0;
}
