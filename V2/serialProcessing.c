#include <stdio.h>
#include <string.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/md5.h>

#define MAX_WORDS 100000000
#define WORD_LEN 512

// MD5 binary hash to readable string
static void md5_to_hex(const unsigned char *hash, char *output) {
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[32] = '\0';
}

int main(int argc, char *argv[]) {
    char target_hash[] = "2e5f7c14c9aad53efae18bde750eb249"; /* MD5 of "hello" */
    char word[WORD_LEN];
    unsigned char hash[MD5_DIGEST_LENGTH];
    char hash_hex[33];
    unsigned int digest_len = 0;
    int attempts = 0;
    int found = 0;

    const char *dict_path = (argc > 1) ? argv[1] : "dictionary_old.txt";

    FILE *file = fopen(dict_path, "r");
    if (!file) file = fopen("V2/dictionary_old.txt", "r");
    if (!file) file = fopen("Serial/dictionary_old.txt", "r");
    if (!file) file = fopen("Serial/dictionary.txt", "r");
    if (!file) file = fopen("dictionary_old.txt", "r");
    if (!file) file = fopen("dictionary.txt", "r");

    if (!file) {
        printf("Cannot open dictionary file.\n");
        return 1;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        printf("Failed to create EVP_MD_CTX\n");
        fclose(file);
        return 1;
    }

    printf("Target hash: %s\n", target_hash);
    printf("Max words  : %d\n\n", MAX_WORDS);

    clock_t cpu_start = clock();
    struct timespec wall_start;
    struct timespec wall_end;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);

    /* Try each word from dictionary */
    while (attempts < MAX_WORDS && fgets(word, sizeof(word), file)) {
        word[strcspn(word, "\r\n")] = 0; /* remove newline */
        attempts++;

        EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
        EVP_DigestUpdate(ctx, word, strlen(word));
        EVP_DigestFinal_ex(ctx, hash, &digest_len);

        md5_to_hex(hash, hash_hex);

        if (strcmp(hash_hex, target_hash) == 0) {
            found = 1;
            printf("Password found: %s\n", word);
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    clock_t cpu_end = clock();
    double cpu_time = (double)(cpu_end - cpu_start) / CLOCKS_PER_SEC;
    double wall_time = (double)(wall_end.tv_sec - wall_start.tv_sec)
                     + (double)(wall_end.tv_nsec - wall_start.tv_nsec) / 1e9;

    EVP_MD_CTX_free(ctx);
    fclose(file);

    if (!found)
        printf("Password not found in dictionary.\n");

    printf("\nAttempts: %d\n", attempts);
    printf("CPU time: %.4f seconds\n", cpu_time);
    printf("Wall time: %.4f seconds\n", wall_time);

    return 0;
}

