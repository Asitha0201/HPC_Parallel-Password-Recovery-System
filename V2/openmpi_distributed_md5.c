/*
 * openmpi_distributed_md5.c
 *
 * Efficient multi-rank Open MPI MD5 password recovery.
 *
 * Key idea:
 *   Do NOT load the dictionary on rank 0 and scatter huge buffers.
 *   Instead, every MPI rank streams the same dictionary file and hashes only
 *   the line numbers assigned to it:
 *
 *     Rank 0: lines 0, size, 2*size, ...
 *     Rank 1: lines 1, size+1, 2*size+1, ...
 *
 *   This cyclic distribution keeps the work balanced and avoids sending
 *   gigabytes through MPI_Scatter.
 *
 * Compile with Open MPI:
 *   mpicc -O2 -o openmpi_distributed_md5 openmpi_distributed_md5.c -lssl -lcrypto
 *
 * Run:
 *   mpirun -np 4 ./openmpi_distributed_md5
 *   mpirun -np 4 ./openmpi_distributed_md5 dictionary_old.txt
 */

#include <mpi.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WORDS 10000000
#define WORD_LEN 256
#define STOP_CHECK_LINES 50000

static void hash_to_hex(const unsigned char *hash, char *hex_string, int len) {
    for (int i = 0; i < len; i++) {
        sprintf(hex_string + (i * 2), "%02x", hash[i]);
    }
    hex_string[len * 2] = '\0';
}

int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    const char *target_hash = "2e5f7c14c9aad53efae18bde750eb249";
    const char *dict_path = (argc > 1) ? argv[1] : "dictionary_old.txt";

    if (rank == 0) {
        printf("=== Open MPI Distributed MD5 Password Recovery ===\n");
        printf("Target hash : %s\n", target_hash);
        printf("Dictionary  : %s\n", dict_path);
        printf("Max words   : %d\n", MAX_WORDS);
        printf("MPI ranks   : %d\n", size);
        printf("Distribution: cyclic line assignment, no MPI_Scatter\n\n");
    }

    MPI_Barrier(MPI_COMM_WORLD);
    double total_start = MPI_Wtime();

    FILE *file = fopen(dict_path, "r");
    if (!file) {
        fprintf(stderr, "[Rank %d] Error: Cannot open %s\n", rank, dict_path);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fprintf(stderr, "[Rank %d] Error: Failed to create EVP_MD_CTX\n", rank);
        fclose(file);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    char word[WORD_LEN];
    char found_word[WORD_LEN];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    char hash_hex[33];

    found_word[0] = '\0';
    int found_local_index = -1;
    int found_global_index = -1;
    int global_stop_index = -1;
    int line_index = 0;
    long long local_checked = 0;

    double search_start = MPI_Wtime();

    while (line_index < MAX_WORDS && fgets(word, sizeof(word), file)) {
        if ((line_index % STOP_CHECK_LINES) == 0) {
            int local_signal = (found_global_index >= 0) ? found_global_index : -1;
            MPI_Allreduce(&local_signal, &global_stop_index, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
            if (global_stop_index >= 0) {
                break;
            }
        }

        if (found_global_index < 0 && (line_index % size) == rank) {
            word[strcspn(word, "\r\n")] = '\0';

            EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
            EVP_DigestUpdate(ctx, word, strlen(word));
            EVP_DigestFinal_ex(ctx, digest, &digest_len);
            hash_to_hex(digest, hash_hex, digest_len);

            local_checked++;

            if (strcmp(hash_hex, target_hash) == 0) {
                found_local_index = (int)local_checked - 1;
                found_global_index = line_index;
                strncpy(found_word, word, WORD_LEN - 1);
                found_word[WORD_LEN - 1] = '\0';
            }
        }

        line_index++;
    }

    {
        int local_signal = (found_global_index >= 0) ? found_global_index : -1;
        MPI_Allreduce(&local_signal, &global_stop_index, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    }

    double search_end = MPI_Wtime();

    EVP_MD_CTX_free(ctx);
    fclose(file);

    long long total_checked = 0;
    MPI_Reduce(&local_checked, &total_checked, 1, MPI_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    double local_search_time = search_end - search_start;
    double max_search_time = 0.0;
    MPI_Reduce(&local_search_time, &max_search_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    char *all_found_words = NULL;
    int *all_found_indexes = NULL;
    int *all_found_local_indexes = NULL;
    long long *all_checked = NULL;

    if (rank == 0) {
        all_found_words = (char *)malloc((size_t)size * WORD_LEN);
        all_found_indexes = (int *)malloc((size_t)size * sizeof(int));
        all_found_local_indexes = (int *)malloc((size_t)size * sizeof(int));
        all_checked = (long long *)malloc((size_t)size * sizeof(long long));
        if (!all_found_words || !all_found_indexes || !all_found_local_indexes || !all_checked) {
            fprintf(stderr, "[Rank 0] Error: Failed to allocate result buffers\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    MPI_Gather(found_word, WORD_LEN, MPI_CHAR,
               all_found_words, WORD_LEN, MPI_CHAR,
               0, MPI_COMM_WORLD);

    MPI_Gather(&found_global_index, 1, MPI_INT,
               all_found_indexes, 1, MPI_INT,
               0, MPI_COMM_WORLD);

    MPI_Gather(&found_local_index, 1, MPI_INT,
               all_found_local_indexes, 1, MPI_INT,
               0, MPI_COMM_WORLD);

    MPI_Gather(&local_checked, 1, MPI_LONG_LONG,
               all_checked, 1, MPI_LONG_LONG,
               0, MPI_COMM_WORLD);

    double total_time = MPI_Wtime() - total_start;
    double max_total_time = 0.0;
    MPI_Reduce(&total_time, &max_total_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    printf("[Rank %d] checked %lld candidates%s\n",
           rank,
           local_checked,
           (found_global_index >= 0) ? " and found the password" : "");
    fflush(stdout);

    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        int found_rank = -1;
        for (int r = 0; r < size; r++) {
            if (all_found_indexes[r] >= 0) {
                found_rank = r;
                break;
            }
        }

        printf("\n=== Result ===\n");
        if (found_rank >= 0) {
            printf("Password found by Rank %d: \"%s\"\n",
                   found_rank, all_found_words + (size_t)found_rank * WORD_LEN);
            printf("Global index : %d\n", all_found_indexes[found_rank]);
            printf("Local index  : %d\n", all_found_local_indexes[found_rank]);
        } else {
            printf("Password NOT found.\n");
        }

        printf("\n=== Work Distribution ===\n");
        for (int r = 0; r < size; r++) {
            printf("Rank %d checked %lld candidates\n", r, all_checked[r]);
        }

        printf("\n=== Timing ===\n");
        printf("Search time : %.4f s\n", max_search_time);
        printf("Total time  : %.4f s\n", max_total_time);
        printf("Checked     : %lld candidate hashes\n", total_checked);
    }

    if (rank == 0) {
        free(all_found_words);
        free(all_found_indexes);
        free(all_found_local_indexes);
        free(all_checked);
    }

    MPI_Finalize();
    return 0;
}
