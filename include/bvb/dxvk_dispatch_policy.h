#ifndef BVB_DXVK_DISPATCH_POLICY_H
#define BVB_DXVK_DISPATCH_POLICY_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define BVB_DXVK_POLICY_EXPORT __attribute__((visibility("default")))
#else
#define BVB_DXVK_POLICY_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum bvb_dxvk_dispatch_scope {
    BVB_DXVK_SCOPE_GLOBAL = 1,
    BVB_DXVK_SCOPE_INSTANCE = 2,
    BVB_DXVK_SCOPE_DEVICE = 3,
    BVB_DXVK_SCOPE_PRIVATE = 4,
};

enum bvb_dxvk_dispatch_support {
    BVB_DXVK_SUPPORT_PROBED_NULL = 1,
    BVB_DXVK_SUPPORT_REQUIRED_UNIMPLEMENTED = 2,
    BVB_DXVK_SUPPORT_EXECUTABLE = 3,
};

struct bvb_dxvk_dispatch_policy_entry {
    const char *name;
    const char *canonical_name;
    uint32_t lookup_count;
    enum bvb_dxvk_dispatch_scope scope;
    enum bvb_dxvk_dispatch_support support;
};

BVB_DXVK_POLICY_EXPORT size_t bvb_dxvk_dispatch_policy_count(void);
BVB_DXVK_POLICY_EXPORT const struct bvb_dxvk_dispatch_policy_entry *
bvb_dxvk_dispatch_policy_at(size_t index);
BVB_DXVK_POLICY_EXPORT const struct bvb_dxvk_dispatch_policy_entry *
bvb_dxvk_dispatch_policy_lookup(const char *name);

#ifdef __cplusplus
}
#endif

#endif
