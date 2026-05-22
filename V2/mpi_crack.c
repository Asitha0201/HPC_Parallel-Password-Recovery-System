/*
 * mpi_crack.c — Phase 3: MPI Distributed Memory Password Recovery
 * EE7218 / EC7207 High Performance Computing — Group G38
 *
 * HOW "SPREADING ACROSS MULTIPLE COMPUTERS" WORKS:
 *
 * MPI (Message Passing Interface) lets separate processes running on
 * DIFFERENT PHYSICAL MACHINES communicate over a network (Ethernet /
 * InfiniBand). Each process has its own private memory — they cannot
 * read each other's RAM the way OpenMP threads can.
 *
 * The flow is:
 *
 *   [Node 0 — Master]
 *     ① Reads dictionary.txt into its own RAM.
 *     ② Calls MPI_Bcast  → tells everyone how many words there are.
 *     ③ Calls MPI_Scatter → splits the word array into equal chunks
 *                           and SENDS each chunk over the network
 *                           to the corresponding worker node.
 *
 *   [Node 1 … N-1 — Workers]
 *     ④ MPI_Scatter blocks until they receive their chunk.
 *     ⑤ Each worker hashes only its own slice of the dictionary.
 *     ⑥ Stores the result (found word or empty string) locally.
 *
 *   [All nodes — Gather phase]
 *     ⑦ MPI_Gather → every node sends its result back to Node 0.
 *     ⑧ Node 0 scans the gathered array and prints the answer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <openssl/evp.h>

#define MAX_WORDS  10000000
#define WORD_LEN   256
#define STOP_TAG   99
#define STOP_CHECK_INTERVAL 256

static void hash_to_hex(unsigned char *hash, char *hex_string, int len) {
    for (int i = 0; i < len; i++) {
        sprintf(hex_string + (i * 2), "%02x", hash[i]);
    }
    hex_string[len * 2] = '\0';
}

int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);  /* My process ID (0 = master) */
    MPI_Comm_size(MPI_COMM_WORLD, &size);  /* Total number of processes */

    const char *target_hash = "2e5f7c14c9aad53efae18bde750eb249"; /* MD5("hello") */
    int total_words = 0;

    /*
     * Step 1: Master reads the dictionary.
     *
     * NOTE:
     * Previous versions used huge static arrays in .bss which can fail
     * to link with "relocation truncated" on some toolchains.
     * We allocate on the heap instead.
     */
    char *all_words = NULL;  /* flat array: all_words[i * WORD_LEN + j] */

    if (rank == 0) {
        const char *path = (argc > 1) ? argv[1] : "dictionary_old.txt";
        FILE *f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "[Node 0] Error: Cannot open %s\n", path);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        /* Allocate enough for MAX_WORDS, but still capped by actual file size. */
        size_t capacity_words = MAX_WORDS;
        size_t needed = capacity_words * (size_t)WORD_LEN;
        all_words = (char *)malloc(needed);
        if (!all_words) {
            fprintf(stderr, "[Node 0] Error: Failed to allocate %.2f GB\n",
                    (double)needed / 1e9);
            fclose(f);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        char line[WORD_LEN];
        while (total_words < MAX_WORDS && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            /* copy into flat storage */
            strncpy(all_words + (size_t)total_words * WORD_LEN, line, WORD_LEN - 1);
            all_words[(size_t)total_words * WORD_LEN + WORD_LEN - 1] = 0;
            total_words++;
        }

        fclose(f);
        printf("[Node 0] Dictionary loaded: %d words\n", total_words);
        printf("[Node 0] Distributing %d words across %d nodes...\n", total_words, size);
    }

    /* Step 2: Broadcast total word count to all nodes */
    MPI_Bcast(&total_words, 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (total_words <= 0) {
        if (rank == 0) {
            fprintf(stderr, "No dictionary words loaded.\n");
        }
        MPI_Finalize();
        return 1;
    }

    /*
     * Trim total to be divisible by `size` so MPI_Scatter works evenly.
     * The remainder is ignored here (original behavior).
     */
    int chunk = total_words / size;
    int adjusted_total = chunk * size;
    (void)adjusted_total;

    /*
     * Step 3: Scatter word chunks to all nodes.
     * Each node receives exactly `chunk` words × WORD_LEN bytes.
     */
    char *my_words = NULL;
    if (size == 1) {
        my_words = all_words;
    } else if (chunk > 0) {
        my_words = (char *)malloc((size_t)chunk * WORD_LEN);
        if (!my_words) {
            fprintf(stderr, "[Node %d] Error: Failed to allocate my_words\n", rank);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    if (size > 1 && chunk > 0) {
        MPI_Scatter(
            all_words,        (size_t)chunk * WORD_LEN, MPI_CHAR, /* send buffer (only valid on rank 0) */
            my_words,         (size_t)chunk * WORD_LEN, MPI_CHAR, /* recv buffer (each rank) */
            0, MPI_COMM_WORLD
        );
    }

    /* Step 4: Each node hashes its own chunk */
    double t0 = MPI_Wtime();

    char found_word[WORD_LEN];
    memset(found_word, 0, WORD_LEN);
    int local_found_index = -1;
    int stop_requested = 0;
    long long local_checked = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len;
    char hex[33];

    if (chunk > 0) {
        for (int i = 0; i < chunk; i++) {
            if ((i % STOP_CHECK_INTERVAL) == 0) {
                int flag = 0;
                MPI_Status status;
                MPI_Iprobe(MPI_ANY_SOURCE, STOP_TAG, MPI_COMM_WORLD, &flag, &status);
                if (flag) {
                    MPI_Recv(NULL, 0, MPI_CHAR, status.MPI_SOURCE, STOP_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    stop_requested = 1;
                }
            }
            if (stop_requested) {
                break;
            }

            const char *word = my_words + (size_t)i * WORD_LEN;
            local_checked++;

            EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
            EVP_DigestUpdate(ctx, word, strlen(word));
            EVP_DigestFinal_ex(ctx, digest, &digest_len);
            hash_to_hex(digest, hex, digest_len);

            if (strcmp(hex, target_hash) == 0) {
                local_found_index = i;
                strncpy(found_word, word, WORD_LEN - 1);
                printf("[Node %d] FOUND: \"%s\" at local index %d (global index %d)\n",
                       rank, found_word, i, rank * chunk + i);

                for (int dest = 0; dest < size; dest++) {
                    if (dest != rank) {
                        MPI_Send(NULL, 0, MPI_CHAR, dest, STOP_TAG, MPI_COMM_WORLD);
                    }
                }
                break;
            }
        }
    }

    long long total_checked = 0;
    MPI_Reduce(&local_checked, &total_checked, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    EVP_MD_CTX_free(ctx);
    double t1 = MPI_Wtime();

    /* Step 5: Gather results back to master */
    char *all_results = NULL; /* flat: size × WORD_LEN */
    if (rank == 0) {
        all_results = (char *)malloc((size_t)size * WORD_LEN);
        if (!all_results) {
            fprintf(stderr, "[Node 0] Error: Failed to allocate all_results\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    MPI_Gather(
        found_word, WORD_LEN, MPI_CHAR,
        all_results, WORD_LEN, MPI_CHAR,
        0, MPI_COMM_WORLD
    );

    /* Step 6: Master prints final answer */
    if (rank == 0) {
        printf("\n=== MPI Results ===\n");
        int found = 0;
        for (int i = 0; i < size; i++) {
            char *w = all_results + (size_t)i * WORD_LEN;
            if (strlen(w) > 0) {
                printf("Password found by Node %d: \"%s\"\n", i, w);
                found = 1;
            }
        }
        if (!found) {
            printf("Password NOT found.\n");
        }

        printf("Nodes       : %d\n", size);
        printf("Time        : %.4f s (wall clock — parallel portion)\n", t1 - t0);
        printf("Chunk/node  : %d words\n", chunk);
        printf("Checked     : %lld candidate hashes\n", total_checked);
    }

    if (size > 1) {
        free(my_words);
    }
    if (rank == 0) {
        free(all_words);
        free(all_results);
    }

    MPI_Finalize();
    return 0;
}
