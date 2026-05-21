/*
 * omp_crack.c — Phase 2: OpenMP Shared Memory Password Recovery
 * EE7218 / EC7207 High Performance Computing — Group G38
 *
 * HOW IT WORKS:
 *   1. Load the entire dictionary into a shared array in RAM.
 *   2. OpenMP spawns N threads (one per CPU core).
 *   3. Each thread handles a CHUNK of the word list (schedule dynamic).
 *   4. Every thread has its OWN EVP_MD_CTX — this is critical for thread safety.
 *   5. When any thread finds the match it sets `found` inside a critical section.
 *   6. Other threads check `found` at loop start and skip remaining work.
 *
 * Compile:
 *   gcc -O2 -fopenmp -lssl -lcrypto -o omp_crack omp_crack.c
 *
 * Run:
 *   ./omp_crack
 *   OMP_NUM_THREADS=8 ./omp_crack   (force thread count)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <openssl/evp.h>

#define MAX_WORDS  10000000
#define WORD_LEN   256

/* Convert binary MD5 digest to 32-char hex string */
void hash_to_hex(unsigned char *hash, char *hex_string, int len) {
    for (int i = 0; i < len; i++)
        sprintf(hex_string + (i * 2), "%02x", hash[i]);
    hex_string[len * 2] = '\0';
}

/* Load dictionary into memory. Returns word count. */
static char words[MAX_WORDS][WORD_LEN];

int load_dictionary(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Error: Cannot open %s\n", path); return -1; }
    int n = 0;
    while (n < MAX_WORDS && fgets(words[n], WORD_LEN, f)) {
        words[n][strcspn(words[n], "\r\n")] = 0;
        n++;
    }
    fclose(f);
    return n;
}

int main(int argc, char *argv[]) {
    const char *target_hash = "5d41402abc4b2a76b9719d911017c592"; /* MD5("hello") */
    const char *dict_path   = (argc > 1) ? argv[1] : "dictionary.txt";

    printf("=== OpenMP MD5 Password Recovery ===\n");
    printf("Target hash : %s\n", target_hash);
    printf("Threads     : %d\n\n", omp_get_max_threads());

    int n = load_dictionary(dict_path);
    if (n <= 0) return 1;
    printf("Dictionary  : %d words loaded\n\n", n);

    int found_index = -1;   /* -1 = not found yet */
    double t0 = omp_get_wtime();

    /*
     * Parallel region:
     *   - shared(found_index): all threads read/write the same flag
     *   - Each thread allocates its own ctx — no sharing of EVP state
     *   - schedule(dynamic,1000): steal work in 1000-word chunks
     *     so faster cores don't sit idle waiting for slow ones
     */
    #pragma omp parallel shared(found_index)
    {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  digest_len;
        char hex[33];

        #pragma omp for schedule(dynamic, 1000)
        for (int i = 0; i < n; i++) {

            /* Skip remaining work once password is found */
            if (found_index >= 0) continue;

            EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
            EVP_DigestUpdate(ctx, words[i], strlen(words[i]));
            EVP_DigestFinal_ex(ctx, digest, &digest_len);
            hash_to_hex(digest, hex, digest_len);

            if (strcmp(hex, target_hash) == 0) {
                /*
                 * Critical section: only one thread updates found_index.
                 * The double-check (found_index < 0) prevents a second
                 * thread from overwriting after the first already set it.
                 */
                #pragma omp critical
                {
                    if (found_index < 0)
                        found_index = i;
                }
            }
        }
        EVP_MD_CTX_free(ctx);
    }

    double t1 = omp_get_wtime();
    double elapsed = t1 - t0;

    if (found_index >= 0)
        printf("Password found : \"%s\" (index %d)\n", words[found_index], found_index);
    else
        printf("Password NOT found in dictionary.\n");

    printf("Threads        : %d\n", omp_get_max_threads());
    printf("Time           : %.4f s\n", elapsed);
    printf("Throughput     : ~%.2f M hash/s\n", (found_index >= 0 ? found_index : n) / elapsed / 1e6);

    return 0;
}
