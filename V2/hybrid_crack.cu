/*
 *  hybrid_crack.cu
 *  G38 - EE7218 / EC7207  High Performance Computing
 *  Phase 4 : Hybrid CUDA + OpenMP Password Recovery
 *
 *  Compile:
 *      nvcc -O2 -Xcompiler -fopenmp -o hybrid hybrid_crack.cu
 *
 *  Run:
 *      ./hybrid
 *      ./hybrid dictionary.txt
 *
 * -------------------------------------------------------------------
 *  HOW THIS WORKS INTERNALLY
 *
 *  Step 1  CPU reads dictionary into HEAP memory (malloc).
 *          Static arrays in main() go on the stack - 128 MB would
 *          instantly crash. malloc() uses heap which handles it fine.
 *          Words are stored in a flat 1D buffer:
 *              word[i] starts at  (word_buffer + i * WORD_LEN)
 *
 *  Step 2  CPU converts target hash hex string to 16 raw bytes.
 *          "5d41402a..." becomes {0x5d, 0x41, 0x40, ...}
 *          GPU compares bytes not strings - faster.
 *
 *  Step 3  cudaMalloc  allocates VRAM on the GPU card itself.
 *          cudaMemcpy  transfers the word buffer CPU RAM -> GPU VRAM.
 *          GPU cannot read CPU memory directly - must copy first.
 *
 *  Step 4  CPU launches the kernel:
 *              crack_kernel<<<num_blocks, BLOCK_SIZE>>>(...)
 *          This starts (num_blocks * BLOCK_SIZE) GPU threads all at once.
 *          For 1M words: 3907 blocks x 256 threads = ~1M parallel threads.
 *          CPU does NOT block here - it falls through immediately.
 *
 *  Step 5  Every GPU thread independently:
 *          a) Calculates its index: idx = blockIdx.x * blockDim.x + threadIdx.x
 *          b) Gets its word from the flat buffer
 *          c) Calls gpu_md5() to hash that one word
 *          d) Compares the 16 result bytes to target bytes
 *          e) On match: atomicExch(result, idx) - thread-safe write
 *
 *  Step 6  cudaDeviceSynchronize() - CPU waits here until GPU finishes.
 *          Then cudaMemcpy brings the result index back CPU <- GPU.
 *
 *  Step 7  CPU looks up words[result] and prints the password.
 *
 *  Why OpenMP here?
 *      omp_get_wtime() = high-resolution wall clock timer.
 *      On systems with 2+ GPUs, you would spawn one OpenMP thread
 *      per GPU and each calls cudaSetDevice(thread_id) to own its GPU.
 * -------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <cuda_runtime.h>

#define WORD_LEN    64      /* max word length - fits in GPU registers */
#define MAX_WORDS   2000000 /* 2 million words max - loaded via malloc  */
#define BLOCK_SIZE  256     /* 256 threads per CUDA block - safe value  */


/* ================================================================
 *  GPU constant memory - shared cache for all GPU cores.
 *  K[] and S[] are the MD5 standard constants from RFC 1321.
 *  Storing them as __constant__ avoids reading from slow global memory.
 * ================================================================ */

__constant__ unsigned int K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

__constant__ unsigned int S[64] = {
    7,  12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,   9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4,  11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6,  10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

/* Left-rotate a 32-bit value */
#define ROTL32(x, n)  ( ((x) << (n)) | ((x) >> (32 - (n))) )


/* ================================================================
 *  gpu_md5()
 *
 *  __device__ = runs on GPU, called from GPU threads only.
 *  Each thread calls this with its own local word pointer.
 *  No shared state - completely independent per thread.
 *
 *  Only handles strings up to 55 characters (one MD5 block).
 *  Dictionary words are all shorter than this.
 * ================================================================ */

__device__ void gpu_md5(const char *word, int word_len, unsigned char *digest)
{
    int i;

    /* Build the padded 64-byte message block.
     * MD5 processes data in 512-bit (64-byte) chunks.
     * For a short word we construct the entire chunk here:
     *   bytes 0..word_len-1 : the word itself
     *   byte  word_len      : 0x80  (start of padding)
     *   bytes word_len+1..55: 0x00  (zero padding)
     *   bytes 56..63        : bit length as 64-bit little-endian
     */
    unsigned char block[64];
    for (i = 0; i < 64; i++) block[i] = 0;
    for (i = 0; i < word_len; i++) block[i] = (unsigned char)word[i];
    block[word_len] = 0x80;

    unsigned long long bit_len = (unsigned long long)word_len * 8;
    block[56] = (unsigned char)(bit_len);
    block[57] = (unsigned char)(bit_len >> 8);
    block[58] = (unsigned char)(bit_len >> 16);
    block[59] = (unsigned char)(bit_len >> 24);
    /* block[60..63] remain 0 */

    /* Split block into 16 little-endian 32-bit integers */
    unsigned int M[16];
    for (i = 0; i < 16; i++) {
        M[i] = ((unsigned int)block[i*4 + 0])
             | ((unsigned int)block[i*4 + 1] << 8)
             | ((unsigned int)block[i*4 + 2] << 16)
             | ((unsigned int)block[i*4 + 3] << 24);
    }

    /* MD5 starting state (fixed constants from the standard) */
    unsigned int a = 0x67452301;
    unsigned int b = 0xefcdab89;
    unsigned int c = 0x98badcfe;
    unsigned int d = 0x10325476;

    unsigned int a0 = a, b0 = b, c0 = c, d0 = d;

    /* 64 rounds of MD5 mixing.
     * Each round uses a different nonlinear function to mix
     * the 4 state variables with a piece of the message.
     */
    for (i = 0; i < 64; i++) {
        unsigned int F, g;

        if (i < 16) {
            F = (b & c) | (~b & d);    /* rounds  0-15 */
            g = i;
        } else if (i < 32) {
            F = (d & b) | (~d & c);    /* rounds 16-31 */
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            F = b ^ c ^ d;             /* rounds 32-47 */
            g = (3 * i + 5) % 16;
        } else {
            F = c ^ (b | ~d);          /* rounds 48-63 */
            g = (7 * i) % 16;
        }

        F = F + a + K[i] + M[g];
        a = d;
        d = c;
        c = b;
        b = b + ROTL32(F, S[i]);
    }

    a += a0;  b += b0;  c += c0;  d += d0;

    /* Output: 16 bytes, little-endian order */
    digest[0]  = (unsigned char)(a);         digest[1]  = (unsigned char)(a >> 8);
    digest[2]  = (unsigned char)(a >> 16);   digest[3]  = (unsigned char)(a >> 24);
    digest[4]  = (unsigned char)(b);         digest[5]  = (unsigned char)(b >> 8);
    digest[6]  = (unsigned char)(b >> 16);   digest[7]  = (unsigned char)(b >> 24);
    digest[8]  = (unsigned char)(c);         digest[9]  = (unsigned char)(c >> 8);
    digest[10] = (unsigned char)(c >> 16);   digest[11] = (unsigned char)(c >> 24);
    digest[12] = (unsigned char)(d);         digest[13] = (unsigned char)(d >> 8);
    digest[14] = (unsigned char)(d >> 16);   digest[15] = (unsigned char)(d >> 24);
}


/* ================================================================
 *  crack_kernel()
 *
 *  __global__ = called by CPU, runs on GPU.
 *  One thread per dictionary word.
 *  Threads that find a match write their index via atomicExch.
 * ================================================================ */

__global__ void crack_kernel(
    char          *words,
    int            n_words,
    unsigned char *target,
    int           *result
)
{
    /* Each thread computes its unique word index.
     *   blockIdx.x  = which block (group of 256) this thread is in
     *   blockDim.x  = 256 (threads per block)
     *   threadIdx.x = position within the block (0..255)
     */
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n_words) return;

    char *my_word = words + idx * WORD_LEN;

    /* Count word length (no library functions available on GPU) */
    int len = 0;
    while (len < WORD_LEN - 1 && my_word[len] != '\0') len++;

    /* Hash this word */
    unsigned char my_hash[16];
    gpu_md5(my_word, len, my_hash);

    /* Compare 16 bytes to target */
    int matched = 1;
    for (int i = 0; i < 16; i++) {
        if (my_hash[i] != target[i]) { matched = 0; break; }
    }

    /* atomicExch: safe even when multiple threads match simultaneously */
    if (matched) {
        atomicExch(result, idx);
    }
}


/* ================================================================
 *  hex_to_bytes()  -  CPU helper
 *  "5d41402abc4b2a76b9719d911017c592" -> {0x5d, 0x41, 0x40, ...}
 * ================================================================ */

void hex_to_bytes(const char *hex, unsigned char *out)
{
    for (int i = 0; i < 16; i++) {
        unsigned int val = 0;
        sscanf(hex + i * 2, "%02x", &val);
        out[i] = (unsigned char)val;
    }
}


/* ================================================================
 *  main()
 * ================================================================ */

int main(int argc, char *argv[])
{
    const char *target_hex = "1f3870be274f6c49b3e31a0c6728957f";
    const char *dict_path  = (argc > 1) ? argv[1] : "dictionary_old.txt";

    printf("=== Hybrid CUDA + OpenMP Password Cracker ===\n");
    printf("Target : %s\n", target_hex);
    printf("Dict   : %s\n\n", dict_path);

    /* Check CUDA GPU is present */
    int gpu_count = 0;
    if (cudaGetDeviceCount(&gpu_count) != cudaSuccess || gpu_count == 0) {
        printf("ERROR: No CUDA GPU found.\n");
        return 1;
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("GPU    : %s  (%lu MB VRAM)\n\n", prop.name,
           prop.totalGlobalMem / (1024 * 1024));

    /* Convert target hash string to bytes */
    unsigned char target_bytes[16];
    hex_to_bytes(target_hex, target_bytes);

    /* ------- Load dictionary -------
     * malloc on heap - NOT a static/local array!
     * 2M * 64 bytes = 128 MB on heap is fine.
     * 128 MB on the stack = stack overflow = crash.
     */
    char *word_buf = (char *)malloc((size_t)MAX_WORDS * WORD_LEN);
    if (!word_buf) {
        printf("ERROR: malloc failed. Not enough RAM.\n");
        return 1;
    }

    FILE *fp = fopen(dict_path, "r");
    if (!fp) {
        printf("ERROR: Cannot open %s\n", dict_path);
        free(word_buf);
        return 1;
    }

    int n_words = 0;
    char line[256];
    while (n_words < MAX_WORDS && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        int wlen = (int)strlen(line);
        if (wlen == 0 || wlen >= WORD_LEN) continue;
        char *slot = word_buf + n_words * WORD_LEN;
        memset(slot, 0, WORD_LEN);
        memcpy(slot, line, wlen);
        n_words++;
    }
    fclose(fp);
    printf("Loaded : %d words\n", n_words);

    /* ------- GPU memory allocation ------- */
    char          *d_words  = NULL;
    unsigned char *d_target = NULL;
    int           *d_result = NULL;
    size_t wbytes = (size_t)n_words * WORD_LEN;

    if (cudaMalloc(&d_words,  wbytes)     != cudaSuccess ||
        cudaMalloc(&d_target, 16)         != cudaSuccess ||
        cudaMalloc(&d_result, sizeof(int))!= cudaSuccess) {
        printf("ERROR: cudaMalloc failed. Try smaller MAX_WORDS.\n");
        cudaFree(d_words); cudaFree(d_target); cudaFree(d_result);
        free(word_buf); return 1;
    }

    /* ------- Copy CPU -> GPU ------- */
    printf("Copying %.1f MB to GPU VRAM...\n", (float)wbytes / (1024*1024));
    cudaMemcpy(d_words,  word_buf,     wbytes,      cudaMemcpyHostToDevice);
    cudaMemcpy(d_target, target_bytes, 16,           cudaMemcpyHostToDevice);
    int h_result = -1;
    cudaMemcpy(d_result, &h_result,    sizeof(int),  cudaMemcpyHostToDevice);

    /* ------- Launch kernel ------- */
    int num_blocks = (n_words + BLOCK_SIZE - 1) / BLOCK_SIZE;
    printf("Kernel : %d blocks x %d threads = %d GPU threads\n\n",
           num_blocks, BLOCK_SIZE, num_blocks * BLOCK_SIZE);

    double t0 = omp_get_wtime();

    crack_kernel<<<num_blocks, BLOCK_SIZE>>>(d_words, n_words, d_target, d_result);
    cudaDeviceSynchronize();   /* wait for all GPU threads to finish */

    double t1 = omp_get_wtime();

    /* Check kernel errors */
    cudaError_t kerr = cudaGetLastError();
    if (kerr != cudaSuccess)
        printf("Kernel error: %s\n", cudaGetErrorString(kerr));

    /* ------- Copy result GPU -> CPU ------- */
    cudaMemcpy(&h_result, d_result, sizeof(int), cudaMemcpyDeviceToHost);

    /* ------- Print results ------- */
    double elapsed = t1 - t0;
    if (h_result >= 0)
        printf("FOUND  : \"%s\"  (index %d)\n", word_buf + h_result * WORD_LEN, h_result);
    else
        printf("RESULT : Not found in dictionary\n");

    printf("Time   : %.4f s\n", elapsed);
    printf("Speed  : %.2f M hash/s\n", (double)n_words / elapsed / 1e6);

    /* Free everything */
    cudaFree(d_words);  cudaFree(d_target);  cudaFree(d_result);
    free(word_buf);
    return 0;
}
