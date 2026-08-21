#ifndef BVB_FIRST_REJECTION_H
#define BVB_FIRST_REJECTION_H

#include <bvb/dxvk_dispatch_policy.h>

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#if defined(__GNUC__) || defined(__clang__)
#define BVB_FIRST_REJECTION_EXPORT __attribute__((visibility("default")))
#else
#define BVB_FIRST_REJECTION_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BVB_FIRST_REJECTION_TEXT_MAX = 385,
    BVB_FIRST_REJECTION_RECORD_MAX = 1536,
    BVB_FIRST_REJECTION_VOID_EXIT_STATUS = 86,
};

struct bvb_first_rejection_snapshot {
    uint64_t executable_invocations;
    uint64_t stub_queries;
    uint64_t stub_invocations;
    uint64_t null_queries;
    uint64_t implemented_rejections;
    uint64_t command_poisons;
    uint64_t command_end_failures;
    uint64_t command_buffer_id;
    uint64_t command_sequence;
    uint64_t pointer_mask;
    int32_t result;
    uint32_t argument_count;
    bool enabled;
    bool emitted;
    bool end_poison;
    char category[32];
    char entry[96];
    char canonical[96];
    char scope[16];
    char reason[64];
    char shape[BVB_FIRST_REJECTION_TEXT_MAX];
};

BVB_FIRST_REJECTION_EXPORT bool bvb_first_rejection_enabled(void);
BVB_FIRST_REJECTION_EXPORT PFN_vkVoidFunction
bvb_first_rejection_wrap(
    const char *name, enum bvb_dxvk_dispatch_scope scope,
    PFN_vkVoidFunction raw);
BVB_FIRST_REJECTION_EXPORT PFN_vkVoidFunction
bvb_first_rejection_required_stub(
    const char *name, enum bvb_dxvk_dispatch_scope scope);
BVB_FIRST_REJECTION_EXPORT void
bvb_first_rejection_note_executable_invocation(void);
BVB_FIRST_REJECTION_EXPORT void bvb_first_rejection_record(
    const char *category, const char *entry, const char *canonical,
    const char *scope, const char *reason, VkResult result,
    uint32_t argument_count, uint64_t pointer_mask, const char *shape,
    uint64_t command_buffer_id, uint64_t command_sequence, bool end_poison);
BVB_FIRST_REJECTION_EXPORT void bvb_first_rejection_record_stub(
    const char *entry, const char *canonical, const char *scope,
    uint32_t argument_count, uint64_t pointer_mask, const char *shape);
BVB_FIRST_REJECTION_EXPORT void bvb_first_rejection_record_command_poison(
    const char *entry, const char *reason, const char *shape, int status,
    uint64_t command_buffer_id, uint64_t command_sequence);
BVB_FIRST_REJECTION_EXPORT int bvb_first_rejection_snapshot(
    struct bvb_first_rejection_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
