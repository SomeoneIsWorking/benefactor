/* tests/test_lockstep_digest.c — the bytes the two products must agree on.
 *
 * `src/port/lockstep_digest.h` is the ONE implementation of the lockstep
 * protocol: this product includes it, and `tools/oracle_diff.py` copies the
 * same file into the reference worktree so the reference includes it too. A
 * difference between the two products' digests therefore means their guest
 * memory differs — never that they hashed it differently. These tests hold
 * that: the hash notices a change and puts it in the right region, and the
 * lines it formats are the ones `tools/lockstep.py` parses. */
#include "port/lockstep_digest.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MEMORY_BYTES 8192u
#define REGIONS 8u

static void fill(unsigned char *memory, size_t bytes) {
    for (size_t index = 0; index < bytes; index++)
        memory[index] = (unsigned char)(index * 31u + 7u);
}

static void test_hash_notices_a_single_changed_byte(void) {
    unsigned char memory[64];
    fill(memory, sizeof memory);
    const uint64_t before = lockstep_hash(memory, sizeof memory);
    memory[17] ^= 0x01u;
    assert(lockstep_hash(memory, sizeof memory) != before);
}

static void test_hash_notices_two_words_swapped(void) {
    unsigned char memory[64];
    fill(memory, sizeof memory);
    const uint64_t before = lockstep_hash(memory, sizeof memory);
    unsigned char word[8];
    memcpy(word, memory, sizeof word);
    memcpy(memory, memory + 8, sizeof word);
    memcpy(memory + 8, word, sizeof word);
    assert(lockstep_hash(memory, sizeof memory) != before);
}

static void test_hash_of_the_same_bytes_is_the_same(void) {
    unsigned char left[128], right[128];
    fill(left, sizeof left);
    fill(right, sizeof right);
    assert(lockstep_hash(left, sizeof left) == lockstep_hash(right, sizeof right));
}

static void test_ranges_are_parsed_from_the_spec_both_products_are_given(void) {
    LockstepRanges skip;
    assert(lockstep_parse_ranges(&skip, "78-7B,78000-7FFFF") == 2);
    assert(skip.start[0] == 0x78u && skip.end[0] == 0x7Cu);
    assert(skip.start[1] == 0x78000u && skip.end[1] == 0x80000u);
    assert(lockstep_parse_ranges(&skip, "") == 0);
    assert(lockstep_parse_ranges(&skip, NULL) == 0);
}

static void test_an_excluded_range_is_not_hashed(void) {
    unsigned char memory[MEMORY_BYTES];
    uint64_t before[REGIONS], after[REGIONS];
    LockstepRanges skip;
    /* 8192 bytes in 8 regions: region 3 spans $C00..$FFF. */
    lockstep_parse_ranges(&skip, "C00-CFF");
    fill(memory, sizeof memory);
    lockstep_digest_regions_excluding(memory, sizeof memory, before, REGIONS, &skip);
    memory[0xC80] ^= 0xFFu; /* inside the excluded range */
    lockstep_digest_regions_excluding(memory, sizeof memory, after, REGIONS, &skip);
    for (size_t index = 0; index < REGIONS; index++)
        assert(before[index] == after[index]);
    memory[0xD00] ^= 0xFFu; /* one byte past it, in the same region */
    lockstep_digest_regions_excluding(memory, sizeof memory, after, REGIONS, &skip);
    assert(before[3] != after[3]);
}

static void test_an_exclusion_spanning_a_whole_region_still_hashes_the_rest(void) {
    unsigned char memory[MEMORY_BYTES];
    uint64_t before[REGIONS], after[REGIONS];
    LockstepRanges skip;
    lockstep_parse_ranges(&skip, "1000-17FF"); /* the whole of regions 4 and 5 */
    fill(memory, sizeof memory);
    lockstep_digest_regions_excluding(memory, sizeof memory, before, REGIONS, &skip);
    memory[0x1400] ^= 0xFFu; /* inside region 5, which is entirely excluded */
    memory[0x1C00] ^= 0xFFu; /* region 7, which is not */
    lockstep_digest_regions_excluding(memory, sizeof memory, after, REGIONS, &skip);
    assert(before[5] == after[5]);
    assert(before[7] != after[7]);
}

static void test_a_change_moves_only_its_own_region(void) {
    unsigned char memory[MEMORY_BYTES];
    uint64_t before[REGIONS], after[REGIONS];
    fill(memory, sizeof memory);
    lockstep_digest_regions(memory, sizeof memory, before, REGIONS);
    /* Region 3 spans 3072..4095 for this geometry. */
    memory[3100] ^= 0x80u;
    lockstep_digest_regions(memory, sizeof memory, after, REGIONS);
    for (size_t index = 0; index < REGIONS; index++) {
        if (index == 3)
            assert(before[index] != after[index]);
        else
            assert(before[index] == after[index]);
    }
}

static void test_the_frame_line_is_the_one_the_driver_parses(void) {
    char line[512];
    LockstepFrame state;
    memset(&state, 0, sizeof state);
    state.frame = 42;
    state.cop1lc = 0x007BC8u;
    state.palette = 0x0000ABCDu;
    state.dmacon = 0x0200u;
    state.audio_dma = 0x3u;
    for (unsigned channel = 0; channel < 4; channel++) {
        state.audio_pointer[channel] = 0x060000u + channel;
        state.audio_period[channel] = 320u + channel;
        state.audio_volume[channel] = 60u + channel;
    }
    const uint64_t digests[3] = {1u, 2u, 3u};
    const int written = lockstep_format_frame(line, sizeof line, &state, digests, 3u);
    assert(written > 0 && written < (int)sizeof line);
    assert(strncmp(line, "lockstep frame=42 ", 18) == 0);
    assert(strstr(line, "cop1lc=007BC8") != NULL);
    assert(strstr(line, "pal=0000ABCD") != NULL);
    assert(strstr(line, "alc=060000,060001,060002,060003") != NULL);
    assert(strstr(line, "aper=320,321,322,323") != NULL);
    assert(strstr(line, "avol=60,61,62,63") != NULL);
    assert(strstr(line, "adma=200") != NULL);
    /* The audio enables are reported apart from the rest of DMACON: the
     * reference cannot run the handler that writes them (see
     * STRUCTURAL_DIFFERENCES in tools/lockstep.py), so folding them into the
     * compared value would stop the run on frame 8 of every boot. */
    assert(strstr(line, "aaud=3") != NULL);
    assert(strstr(line, " mem=0000000000000001,0000000000000002,0000000000000003") != NULL);
    assert(strchr(line, '\n') == NULL); /* the channel adds the newline, once */
}

static void test_a_line_too_long_for_the_buffer_fails_rather_than_truncating(void) {
    char line[32];
    LockstepFrame state;
    memset(&state, 0, sizeof state);
    const uint64_t digests[4] = {1u, 2u, 3u, 4u};
    assert(lockstep_format_frame(line, sizeof line, &state, digests, 4u) < 0);
}

static void test_the_hello_line_states_the_geometry(void) {
    char line[128];
    const int written = lockstep_format_hello(line, sizeof line, 256u, 8u * 1024u * 1024u);
    assert(written > 0);
    assert(strcmp(line, "lockstep hello version=2 regions=256 bytes=8388608") == 0);
}

/* ── The channel: the same open/exchange both products speak ─────────────── */

static void write_command(int fd, const char *command) {
    const size_t length = strlen(command);
    assert(write(fd, command, length) == (ssize_t)length);
}

static size_t read_available(int fd, char *out, size_t cap) {
    const ssize_t got = read(fd, out, cap - 1u);
    assert(got >= 0);
    out[got] = 0;
    return (size_t)got;
}

/* Build a channel over two pipes and hand back the ends the test drives. */
static void open_channel(LockstepChannel *channel, int *to_test, int *from_test) {
    int to_product[2], from_product[2];
    assert(pipe(to_product) == 0);
    assert(pipe(from_product) == 0);
    char spec[64];
    snprintf(spec, sizeof spec, "%d:%d", to_product[0], from_product[1]);
    assert(lockstep_open(channel, spec, 4u, 4096u) == 1);
    *to_test = to_product[1];
    *from_test = from_product[0];
}

static void test_opening_the_channel_says_hello(void) {
    LockstepChannel channel;
    int to_product, from_product;
    open_channel(&channel, &to_product, &from_product);
    char said[256];
    read_available(from_product, said, sizeof said);
    assert(strcmp(said, "lockstep hello version=2 regions=4 bytes=4096\n") == 0);
    lockstep_close(&channel);
}

static void test_a_spec_that_is_not_two_fds_leaves_the_channel_off(void) {
    LockstepChannel channel;
    assert(lockstep_open(&channel, "", 4u, 4096u) == 0);
    assert(lockstep_open(&channel, "nonsense", 4u, 4096u) == 0);
}

static void test_the_exchange_sends_the_frame_and_returns_on_go(void) {
    LockstepChannel channel;
    int to_product, from_product;
    unsigned char memory[4096];
    char said[256];
    open_channel(&channel, &to_product, &from_product);
    read_available(from_product, said, sizeof said);
    fill(memory, sizeof memory);
    write_command(to_product, "go\n");
    lockstep_exchange(&channel, "lockstep frame=1 mem=00", memory, sizeof memory);
    read_available(from_product, said, sizeof said);
    assert(strcmp(said, "lockstep frame=1 mem=00\n") == 0);
    assert(channel.enabled == 1);
    lockstep_close(&channel);
}

static void test_a_dump_command_writes_the_memory_and_then_waits_for_go(void) {
    LockstepChannel channel;
    int to_product, from_product;
    unsigned char memory[4096];
    char said[256];
    const char *path = "build/verification/lockstep-dump.bin";
    open_channel(&channel, &to_product, &from_product);
    read_available(from_product, said, sizeof said);
    fill(memory, sizeof memory);
    memory[0x100] = 0x5Au;
    char command[256];
    snprintf(command, sizeof command, "dump %s\ngo\n", path);
    write_command(to_product, command);
    lockstep_exchange(&channel, "lockstep frame=2 mem=00", memory, sizeof memory);
    read_available(from_product, said, sizeof said);
    assert(strstr(said, "lockstep dumped bytes=4096") != NULL);
    FILE *written = fopen(path, "rb");
    assert(written != NULL);
    unsigned char image[4096];
    assert(fread(image, 1u, sizeof image, written) == sizeof image);
    fclose(written);
    assert(memcmp(image, memory, sizeof image) == 0);
    lockstep_close(&channel);
}

static void test_a_closed_channel_stops_the_product_reporting_rather_than_blocking(void) {
    LockstepChannel channel;
    int to_product, from_product;
    unsigned char memory[64];
    open_channel(&channel, &to_product, &from_product);
    close(to_product); /* the driver went away */
    lockstep_exchange(&channel, "lockstep frame=3 mem=00", memory, sizeof memory);
    assert(channel.enabled == 0);
    close(from_product);
}

int main(void) {
    test_hash_notices_a_single_changed_byte();
    test_hash_notices_two_words_swapped();
    test_hash_of_the_same_bytes_is_the_same();
    test_ranges_are_parsed_from_the_spec_both_products_are_given();
    test_an_excluded_range_is_not_hashed();
    test_an_exclusion_spanning_a_whole_region_still_hashes_the_rest();
    test_a_change_moves_only_its_own_region();
    test_the_frame_line_is_the_one_the_driver_parses();
    test_a_line_too_long_for_the_buffer_fails_rather_than_truncating();
    test_the_hello_line_states_the_geometry();
    test_opening_the_channel_says_hello();
    test_a_spec_that_is_not_two_fds_leaves_the_channel_off();
    test_the_exchange_sends_the_frame_and_returns_on_go();
    test_a_dump_command_writes_the_memory_and_then_waits_for_go();
    test_a_closed_channel_stops_the_product_reporting_rather_than_blocking();
    return 0;
}
