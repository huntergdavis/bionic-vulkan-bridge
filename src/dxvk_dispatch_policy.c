#include <bvb/dxvk_dispatch_policy.h>

#include <string.h>

#define BVB_DXVK_DISPATCH_POLICY_ENTRY(                                      \
    entry_name, canonical, entry_scope, count, entry_support)                 \
    {                                                                         \
        .name = entry_name, .canonical_name = canonical,                      \
        .lookup_count = count, .scope = entry_scope,                          \
        .support = entry_support,                                             \
    },
static const struct bvb_dxvk_dispatch_policy_entry policy_entries[] = {
#include "bvb_dxvk_dispatch_policy.inc"
};
#undef BVB_DXVK_DISPATCH_POLICY_ENTRY

BVB_DXVK_POLICY_EXPORT size_t bvb_dxvk_dispatch_policy_count(void) {
    return sizeof(policy_entries) / sizeof(policy_entries[0]);
}

BVB_DXVK_POLICY_EXPORT const struct bvb_dxvk_dispatch_policy_entry *
bvb_dxvk_dispatch_policy_at(size_t index) {
    return index < bvb_dxvk_dispatch_policy_count() ? &policy_entries[index]
                                                    : NULL;
}

BVB_DXVK_POLICY_EXPORT const struct bvb_dxvk_dispatch_policy_entry *
bvb_dxvk_dispatch_policy_lookup(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    size_t lower = 0U;
    size_t upper = bvb_dxvk_dispatch_policy_count();
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2U;
        const int comparison = strcmp(name, policy_entries[middle].name);
        if (comparison == 0) {
            return &policy_entries[middle];
        }
        if (comparison < 0) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return NULL;
}
