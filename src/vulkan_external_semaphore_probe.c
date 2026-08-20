#define VK_NO_PROTOTYPES

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PFN_vkVoidFunction loader_symbol(void *loader, const char *name) {
    void *symbol = dlsym(loader, name);
    PFN_vkVoidFunction function = NULL;
    if (symbol != NULL) memcpy(&function, &symbol, sizeof(function));
    return function;
}

static void print_json_string(const char *value) {
    (void)putchar('"');
    for (const unsigned char *current = (const unsigned char *)value;
         *current != '\0'; ++current) {
        if (*current == '"' || *current == '\\') (void)putchar('\\');
        if (*current >= 0x20U) (void)putchar(*current);
    }
    (void)putchar('"');
}

int main(int argc, char **argv) {
    const char *loader_path = "/system/lib64/libvulkan.so";
    if (argc == 3 && strcmp(argv[1], "--loader") == 0 && argv[2][0] == '/') {
        loader_path = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--loader ABSOLUTE_PATH]\n", argv[0]);
        return 2;
    }
    void *loader = dlopen(loader_path, RTLD_NOW | RTLD_LOCAL);
    if (loader == NULL) {
        fprintf(stderr, "could not load %s: %s\n", loader_path, dlerror());
        return 3;
    }
    PFN_vkGetInstanceProcAddr get_instance_proc_addr =
        (PFN_vkGetInstanceProcAddr)loader_symbol(loader,
                                                  "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = get_instance_proc_addr == NULL
        ? NULL
        : (PFN_vkCreateInstance)get_instance_proc_addr(
              VK_NULL_HANDLE, "vkCreateInstance");
    if (create_instance == NULL) {
        fprintf(stderr, "loader has no vkCreateInstance\n");
        (void)dlclose(loader);
        return 3;
    }
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-external-semaphore-probe",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const char *extension =
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME;
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 1U,
        .ppEnabledExtensionNames = &extension,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = create_instance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", (int)result);
        (void)dlclose(loader);
        return 4;
    }
#define LOAD_INSTANCE(name)                                                     \
    PFN_##name name = (PFN_##name)get_instance_proc_addr(instance, #name)
    LOAD_INSTANCE(vkDestroyInstance);
    LOAD_INSTANCE(vkEnumeratePhysicalDevices);
    LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE(vkGetPhysicalDeviceExternalSemaphorePropertiesKHR);
#undef LOAD_INSTANCE
    if (vkGetPhysicalDeviceExternalSemaphorePropertiesKHR == NULL) {
        vkGetPhysicalDeviceExternalSemaphorePropertiesKHR =
            (PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR)
                get_instance_proc_addr(
                    instance,
                    "vkGetPhysicalDeviceExternalSemaphoreProperties");
    }
    if (vkDestroyInstance == NULL || vkEnumeratePhysicalDevices == NULL ||
        vkGetPhysicalDeviceProperties == NULL ||
        vkGetPhysicalDeviceExternalSemaphorePropertiesKHR == NULL) {
        fprintf(stderr, "loader is missing external-semaphore queries\n");
        if (vkDestroyInstance != NULL) vkDestroyInstance(instance, NULL);
        (void)dlclose(loader);
        return 4;
    }
    uint32_t device_count = 0U;
    result = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    if (result != VK_SUCCESS || device_count == 0U || device_count > 16U) {
        fprintf(stderr, "physical-device enumeration failed: %d count=%u\n",
                (int)result, device_count);
        vkDestroyInstance(instance, NULL);
        (void)dlclose(loader);
        return 4;
    }
    VkPhysicalDevice devices[16];
    result = vkEnumeratePhysicalDevices(instance, &device_count, devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        fprintf(stderr, "physical-device list failed: %d\n", (int)result);
        vkDestroyInstance(instance, NULL);
        (void)dlclose(loader);
        return 4;
    }
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(devices[0], &device_properties);
    const VkExternalSemaphoreHandleTypeFlagBits handle_types[] = {
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkExternalSemaphoreProperties properties[2];
    for (size_t index = 0U; index < 2U; ++index) {
        const VkPhysicalDeviceExternalSemaphoreInfo query = {
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .handleType = handle_types[index],
        };
        properties[index] = (VkExternalSemaphoreProperties){
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
        };
        vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(
            devices[0], &query, &properties[index]);
    }
    printf("{\"schema_version\":1,\"loader_path\":");
    print_json_string(loader_path);
    printf(",\"device_name\":");
    print_json_string(device_properties.deviceName);
    printf(",\"opaque_fd\":{\"features\":%u,\"compatible\":%u,"
           "\"export_from_imported\":%u},"
           "\"sync_fd\":{\"features\":%u,\"compatible\":%u,"
           "\"export_from_imported\":%u}}\n",
           properties[0].externalSemaphoreFeatures,
           properties[0].compatibleHandleTypes,
           properties[0].exportFromImportedHandleTypes,
           properties[1].externalSemaphoreFeatures,
           properties[1].compatibleHandleTypes,
           properties[1].exportFromImportedHandleTypes);
    vkDestroyInstance(instance, NULL);
    (void)dlclose(loader);
    return 0;
}
