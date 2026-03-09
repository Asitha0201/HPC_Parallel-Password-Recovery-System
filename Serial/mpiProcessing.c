#include <stdio.h>
#include <string.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <mpi.h> /* MPI parallelism */

// MD5 binary hash to readable  string
void md5_to_hex(unsigned char *hash, char *output)
{
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(output + (i * 2), "%02x", hash[i]);
    output[32] = '\0';
}

int main(int argc, char *argv[])
{
    /* MPI initialization */
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    char target_hash[] = "5d41402abc4b2a76b9719d911017c592"; /* MD5 of "hello" */
    char word[256];
    unsigned char hash[MD5_DIGEST_LENGTH];
    unsigned int digest_len = 0; /* length output from EVP_DigestFinal_ex */
    char hash_hex[33];
    int attempts = 0;
    int found = 0;

    FILE *file = fopen("dictionary.txt", "r"); //*file pointer to read the file, "r" for read mode
    if (!file)
    {
        if (rank == 0)
            printf("Cannot open dictionary.txt\n");
        MPI_Finalize();
        return 1;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new(); // we use the OpenSSL library to compute the MD5 hash, we create a new context for the hashing operation using EVP_MD_CTX_new().

    if (rank == 0)
        printf("Target hash: %s\n\n", target_hash);

    double start = MPI_Wtime();

    /* Try each word from dictionary, distributing lines across ranks */
    int line_no = 0;
    while (fgets(word, sizeof(word), file))
    {
        if ((line_no % size) != rank)
        {
            line_no++;
            continue;
        }
        line_no++;

        word[strcspn(word, "\r\n")] = 0;
        attempts++;

        /* Compute MD5 hash using EVP API */
        EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
        EVP_DigestUpdate(ctx, word, strlen(word));
        EVP_DigestFinal_ex(ctx, hash, &digest_len);
        md5_to_hex(hash, hash_hex);

        if (strcmp(hash_hex, target_hash) == 0)
        {
            found = 1;
            printf("[rank %d] Password found: %s\n", rank, word);
            break;
        }
    }

    /* gather timings and results */
    double local_end = MPI_Wtime();
    double local_time = local_end - start;

    EVP_MD_CTX_free(ctx);
    fclose(file);

    int global_found;
    MPI_Allreduce(&found, &global_found, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);

    int total_attempts;
    MPI_Reduce(&attempts, &total_attempts, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    double max_time;
    MPI_Reduce(&local_time, &max_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    if (rank == 0)
    {
        if (!global_found)
            printf("Password not found in dictionary.\n");
        printf("\nTotal attempts: %d\n", total_attempts);
        printf("Elapsed time (max over ranks): %.4f seconds\n", max_time);
    }

    MPI_Finalize();
    return 0;
}
