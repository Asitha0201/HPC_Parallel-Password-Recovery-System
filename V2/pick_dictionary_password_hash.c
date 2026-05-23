/*
 * pick_dictionary_password_hash.c
 *
 * Pick a password from near the end of dictionary.txt and print its MD5 hash.
 *
 * Compile:
 *   gcc -O2 -o pick_dictionary_password_hash pick_dictionary_password_hash.c -lssl -lcrypto
 *
 * Run:
 *   ./pick_dictionary_password_hash
 *   ./pick_dictionary_password_hash dictionary.txt
 *   ./pick_dictionary_password_hash dictionary.txt 0      # last valid word
 *   ./pick_dictionary_password_hash dictionary.txt 1000   # 1000th from end
 */

#include <openssl/evp.h>
#include <openssl/md5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_LEN 256
#define DEFAULT_OFFSET_FROM_END 100
#define MD5_DIGEST_LENGTH 16

static void md5_to_hex(const unsigned char *hash, char *output) {
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[32] = '\0';
}

static int compute_md5_hex(const char *word, char *hash_hex) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return 0;
    }

    unsigned char hash[MD5_DIGEST_LENGTH];
    unsigned int digest_len = 0;

    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, word, strlen(word));
    EVP_DigestFinal_ex(ctx, hash, &digest_len);
    EVP_MD_CTX_free(ctx);

    md5_to_hex(hash, hash_hex);
    return 1;
}

int main(int argc, char *argv[]) {
    const char *dict_path = (argc > 1) ? argv[1] : "dictionary.txt";
    int offset_from_end = (argc > 2) ? atoi(argv[2]) : DEFAULT_OFFSET_FROM_END;

    if (offset_from_end < 0) {
        fprintf(stderr, "Error: offset_from_end must be >= 0\n");
        return 1;
    }

    int keep_count = offset_from_end + 1;
    char *ring = (char *)calloc((size_t)keep_count, WORD_LEN);
    long long *line_indexes = (long long *)calloc((size_t)keep_count, sizeof(long long));
    if (!ring || !line_indexes) {
        fprintf(stderr, "Error: failed to allocate buffer\n");
        free(ring);
        free(line_indexes);
        return 1;
    }

    FILE *file = fopen(dict_path, "r");
    if (!file) {
        fprintf(stderr, "Error: cannot open %s\n", dict_path);
        free(ring);
        free(line_indexes);
        return 1;
    }

    char line[WORD_LEN];
    long long line_index = 0;
    long long valid_words = 0;

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] != '\0') {
            int slot = (int)(valid_words % keep_count);
            char *dest = ring + (size_t)slot * WORD_LEN;
            size_t len = strlen(line);
            if (len >= WORD_LEN) {
                len = WORD_LEN - 1;
            }
            memcpy(dest, line, len);
            dest[len] = '\0';
            line_indexes[slot] = line_index;
            valid_words++;
        }

        line_index++;
    }

    fclose(file);

    if (valid_words == 0) {
        fprintf(stderr, "Error: no valid words found in %s\n", dict_path);
        free(ring);
        free(line_indexes);
        return 1;
    }

    if (offset_from_end >= valid_words) {
        fprintf(stderr,
                "Error: offset %d is too large. Dictionary has only %lld valid words.\n",
                offset_from_end, valid_words);
        free(ring);
        free(line_indexes);
        return 1;
    }

    long long target_valid_pos = valid_words - 1 - offset_from_end;
    int target_slot = (int)(target_valid_pos % keep_count);
    const char *password = ring + (size_t)target_slot * WORD_LEN;

    char hash_hex[33];
    if (!compute_md5_hex(password, hash_hex)) {
        fprintf(stderr, "Error: failed to compute MD5 hash\n");
        free(ring);
        free(line_indexes);
        return 1;
    }

    printf("=== Dictionary Password Picker ===\n");
    printf("Dictionary       : %s\n", dict_path);
    printf("Total lines      : %lld\n", line_index);
    printf("Valid words      : %lld\n", valid_words);
    printf("Offset from end  : %d\n", offset_from_end);
    printf("Line index       : %lld\n", line_indexes[target_slot]);
    printf("Password         : %s\n", password);
    printf("MD5 hash         : %s\n", hash_hex);

    free(ring);
    free(line_indexes);
    return 0;
}
