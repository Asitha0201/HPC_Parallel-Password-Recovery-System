#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <omp.h>

#define MAX_LEN 256

const char *target_hash = "07b6cc8e85893a83ca6c7e3f1ca58ea1";

static inline void hash_to_hex(unsigned char *hash, char *hex, int len)
{
    for (int i = 0; i < len; i++)
        sprintf(hex + (i * 2), "%02x", hash[i]);
    hex[len * 2] = '\0';
}

static inline void compute_md5(const char *input, char *out)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len;

    if (!ctx)
        return;

    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, input, strlen(input));
    EVP_DigestFinal_ex(ctx, digest, &len);

    hash_to_hex(digest, out, len);
    EVP_MD_CTX_free(ctx);
}

int main()
{
    char word[MAX_LEN];
    char found_word[MAX_LEN] = "";
    long attempts = 0;
    int found = 0;

    FILE *file = fopen("dictionary.txt", "r");
    if (!file)
    {
        printf("Error: Cannot open dictionary.txt\n");
        return 1;
    }

    printf("=== OpenMP MD5 Password Recovery ===\n");
    printf("Target hash: %s\n", target_hash);
    printf("Threads: %d\n\n", omp_get_max_threads());

    double start = omp_get_wtime();

#pragma omp parallel
    {
        char local[MAX_LEN];

#pragma omp single nowait
        {
            while (fgets(word, MAX_LEN, file))
            {

                if (found)
                    break;

                strcpy(local, word);
                local[strcspn(local, "\r\n")] = 0;

#pragma omp task firstprivate(local)
                {
                    if (found)
                        return;

                    char hex[33];

#pragma omp atomic
                    attempts++;

                    compute_md5(local, hex);

                    if (strcmp(hex, target_hash) == 0)
                    {

#pragma omp critical
                        {
                            if (!found)
                            {
                                found = 1;
                                strcpy(found_word, local);
                            }
                        }
                    }
                }
            }
        }
    }

    fclose(file);

    double end = omp_get_wtime();

    if (found)
        printf("Password found: %s\n", found_word);
    else
        printf("Password not found in dictionary.\n");

    printf("Attempts: %ld\n", attempts);
    printf("Time: %.6f seconds\n", end - start);

    return 0;
}