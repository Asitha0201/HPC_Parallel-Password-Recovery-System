/*
 * omp_dictionary_txt_full.c
 *
 * OpenMP MD5 password recovery over the entire dictionary.txt file.
 * Uses CPU batching so it does not need to load the full 2.7GB file at once.
 *
 * Compile:
 *   gcc -O2 -fopenmp -o omp_dictionary_txt_full omp_dictionary_txt_full.c -lssl -lcrypto
 *
 * Run:
 *   ./omp_dictionary_txt_full
 *   OMP_NUM_THREADS=8 ./omp_dictionary_txt_full
 */

#include <omp.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_LEN 256
#define BATCH_WORDS 1000000

static void hash_to_hex(const unsigned char *hash, char *hex_string, int len) {
    for (int i = 0; i < len; i++) {
        sprintf(hex_string + (i * 2), "%02x", hash[i]);
    }
    hex_string[len * 2] = '\0';
}

int main(int argc, char *argv[]) {
    const char *target_hash = "7a265bfa1eed87f48aaa30e2c37f6ade";
    const char *dict_path = (argc > 1) ? argv[1] : "dictionary.txt";

    FILE *file = fopen(dict_path, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open %s\n", dict_path);
        return 1;
    }

    char *words = (char *)malloc((size_t)BATCH_WORDS * WORD_LEN);
    if (!words) {
        fprintf(stderr, "Error: Failed to allocate batch buffer\n");
        fclose(file);
        return 1;
    }

    printf("=== OpenMP Full dictionary.txt MD5 Password Recovery ===\n");
    printf("Target hash : %s\n", target_hash);
    printf("Dictionary  : %s\n", dict_path);
    printf("Threads     : %d\n", omp_get_max_threads());
    printf("Batch size  : %d words\n\n", BATCH_WORDS);

    long long total_read = 0;
    long long found_index = -1;
    char found_word[WORD_LEN];
    found_word[0] = '\0';

    double start = omp_get_wtime();

    while (found_index < 0) {
        int batch_count = 0;
        char line[WORD_LEN];

        while (batch_count < BATCH_WORDS && fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *slot = words + (size_t)batch_count * WORD_LEN;
            size_t len = strlen(line);
            if (len >= WORD_LEN) {
                len = WORD_LEN - 1;
            }
            memset(slot, 0, WORD_LEN);
            memcpy(slot, line, len);
            batch_count++;
        }

        if (batch_count == 0) {
            break;
        }

        long long batch_start_index = total_read;
        int local_found = -1;

        #pragma omp parallel
        {
            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int digest_len = 0;
            char hash_hex[33];

            #pragma omp for schedule(dynamic, 1000)
            for (int i = 0; i < batch_count; i++) {
                const char *word = words + (size_t)i * WORD_LEN;
                EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
                EVP_DigestUpdate(ctx, word, strlen(word));
                EVP_DigestFinal_ex(ctx, digest, &digest_len);
                hash_to_hex(digest, hash_hex, digest_len);

                if (strcmp(hash_hex, target_hash) == 0) {
                    #pragma omp critical
                    {
                        if (local_found < 0) {
                            local_found = i;
                        }
                    }
                }
            }

            EVP_MD_CTX_free(ctx);
        }

        total_read += batch_count;

        if (local_found >= 0) {
            found_index = batch_start_index + local_found;
            strncpy(found_word, words + (size_t)local_found * WORD_LEN, WORD_LEN - 1);
            found_word[WORD_LEN - 1] = '\0';
            break;
        }

        printf("Scanned %lld words...\n", total_read);
        fflush(stdout);
    }

    double end = omp_get_wtime();

    if (found_index >= 0) {
        printf("\nPassword found : \"%s\"\n", found_word);
        printf("Index          : %lld\n", found_index);
    } else {
        printf("\nPassword NOT found.\n");
    }

    printf("Words scanned  : %lld\n", (found_index >= 0) ? found_index + 1 : total_read);
    printf("Time           : %.4f s\n", end - start);

    free(words);
    fclose(file);
    return 0;
}
