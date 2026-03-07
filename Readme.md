# HPC Parallel Password Recovery System

## Description

This project is a simple password recovery program.
It tries to find passwords by comparing dictionary words with given hash values.

The program reads a list of possible words and checks if any of them match the target hashes after hashing them using SHA256.

This is the basic serial version of the project.

## How it works

1. Read words from `words.txt`
2. Read target hashes from `hashes.txt`
3. Hash each word using SHA256
4. Compare the hash with the target hashes
5. If a match is found, print the password

## Files

* `main.cpp` → main program logic
* `hasher.cpp` → SHA256 hashing function
* `hasher.h` → header file for hashing
* `words.txt` → list of possible passwords
* `hashes.txt` → list of target hashes

## Compile

Run this command inside the project folder:

g++ main.cpp hasher.cpp -o recovery -lcrypto

## Run

./recovery

## Example Output

Password found: password123
Total matches: 1
Execution time: 3 ms

## Future Improvements

* Add OpenMP parallel processing
* Add MPI for distributed computing
* Add CUDA GPU acceleration
