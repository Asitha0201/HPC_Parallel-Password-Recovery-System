#include <stdio.h>
#include <string.h>
#include <time.h>
#include <openssl/evp.h>

void md5_to_hex(unsigned char *hash, char *output)
{
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(output + (i * 2), "%02x", hash[i]);
    output[32] = '\0';
}

int main()
{
    char target_hash[] = "5d41402abc4b2a76b9719d911017c592";
    char word[256];
    unsigned char hash[MD5_DIGEST_LENGTH];
    unsigned int digest_len; // FIX ADDED
    char hash_hex[33];
    int attempts = 0;
    int found = 0;
    FILE *file = NULL;
    file = fopen("dictionary.txt", "r"); //*file pointer to read the file, "r" for read mode
    if (!file)
    {
        printf("Cannot open dictionary.txt\n");
        return 1;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    if (!ctx)
    {
        printf("Failed to create EVP context\n");
        return 1;
    }
    // printf("=== Serial MD5 Password Recovery ===\n");
    printf("\n=== MD5 Password Recovery ===\n");
    printf("Target hash: %s\n\n", target_hash);

    clock_t start = clock();

    /* Try each word from dictionary */
    while (fgets(word, sizeof(word), file))
    {
        size_t len = strcspn(word, "\r\n");
        word[len] = '\0';
        attempts++;
        EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
        EVP_DigestUpdate(ctx, word, strlen(word));
        EVP_DigestFinal_ex(ctx, hash, &digest_len);
        md5_to_hex(hash, hash_hex); /* convert to hex string */

        if (strcmp(hash_hex, target_hash) == 0)
        { /* compare */
            found = 1;
            printf("Password found: %s\n", word);
            break;
        }
    }

    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;

    EVP_MD_CTX_free(ctx);
    fclose(file);

    if (!found)
        printf("Password not found in dictionary.\n");

    printf("\nAttempts: %d\n", attempts);
    printf("Time: %.4f seconds\n", time_spent);

    return 0;
}