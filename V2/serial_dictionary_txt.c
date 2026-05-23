
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WORD_LEN 256

// use this function to convert binary MD5 digest to hex string
static void md5_to_hex(const unsigned char *hash, char *output) {
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[32] = '\0';
}

int main(int argc, char *argv[]) {
    const char *dict_path = (argc > 1) ? argv[1] : "dictionary.txt";
    const char *target_hash = (argc > 2) ? argv[2] : "7a265bfa1eed87f48aaa30e2c37f6ade"; // we can define target hash from here 

    FILE *file = fopen(dict_path, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open %s\n", dict_path);
        return 1;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fprintf(stderr, "Error: Failed to create EVP_MD_CTX\n");
        fclose(file);
        return 1;
    }

    printf("=== Serial MD5 Password Recovery - dictionary.txt ===\n");
    printf("Target hash : %s\n", target_hash);
    printf("Dictionary  : %s\n", dict_path);
    printf("Max words   : entire file\n\n");

    char word[WORD_LEN];
    unsigned char hash[MD5_DIGEST_LENGTH];
    char hash_hex[33];
    unsigned int digest_len = 0;
    long long attempts = 0;
    int found = 0;

    clock_t cpu_start = clock();
    struct timespec wall_start;
    struct timespec wall_end;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);

    while (fgets(word, sizeof(word), file)) {
        word[strcspn(word, "\r\n")] = '\0';

        EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
        EVP_DigestUpdate(ctx, word, strlen(word));
        EVP_DigestFinal_ex(ctx, hash, &digest_len);
        md5_to_hex(hash, hash_hex);

        attempts++;

        if (strcmp(hash_hex, target_hash) == 0) {
            found = 1;
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

    if (found) {
        printf("Password found : \"%s\"\n", word);
        printf("Index          : %lld\n", attempts - 1);
    } else {
        printf("Password NOT found in dictionary.\n");
    }

    printf("Attempts       : %lld\n", attempts);
    printf("CPU time       : %.4f s\n", cpu_time);
    printf("Wall time      : %.4f s\n", wall_time);

    return 0;
}
