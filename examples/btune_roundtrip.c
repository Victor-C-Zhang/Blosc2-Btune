#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <btune.h>
#include <blosc2/tuners-registry.h>
#include "blosc2.h"


#define KB  1024.
#define MB  (1024*KB)


long get_file_size(FILE *file) {
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file); // Reset the file pointer to the beginning
    return length;
}
// Function to copy a file to a buffer
void copy_file_to_buffer(FILE *file, char *buffer, long length) {
    size_t bytes_read = fread(buffer, 1, length, file);
    if (bytes_read != length) {
        fprintf(stderr, "Error reading file: expected %ld bytes, read %zu bytes\n", length, bytes_read);
        exit(EXIT_FAILURE);
    }
}

static int round_trip(const char* in_fname) {
    // Open input file
    FILE* in_file = fopen(in_fname, "rb");
    if (in_file == NULL) {
        fprintf(stderr, "Input file cannot be opened.\n");
        return EXIT_FAILURE;
    }
    long file_size = get_file_size(in_file);
    printf("File length: %ld bytes\n", file_size);
    char *buffer = malloc(file_size + 1); // Allocate space for the file contents and a null terminator
    if (!buffer) {
        fprintf(stderr, "Error allocating memory for file buffer\n");
        fclose(in_file);
        return EXIT_FAILURE;
    }
    copy_file_to_buffer(in_file, buffer, file_size);
    buffer[file_size] = '\0'; // Add a null terminator to the end of the buffer
    fclose(in_file);

    // compression params
    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.nthreads = 1; // Btune may lower this
    cparams.typesize = 8;

    // btune
    btune_config btune_config = BTUNE_CONFIG_DEFAULTS;
    //btune_config.perf_mode = BTUNE_PERF_DECOMP;
    btune_config.tradeoff[0] = .5;
    btune_config.tradeoff_nelems = 1;
    /* For lossy mode it would be
    btune_config.tradeoff[0] = .5;
    btune_config.tradeoff[1] = .2;
    btune_config.tradeoff[2] = .3;
    btune_config.tradeoff_nelems = 3;
    */
    btune_config.behaviour.nhards_before_stop = 10;
    btune_config.behaviour.repeat_mode = BTUNE_REPEAT_ALL;
    btune_config.use_inference = 2;
    char *models_dir = "./models/";
    strcpy(btune_config.models_dir, models_dir);
    cparams.tuner_id = BLOSC_BTUNE;
    cparams.tuner_params = &btune_config;

    // Create super chunk
    blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
    dparams.nthreads = 1;
    blosc2_storage storage = {
        .cparams=&cparams,
        .dparams=&dparams,
        .contiguous=true,
        .urlpath=NULL,
    };
    blosc2_schunk* schunk_out = blosc2_schunk_new(&storage);
    if (schunk_out == NULL) {
        fprintf(stderr, "Output file cannot be created.\n");
        return 1;
    }

    // Statistics
    blosc_timestamp_t c0;
    blosc_timestamp_t c1;
    blosc_timestamp_t d0;
    blosc_timestamp_t d1;
    double ctotal = 0.0;
    double dtotal = 0.0;

    // Compress
    const int numChunks = 50; // give BTune a reasonable number of iterations to come online
    int chunkSize = file_size / numChunks;
    int leftover = file_size - chunkSize * numChunks;
    for (int i = 0; i < numChunks; i++) {
        blosc_set_timestamp(&c0);
        const int res = blosc2_schunk_append_buffer(schunk_out, buffer + chunkSize * i, chunkSize);
        blosc_set_timestamp(&c1);
        if (res != i + 1) {
            fprintf(stderr, "Error in appending data to destination file");
            return 1;
        }
        ctotal += blosc_elapsed_secs(c0, c1);
    }
    if (leftover > 0) {
        blosc_set_timestamp(&c0);
        const int res = blosc2_schunk_append_buffer(schunk_out, buffer + chunkSize * numChunks, leftover);
        blosc_set_timestamp(&c1);
        if (res != numChunks + 1) {
            fprintf(stderr, "Error in appending (leftover) data to destination file");
            return 1;
        }
        ctotal += blosc_elapsed_secs(c0, c1);
    }

    // Decompress
    char* regen = malloc(file_size + 1);
    regen[file_size] = '\0';
    for (int i = 0; i < numChunks; i++) {
        blosc_set_timestamp(&d0);
        int size = blosc2_schunk_decompress_chunk(schunk_out, i, regen + chunkSize * i, chunkSize);
        blosc_set_timestamp(&d1);
        if (size != chunkSize) {
            fprintf(stderr, "Error in decompressing data");
            return 1;
        }
        dtotal += blosc_elapsed_secs(d0, d1);
    }
    if (leftover > 0) {
        blosc_set_timestamp(&d0);
        int size = blosc2_schunk_decompress_chunk(schunk_out, numChunks, regen + chunkSize * numChunks, leftover);
        blosc_set_timestamp(&d1);
        if (size != leftover) {
            fprintf(stderr, "Error in decompressing (leftover) data");
            return 1;
        }
        dtotal += blosc_elapsed_secs(d0, d1);
    }

    if (memcmp(buffer, regen, file_size) != 0) {
        fprintf(stderr, "Roundtrip failed!");
        return 1;
    }

    // Statistics
    int64_t nbytes = schunk_out->nbytes;
    int64_t cbytes = schunk_out->cbytes;
    printf("Compression ratio: %.1f MiB -> %.1f MiB (%.1fx)\n",
            (float)nbytes / MB, (float)cbytes / MB, (1. * (float)nbytes) / (float)cbytes);
    printf("Decompression time: %.3g s, %.1f MiB/s\n", dtotal, (float)nbytes / (dtotal * MB));
    printf("Compression time: %.3g s, %.1f MiB/s\n",
            ctotal, (float)nbytes / (ctotal * MB));

    // Free resources
    blosc2_schunk_free(schunk_out);
    free(buffer);
    free(regen);
    return 0;
}


int main(int argc, char* argv[]) {
    blosc2_init();

    // Input parameters
    if (argc != 2) {
        fprintf(stderr, "btune_example <input file>\n");
        return 1;
    }

    const char* in_fname = argv[1];
    round_trip(in_fname);

    blosc2_destroy();

    return 0;
}
