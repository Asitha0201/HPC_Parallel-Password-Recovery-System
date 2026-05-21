/*
 * mpi_crack.c — Phase 3: MPI Distributed Memory Password Recovery
 * EE7218 / EC7207 High Performance Computing — Group G38
 *
 * ─────────────────────────────────────────────────────────────────
 *  HOW "SPREADING ACROSS MULTIPLE COMPUTERS" WORKS:
 * ─────────────────────────────────────────────────────────────────
 *
 *  MPI (Message Passing Interface) lets separate processes running on
 *  DIFFERENT PHYSICAL MACHINES communicate over a network (Ethernet /
 *  InfiniBand). Each process has its own private memory — they cannot
 *  read each other's RAM the way OpenMP threads can.
 *
 *  The flow is:
 *
 *    [Node 0 — Master]
 *      ① Reads dictionary.txt into its own RAM.
 *      ② Calls MPI_Bcast  → tells everyone how many words there are.
 *      ③ Calls MPI_Scatter → splits the word array into equal chunks
 *                            and SENDS each chunk over the network
 *                            to the corresponding worker node.
 *
 *    [Node 1 … N-1 — Workers]
 *      ④ MPI_Scatter blocks until they receive their chunk.
 *      ⑤ Each worker hashes only its own slice of the dictionary.
 *      ⑥ Stores the result (found word or empty string) locally.
 *
 *    [All nodes — Gather phase]
 *      ⑦ MPI_Gather  → every node sends its result back to Node 0.
 *      ⑧ Node 0 scans the gathered array and prints the answer.
 *
 *  KEY POINT — why is MPI sometimes slower than OpenMP?
 *    Network latency: even fast Ethernet adds ~1 ms per MPI_Scatter /
 *    MPI_Gather call. For small dictionaries the latency cost outweighs
 *    the parallelism gain. MPI shines when the dictionary is so large
 *    it no longer fits in one machine's RAM.
 *
 * ─────────────────────────────────────────────────────────────────
 *  Setup (example: 4 machines in a cluster)
 * ─────────────────────────────────────────────────────────────────
 *
 *  1. Install OpenMPI on each machine:
 *       sudo apt install openmpi-bin libopenmpi-dev
 *
 *  2. Create a hostfile (one IP / hostname per line):
 *       node0  slots=1
 *       node1  slots=1
 *       node2  slots=1
 *       node3  slots=1
 *
 *  3. Make sure SSH key-based login works between all nodes.
 *     Copy the binary and dictionary.txt to all nodes (or use NFS).
 *
 *  4. Compile on any node:
 *       mpicc -O2 -lssl -lcrypto -o mpi_crack mpi_crack.c
 *
 *  5. Run:
 *       mpirun -np 4 --hostfile hosts ./mpi_crack
 *
 *  For testing on a single machine (simulates multiple processes):
 *       mpirun -np 4 ./mpi_crack
 */

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
