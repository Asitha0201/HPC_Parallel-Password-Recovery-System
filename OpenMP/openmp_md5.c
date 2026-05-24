#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <omp.h>

#define MAX_WORDS 14500000
#define MAX_LEN 256

const char *target_hash = "07b6cc8e85893a83ca6c7e3f1ca58ea1";

char words[MAX_WORDS][MAX_LEN];

void hash_to_hex(unsigned char *hash, char *hex_string, int len)
{
    for (int i = 0; i < len; i++)
    {
        sprintf(hex_string + (i * 2), "%02x", hash[i]);
    }
    hex_string[len * 2] = '\0';
}

void compute_md5(const char *input, char *output_hex)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len;

    if (!ctx)
        return;

    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, input, strlen(input));
    EVP_DigestFinal_ex(ctx, digest, &len);

    hash_to_hex(digest, output_hex, len);
    EVP_MD_CTX_free(ctx);
}

int main()
{
    int word_count = 0;
    char found_word[MAX_LEN] = "";
    int found = 0;
    long attempts = 0;

    FILE *file = fopen("dictionary.txt", "r");
    if (!file)
    {
        printf("Error: Cannot open dictionary.txt\n");
        return 1;
    }

    while (word_count < MAX_WORDS && fgets(words[word_count], MAX_LEN, file))
    {
        words[word_count][strcspn(words[word_count], "\r\n")] = 0;
        word_count++;
    }

    fclose(file);

    printf("=== OpenMP MD5 Password Recovery ===\n");
    printf("Target hash: %s\n", target_hash);
    printf("Total words: %d\n", word_count);
    printf("Threads: %d\n\n", omp_get_max_threads());

    double start = omp_get_wtime();

#pragma omp parallel for reduction(+ : attempts)
    for (int i = 0; i < word_count; i++)
    {
        if (found)
            continue;

        char hex_digest[33];
        attempts++;

        compute_md5(words[i], hex_digest);

        if (strcmp(hex_digest, target_hash) == 0)
        {
#pragma omp critical
            {
                if (!found)
                {
                    found = 1;
                    strcpy(found_word, words[i]);
                }
            }
        }
    }

    double end = omp_get_wtime();

    printf(found ? "Password found: %s\n" : "Password not found in dictionary.\n", found_word);
    printf("Attempts: %ld\n", attempts);
    printf("Time: %.6f seconds\n", end - start);

    return 0;
}