

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <openssl/evp.h>

#define MAX_WORDS  1000000
#define WORD_LEN   256

void hash_to_hex(unsigned char *hash, char *hex_string, int len) {
    for (int i = 0; i < len; i++)
        sprintf(hex_string + (i * 2), "%02x", hash[i]);
    hex_string[len * 2] = '\0';
}

int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);  /* My process ID (0 = master) */
    MPI_Comm_size(MPI_COMM_WORLD, &size);  /* Total number of processes   */

    const char *target_hash = "5d41402abc4b2a76b9719d911017c592"; /* MD5("hello") */
    int total_words = 0;

    /* ── Step 1: Master reads the dictionary ─────────────────────────── */
    static char all_words[MAX_WORDS][WORD_LEN];
    if (rank == 0) {
        const char *path = (argc > 1) ? argv[1] : "dictionary.txt";
        FILE *f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "[Node 0] Error: Cannot open %s\n", path);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        while (total_words < MAX_WORDS && fgets(all_words[total_words], WORD_LEN, f)) {
            all_words[total_words][strcspn(all_words[total_words], "\r\n")] = 0;
            total_words++;
        }
        fclose(f);
        printf("[Node 0] Dictionary loaded: %d words\n", total_words);
        printf("[Node 0] Distributing %d words across %d nodes...\n",
               total_words, size);
    }

    /* ── Step 2: Broadcast total word count to all nodes ─────────────── */
    MPI_Bcast(&total_words, 1, MPI_INT, 0, MPI_COMM_WORLD);

    /*
     * Trim total to be divisible by `size` so MPI_Scatter works evenly.
     * The last few words (remainder) are handled by the master separately.
     */
    int chunk = total_words / size;
    int adjusted_total = chunk * size;  /* words that will be scattered */

    /* ── Step 3: Scatter word chunks to all nodes ─────────────────────── */
    /*
     * MPI_Scatter sends all_words[rank*chunk .. (rank+1)*chunk) to node `rank`.
     * Each node receives exactly `chunk` words × WORD_LEN bytes.
     * This is the actual NETWORK TRANSFER happening here.
     */
    static char my_words[MAX_WORDS][WORD_LEN];
    MPI_Scatter(
        all_words,  chunk * WORD_LEN, MPI_CHAR,   /* send buffer on master */
        my_words,   chunk * WORD_LEN, MPI_CHAR,   /* recv buffer on each node */
        0, MPI_COMM_WORLD
    );

    /* ── Step 4: Each node hashes its own chunk ───────────────────────── */
    double t0 = MPI_Wtime();
    char found_word[WORD_LEN];
    memset(found_word, 0, WORD_LEN);

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len;
    char hex[33];

    for (int i = 0; i < chunk; i++) {
        EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
        EVP_DigestUpdate(ctx, my_words[i], strlen(my_words[i]));
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        hash_to_hex(digest, hex, digest_len);

        if (strcmp(hex, target_hash) == 0) {
            strncpy(found_word, my_words[i], WORD_LEN - 1);
            printf("[Node %d] FOUND: \"%s\" at local index %d\n", rank, found_word, i);
            break;
        }
    }
    EVP_MD_CTX_free(ctx);
    double t1 = MPI_Wtime();

    /* ── Step 5: Gather all results back to master ────────────────────── */
    /*
     * MPI_Gather collects found_word from every node into all_results on Node 0.
     * An empty found_word means that node didn't find it.
     */
    static char all_results[64][WORD_LEN];
    MPI_Gather(
        found_word,   WORD_LEN, MPI_CHAR,
        all_results,  WORD_LEN, MPI_CHAR,
        0, MPI_COMM_WORLD
    );

    /* ── Step 6: Master prints the final answer ───────────────────────── */
    if (rank == 0) {
        printf("\n=== MPI Results ===\n");
        int found = 0;
        for (int i = 0; i < size; i++) {
            if (strlen(all_results[i]) > 0) {
                printf("Password found by Node %d: \"%s\"\n", i, all_results[i]);
                found = 1;
            }
        }
        if (!found)
            printf("Password NOT found.\n");

        printf("Nodes       : %d\n", size);
        printf("Time        : %.4f s (wall clock — parallel portion)\n", t1 - t0);
        printf("Chunk/node  : %d words\n", chunk);
    }

    MPI_Finalize();
    return 0;
}
