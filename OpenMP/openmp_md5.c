#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <omp.h>

#define MAX_WORDS 14500000
#define MAX_LEN 256

char words[MAX_WORDS][MAX_LEN];

void hash_to_hex(unsigned char *hash, char *hex_string, int len) {
    for (int i = 0; i < len; i++) {
        sprintf(hex_string + (i * 2), "%02x", hash[i]);
    }
    hex_string[len * 2] = '\0';
}

int main() {
    const char *target_hash = "07cc694b9b3fc636710fa08b6922c42b";

    int word_count = 0;
    char found_word[MAX_LEN] = "";
    int found = 0;
    long attempts = 0;

    FILE *file = fopen("dictionary.txt", "r");
    if (!file) {
        printf("Error: Cannot open dictionary.txt\n");
        return 1;
    }

    while (word_count < MAX_WORDS && fgets(words[word_count], MAX_LEN, file)) {
        words[word_count][strcspn(words[word_count], "\r\n")] = 0;
        word_count++;
    }

    fclose(file);

    printf("=== OpenMP MD5 Password Recovery ===\n");
    printf("Target hash: %s\n", target_hash);
    printf("Total words: %d\n", word_count);
    printf("Threads: %d\n\n", omp_get_max_threads());

    double start = omp_get_wtime();

    #pragma omp parallel for reduction(+:attempts)
    for (int i = 0; i < word_count; i++) {
        if (found) continue;

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_len;
        char hex_digest[33];

        if (ctx == NULL) continue;

        attempts++;

        EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
        EVP_DigestUpdate(ctx, words[i], strlen(words[i]));
        EVP_DigestFinal_ex(ctx, digest, &digest_len);

        hash_to_hex(digest, hex_digest, digest_len);

        if (strcmp(hex_digest, target_hash) == 0) {
            #pragma omp critical
            {
                if (!found) {
                    found = 1;
                    strcpy(found_word, words[i]);
                }
            }
        }

        EVP_MD_CTX_free(ctx);
    }

    double end = omp_get_wtime();

    if (found)
        printf("Password found: %s\n", found_word);
    else
        printf("Password not found in dictionary.\n");

    printf("Attempts: %ld\n", attempts);
    printf("Time: %.6f seconds\n", end - start);

    return 0;
}