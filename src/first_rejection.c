#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define VK_NO_PROTOTYPES

#include <bvb/first_rejection.h>
#include <bvb/global_dispatch.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct bvb_first_rejection_state {
    pthread_mutex_t mutex;
    bool enabled;
    struct bvb_first_rejection_snapshot snapshot;
};

static pthread_once_t bvb_first_rejection_once = PTHREAD_ONCE_INIT;
enum bvb_first_rejection_winner_state {
    BVB_FIRST_WINNER_OPEN = 0,
    BVB_FIRST_WINNER_EMITTING = 1,
    BVB_FIRST_WINNER_EMITTED = 2,
};
static _Atomic unsigned bvb_first_winner_state;
static struct bvb_first_rejection_state bvb_first_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

_Static_assert(BVB_FIRST_REJECTION_RECORD_MAX <= PIPE_BUF,
               "first-rejection record must fit in one atomic pipe write");
enum { BVB_FIRST_REJECTION_FIXED_FIELDS_MAX = 512 };
_Static_assert(
    BVB_FIRST_REJECTION_FIXED_FIELDS_MAX +
            sizeof(((struct bvb_first_rejection_snapshot *)0)->category) - 1U +
            sizeof(((struct bvb_first_rejection_snapshot *)0)->entry) - 1U +
            sizeof(((struct bvb_first_rejection_snapshot *)0)->canonical) - 1U +
            sizeof(((struct bvb_first_rejection_snapshot *)0)->scope) - 1U +
            sizeof(((struct bvb_first_rejection_snapshot *)0)->reason) - 1U +
            sizeof(((struct bvb_first_rejection_snapshot *)0)->shape) - 1U <=
        BVB_FIRST_REJECTION_RECORD_MAX,
    "first-rejection fields must fit the single-write record");

static void initialize_first_rejection(void) {
    const char *value = getenv("BVB_FIRST_REJECTION_DIAGNOSTIC");
    bvb_first_state.enabled = value != NULL && strcmp(value, "1") == 0;
    bvb_first_state.snapshot.enabled = bvb_first_state.enabled;
}

bool bvb_first_rejection_enabled(void) {
    (void)pthread_once(&bvb_first_rejection_once, initialize_first_rejection);
    return bvb_first_state.enabled;
}

static void copy_text(char *output, size_t capacity, const char *value) {
    if (output == NULL || capacity == 0U) return;
    (void)snprintf(output, capacity, "%s", value == NULL ? "none" : value);
}

void bvb_first_rejection_note_executable_invocation(void) {
    if (!bvb_first_rejection_enabled() ||
        atomic_load_explicit(&bvb_first_winner_state,
                             memory_order_acquire) != BVB_FIRST_WINNER_OPEN ||
        pthread_mutex_lock(&bvb_first_state.mutex) != 0) return;
    if (atomic_load_explicit(&bvb_first_winner_state,
                             memory_order_relaxed) == BVB_FIRST_WINNER_OPEN)
        ++bvb_first_state.snapshot.executable_invocations;
    (void)pthread_mutex_unlock(&bvb_first_state.mutex);
}

static void emit_first_rejection(
    const struct bvb_first_rejection_snapshot *snapshot) {
    char record[BVB_FIRST_REJECTION_RECORD_MAX];
    const int length = snprintf(
        record, sizeof(record),
        "BVB_FIRST_REJECTION schema=1 category=%s entry=%s "
        "canonical=%s scope=%s reason=%s result=%d argc=%u "
        "pointer_mask=0x%016llx shape=%s executable_invocations=%llu "
        "stub_queries=%llu stub_invocations=%llu null_queries=%llu "
        "implemented_rejections=%llu command_poisons=%llu "
        "command_end_failures=%llu command_buffer=%llu "
        "command_sequence=%llu end_poison=%u\n",
        snapshot->category, snapshot->entry, snapshot->canonical,
        snapshot->scope, snapshot->reason, snapshot->result,
        snapshot->argument_count,
        (unsigned long long)snapshot->pointer_mask, snapshot->shape,
        (unsigned long long)snapshot->executable_invocations,
        (unsigned long long)snapshot->stub_queries,
        (unsigned long long)snapshot->stub_invocations,
        (unsigned long long)snapshot->null_queries,
        (unsigned long long)snapshot->implemented_rejections,
        (unsigned long long)snapshot->command_poisons,
        (unsigned long long)snapshot->command_end_failures,
        (unsigned long long)snapshot->command_buffer_id,
        (unsigned long long)snapshot->command_sequence,
        snapshot->end_poison ? 1U : 0U);
    if (length <= 0 || (size_t)length >= sizeof(record)) return;
    /* No retry: one winner produces at most one stderr syscall. */
    const ssize_t written = write(STDERR_FILENO, record, (size_t)length);
    (void)written;
}

static void record_first_rejection(
    const char *category, const char *entry, const char *canonical,
    const char *scope, const char *reason, VkResult result,
    uint32_t argument_count, uint64_t pointer_mask, const char *shape,
    uint64_t command_buffer_id, uint64_t command_sequence, bool end_poison,
    bool stub_invocation, bool command_poison) {
    if (!bvb_first_rejection_enabled()) return;
    unsigned winner_state = atomic_load_explicit(
        &bvb_first_winner_state, memory_order_acquire);
    if (winner_state != BVB_FIRST_WINNER_OPEN) {
        while (winner_state == BVB_FIRST_WINNER_EMITTING)
            winner_state = atomic_load_explicit(
                &bvb_first_winner_state, memory_order_acquire);
        return;
    }
    if (pthread_mutex_lock(&bvb_first_state.mutex) != 0) return;
    winner_state = atomic_load_explicit(
        &bvb_first_winner_state, memory_order_relaxed);
    if (winner_state != BVB_FIRST_WINNER_OPEN) {
        (void)pthread_mutex_unlock(&bvb_first_state.mutex);
        while (winner_state == BVB_FIRST_WINNER_EMITTING)
            winner_state = atomic_load_explicit(
                &bvb_first_winner_state, memory_order_acquire);
        return;
    }
    struct bvb_first_rejection_snapshot *snapshot = &bvb_first_state.snapshot;
    if (stub_invocation) ++snapshot->stub_invocations;
    if (command_poison) {
        ++snapshot->command_poisons;
        ++snapshot->command_end_failures;
    }
    if (category != NULL &&
        (strcmp(category, "implemented_rejection") == 0 ||
         strcmp(category, "executable_target_missing") == 0))
        ++snapshot->implemented_rejections;
    snapshot->emitted = true;
    snapshot->result = result;
    snapshot->argument_count = argument_count;
    snapshot->pointer_mask = pointer_mask;
    snapshot->command_buffer_id = command_buffer_id;
    snapshot->command_sequence = command_sequence;
    snapshot->end_poison = end_poison;
    copy_text(snapshot->category, sizeof(snapshot->category), category);
    copy_text(snapshot->entry, sizeof(snapshot->entry), entry);
    copy_text(snapshot->canonical, sizeof(snapshot->canonical), canonical);
    copy_text(snapshot->scope, sizeof(snapshot->scope), scope);
    copy_text(snapshot->reason, sizeof(snapshot->reason), reason);
    copy_text(snapshot->shape, sizeof(snapshot->shape), shape);
    const struct bvb_first_rejection_snapshot winner = *snapshot;
    atomic_store_explicit(&bvb_first_winner_state,
                          BVB_FIRST_WINNER_EMITTING,
                          memory_order_release);
    (void)pthread_mutex_unlock(&bvb_first_state.mutex);
    emit_first_rejection(&winner);
    atomic_store_explicit(&bvb_first_winner_state,
                          BVB_FIRST_WINNER_EMITTED,
                          memory_order_release);
}

void bvb_first_rejection_record(
    const char *category, const char *entry, const char *canonical,
    const char *scope, const char *reason, VkResult result,
    uint32_t argument_count, uint64_t pointer_mask, const char *shape,
    uint64_t command_buffer_id, uint64_t command_sequence, bool end_poison) {
    record_first_rejection(
        category, entry, canonical, scope, reason, result, argument_count,
        pointer_mask, shape, command_buffer_id, command_sequence, end_poison,
        false, false);
}

void bvb_first_rejection_record_stub(
    const char *entry, const char *canonical, const char *scope,
    uint32_t argument_count, uint64_t pointer_mask, const char *shape) {
    record_first_rejection(
        "required_unimplemented", entry, canonical, scope,
        "diagnostic_stub_invoked", VK_ERROR_FEATURE_NOT_PRESENT,
        argument_count, pointer_mask, shape, 0U, 0U, false, true, false);
}

void bvb_first_rejection_record_command_poison(
    const char *entry, const char *reason, const char *shape, int status,
    uint64_t command_buffer_id, uint64_t command_sequence) {
    record_first_rejection(
        "command_poison", entry, entry, "device", reason,
        status == 0 ? VK_ERROR_INITIALIZATION_FAILED : (VkResult)status,
        0U, 0U, shape, command_buffer_id, command_sequence, true, false, true);
}

int bvb_first_rejection_snapshot(
    struct bvb_first_rejection_snapshot *snapshot) {
    if (snapshot == NULL) return -EINVAL;
    (void)bvb_first_rejection_enabled();
    if (pthread_mutex_lock(&bvb_first_state.mutex) != 0) return -EDEADLK;
    *snapshot = bvb_first_state.snapshot;
    (void)pthread_mutex_unlock(&bvb_first_state.mutex);
    return 0;
}

#include "bvb_first_rejection_dispatch.inc"

PFN_vkVoidFunction bvb_first_rejection_wrap(
    const char *name, enum bvb_dxvk_dispatch_scope scope,
    PFN_vkVoidFunction raw) {
    if (raw == NULL || name == NULL || !bvb_first_rejection_enabled())
        return raw;
    return bvb_first_generated_wrap(name, scope, raw);
}

PFN_vkVoidFunction bvb_first_rejection_required_stub(
    const char *name, enum bvb_dxvk_dispatch_scope scope) {
    if (name == NULL || !bvb_first_rejection_enabled()) return NULL;
    const struct bvb_dxvk_dispatch_policy_entry *policy =
        bvb_dxvk_dispatch_policy_lookup(name);
    PFN_vkVoidFunction result = NULL;
    if (policy != NULL && policy->scope == scope &&
        policy->support == BVB_DXVK_SUPPORT_REQUIRED_UNIMPLEMENTED)
        result = bvb_first_generated_required_stub(name, scope);
    if (atomic_load_explicit(&bvb_first_winner_state,
                             memory_order_acquire) != BVB_FIRST_WINNER_OPEN)
        return result;
    if (pthread_mutex_lock(&bvb_first_state.mutex) == 0) {
        if (atomic_load_explicit(&bvb_first_winner_state,
                                 memory_order_relaxed) ==
            BVB_FIRST_WINNER_OPEN) {
            if (result == NULL)
                ++bvb_first_state.snapshot.null_queries;
            else
                ++bvb_first_state.snapshot.stub_queries;
        }
        (void)pthread_mutex_unlock(&bvb_first_state.mutex);
    }
    return result;
}
