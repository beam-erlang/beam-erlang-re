/* SPDX-License-Identifier: Apache-2.0 */
#include <beam/beam.h>
#include <beam/host.h>
#include <beam/native_image.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "fixture.h"
#if RE_BENCH_CANDIDATE
#include "../builtin_bindings.h"
#endif

typedef size_t (*counter_fn)(void);
static counter_fn counter[6];
static const char *counter_names[6] = {
    "beam_re_test_allocations", "beam_re_test_resources", "beam_re_test_bytes",
    "beam_re_test_peak_bytes", "beam_re_test_total_allocations",
    "beam_re_test_total_allocated_bytes"
};

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value)) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static int call(beam_vm *vm, const char *function, const unsigned char *args,
                size_t size, beam_result *result)
{
    beam_status status = beam_vm_call(vm, "re_bench", function,
                                    (beam_bytes){args, size}, result);
    if (status == BEAM_OK && result->kind == BEAM_RETURN) return 1;
    fprintf(stderr, "benchmark %s failed: status=%d kind=%d ETF=", function, status, result->kind);
    for (size_t i = 0; i < result->value.size && i < 2000; ++i)
        fprintf(stderr, "%02x", result->value.data[i]);
    fputc('\n', stderr);
    return 0;
}

static int call_ok(beam_vm *vm, const char *function, const unsigned char *args, size_t size)
{
    beam_result result = {0};
    int success = call(vm, function, args, size, &result);
    if (success) {
        const unsigned char *p = result.value.data;
        success = result.value.size == 5 && p[0] == 131 && p[1] == 119 &&
                  p[2] == 2 && p[3] == 'o' && p[4] == 'k';
        if (!success) fprintf(stderr, "benchmark %s did not return ok\n", function);
    }
    beam_result_release(&result);
    return success;
}

static size_t atom(unsigned char *out, const char *value)
{
    size_t length = strlen(value);
    if (length > 255) abort();
    out[0] = 119; out[1] = (unsigned char)length;
    memcpy(out + 2, value, length);
    return length + 2;
}

static size_t integer(unsigned char *out, unsigned value)
{
    out[0] = 98;
    out[1] = (unsigned char)(value >> 24); out[2] = (unsigned char)(value >> 16);
    out[3] = (unsigned char)(value >> 8); out[4] = (unsigned char)value;
    return 5;
}

static size_t prepare_args(unsigned char *out, const char *workload, unsigned iterations)
{
    size_t n = 0;
    out[n++] = 131; out[n++] = 108;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 2;
    n += atom(out + n, workload); n += integer(out + n, iterations);
    out[n++] = 106;
    return n;
}

static size_t verify_args(unsigned char *out, int builtin)
{
    size_t n = 0;
    out[n++] = 131; out[n++] = 108;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    n += atom(out + n, builtin ? "true" : "false"); out[n++] = 106;
    return n;
}

static void print_counter_delta(size_t index, size_t before)
{
    if (counter[index]) printf("%zu", counter[index]() - before);
    else fputs("null", stdout);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "benchmark:%d: %s (%s)\n", __LINE__, #condition, error.message); \
    goto cleanup; } } while (0)

int main(int argc, char **argv)
{
    beam_system *system = NULL; beam_vm *vm = NULL;
    beam_composition *composition = NULL;
    beam_native_image *image = NULL;
    beam_native_owner owner = {.struct_size = sizeof(owner)};
    beam_nif_binding binding = {.struct_size = sizeof(binding)};
    beam_error error = {0};
    int status = 1, argument = 1;
    unsigned char args[512]; size_t size;
    const unsigned char nil[] = {131,106};
    beam_result latency = {0};
    CHECK(argc == (RE_BENCH_CANDIDATE ? 5 : 4));
#if RE_BENCH_CANDIDATE
    CHECK(beam_host_open_nif(argv[argument++], "re", &image, &binding, &owner, &error) == BEAM_OK);
    for (size_t i = 0; i < 6; ++i) {
        void *symbol = NULL;
        if (beam_native_image_symbol(image, counter_names[i], &symbol) == BEAM_OK)
            memcpy(&counter[i], &symbol, sizeof(counter[i]));
    }
#endif
    const char *workload = argv[argument++];
    char *end = NULL;
    unsigned long count = strtoul(argv[argument++], &end, 10);
    CHECK(end && !*end && count > 0 && count <= 10000000);
    unsigned long warmups = strtoul(argv[argument++], &end, 10);
    CHECK(end && !*end && warmups <= 20);
    beam_composition_options spec = {.struct_size = sizeof(spec),
        .module_count = fixture_module_count, .modules = fixture_modules};
#if RE_BENCH_CANDIDATE
    spec.builtin_binding_count = sizeof re_test_canonical_bindings / sizeof *re_test_canonical_bindings;
    spec.builtin_bindings = re_test_canonical_bindings;
    spec.binding_count = 1; spec.bindings = &binding;
    spec.native_owner_count = 1; spec.native_owners = &owner;
#endif
    CHECK(beam_composition_prepare(&spec, &composition, &error) == BEAM_OK);
    CHECK(beam_init(&system, &error) == BEAM_OK);
    beam_vm_options options; beam_vm_options_init(&options);
    options.composition = composition;
    options.schedulers = options.schedulers_online = 1;
    options.dirty_cpu_schedulers = options.dirty_cpu_schedulers_online = 1;
    options.dirty_io_schedulers = 1;
    CHECK(beam_vm_init(system, &options, &vm, &error) == BEAM_OK);
    CHECK(beam_vm_start(vm, &error) == BEAM_OK);
#if RE_BENCH_CANDIDATE
    CHECK(beam_vm_bind_nif(vm, "re", &error) == BEAM_OK);
#endif
    size = verify_args(args, !RE_BENCH_CANDIDATE);
    CHECK(call_ok(vm, "verify", args, size));
    size = prepare_args(args, workload, (unsigned)count);
    CHECK(call_ok(vm, "prepare", args, size));
    for (unsigned long i = 0; i < warmups; ++i) {
        CHECK(call_ok(vm, "sample", nil, sizeof(nil)));
        CHECK(call_ok(vm, "contended", nil, sizeof(nil)));
        CHECK(call_ok(vm, "collect", nil, sizeof(nil)));
    }
    size_t allocations_before = counter[4] ? counter[4]() : 0;
    size_t bytes_before = counter[5] ? counter[5]() : 0;
    uint64_t wall = clock_ns(CLOCK_MONOTONIC), cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    CHECK(call_ok(vm, "sample", nil, sizeof(nil)));
    cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    wall = clock_ns(CLOCK_MONOTONIC) - wall;
    printf("{\"sdk_build_id\":\"%s\",\"case\":\"%s\",\"iterations\":%lu,"
           "\"wall_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"backend_allocations\":",
           beam_build_id(), workload, count, wall, cpu);
    print_counter_delta(4, allocations_before);
    fputs(",\"backend_allocated_bytes\":", stdout); print_counter_delta(5, bytes_before);
    CHECK(call_ok(vm, "collect", nil, sizeof(nil)));
    fputs(",\"backend_bytes_after_collect\":", stdout); print_counter_delta(2, 0);
    fputs(",\"backend_allocations_after_collect\":", stdout); print_counter_delta(0, 0);
    fputs(",\"backend_resources_after_collect\":", stdout); print_counter_delta(1, 0);
    wall = clock_ns(CLOCK_MONOTONIC); cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    CHECK(call_ok(vm, "contended", nil, sizeof(nil)));
    cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    wall = clock_ns(CLOCK_MONOTONIC) - wall;
    printf(",\"contended_wall_ns\":%" PRIu64 ",\"contended_cpu_ns\":%" PRIu64 ",\"latency\":", wall, cpu);
    CHECK(call(vm, "latency_json", nil, sizeof(nil), &latency));
    CHECK(latency.value.size >= 6 && latency.value.data[0] == 131 && latency.value.data[1] == 109);
    const unsigned char *p = latency.value.data;
    size_t length = ((size_t)p[2] << 24) | ((size_t)p[3] << 16) | ((size_t)p[4] << 8) | p[5];
    CHECK(length == latency.value.size - 6 && length < 2000000);
    CHECK(fwrite(p + 6, 1, length, stdout) == length);
    beam_result_release(&latency);
    fputs(",\"peak_backend_bytes\":", stdout);
    if (counter[3]) printf("%zu", counter[3]()); else fputs("null", stdout);
    CHECK(call_ok(vm, "finish", nil, sizeof(nil)));
    CHECK(beam_vm_stop(vm, NULL) == BEAM_OK);
    CHECK(beam_vm_terminate(&vm, NULL) == BEAM_OK);
    fputs(",\"backend_bytes_after_termination\":", stdout);
    if (counter[2]) printf("%zu", counter[2]()); else fputs("null", stdout);
    fputs(",\"backend_allocations_after_termination\":", stdout);
    if (counter[0]) printf("%zu", counter[0]()); else fputs("null", stdout);
    fputs(",\"backend_resources_after_termination\":", stdout);
    if (counter[1]) printf("%zu", counter[1]()); else fputs("null", stdout);
    if (counter[0]) CHECK(counter[0]() == 0);
    if (counter[1]) CHECK(counter[1]() == 0);
    if (counter[2]) CHECK(counter[2]() == 0);
    struct rusage usage;
    CHECK(!getrusage(RUSAGE_SELF, &usage));
    printf(",\"process_peak_rss_bytes\":%" PRIu64 "}\n", (uint64_t)usage.ru_maxrss * 1024);
    status = 0;
cleanup:
    beam_result_release(&latency);
    if (vm) { beam_vm_stop(vm, NULL); beam_vm_terminate(&vm, NULL); }
    if (system) beam_terminate(&system);
    if (composition) beam_composition_release(&composition);
    if (image) beam_native_image_release(&image);
    return status;
}
