#include <stdio.h>
#include <string.h>
#include <time.h>
#include <openssl/evp.h>

// MD5 binary hash to readable  string 
void md5_to_hex(unsigned char *hash, char *output) {
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(output + (i * 2), "%02x", hash[i]);
    output[32] = '\0';
}

int main() {
    char target_hash[] = "5d41402abc4b2a76b9719d911017c592";  /* MD5 of "hello" */
    char word[256];
    unsigned char hash[MD5_DIGEST_LENGTH];
    char hash_hex[33];
    int attempts = 0;
    int found = 0;

    
    FILE *file = fopen("dictionary.txt", "r");//*file pointer to read the file, "r" for read mode
    if (!file) {
        printf("Cannot open dictionary.txt\n");
        return 1;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();//we use the OpenSSL library to compute the MD5 hash, we create a new context for the hashing operation using EVP_MD_CTX_new().  

    //printf("=== Serial MD5 Password Recovery ===\n");
    printf("Target hash: %s\n\n", target_hash);

    clock_t start = clock();

    /* Try each word from dictionary */
    while (fgets(word, sizeof(word), file)) {
        word[strcspn(word, "\r\n")] = 0;  /* remove newline here this text file make on Windows so line endings are different */
        attempts++;

        /* Compute MD5 hash using EVP API */
        EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
        EVP_DigestUpdate(ctx, word, strlen(word));
        EVP_DigestFinal_ex(ctx, hash, &digest_len);  /* compute MD5 hash */
        md5_to_hex(hash, hash_hex);                        /* convert to hex string */

        if (strcmp(hash_hex, target_hash) == 0) {          /* compare */
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