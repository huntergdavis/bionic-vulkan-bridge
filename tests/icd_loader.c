#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    uint32_t api_version = 0U;
    if (vkEnumerateInstanceVersion(&api_version) != VK_SUCCESS ||
        api_version < VK_API_VERSION_1_0) {
        fputs("Vulkan loader version query failed\n", stderr);
        return 1;
    }
    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bvb-e048-icd-loader",
        .applicationVersion = 1U,
        .pEngineName = "bvb",
        .engineVersion = 1U,
        .apiVersion = VK_API_VERSION_1_1,
    };
    uint32_t instance_extension_count = 0U;
    VkResult result = vkEnumerateInstanceExtensionProperties(
        NULL, &instance_extension_count, NULL);
    if (result != VK_SUCCESS || instance_extension_count == 0U ||
        instance_extension_count > 64U) {
        fprintf(stderr, "instance-extension count failed: %d count=%u\n",
                (int)result, instance_extension_count);
        return 1;
    }
    VkExtensionProperties instance_extensions[64];
    memset(instance_extensions, 0, sizeof(instance_extensions));
    result = vkEnumerateInstanceExtensionProperties(
        NULL, &instance_extension_count, instance_extensions);
    int properties2_advertised = 0;
    if (result == VK_SUCCESS) {
        for (uint32_t index = 0U; index < instance_extension_count; ++index) {
            properties2_advertised |= strcmp(
                instance_extensions[index].extensionName,
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0;
        }
    }
    if (result != VK_SUCCESS || properties2_advertised == 0) {
        fputs("required instance extension unavailable\n", stderr);
        return 1;
    }
    const char *enabled_instance_extension =
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = 1U,
        .ppEnabledExtensionNames = &enabled_instance_extension,
    };
    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        fprintf(stderr, "Vulkan loader instance creation failed: %d\n",
                (int)result);
        return 1;
    }
    uint32_t physical_count = 0U;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
    if (result != VK_SUCCESS || physical_count != 1U) {
        fprintf(stderr, "physical-device count failed: %d count=%u\n",
                (int)result, physical_count);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    result = vkEnumeratePhysicalDevices(
        instance, &physical_count, &physical_device);
    if (result != VK_SUCCESS || physical_count != 1U ||
        physical_device == VK_NULL_HANDLE) {
        fprintf(stderr, "physical-device enumeration failed: %d count=%u\n",
                (int)result, physical_count);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDeviceProperties properties;
    memset(&properties, 0, sizeof(properties));
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    if (strcmp(properties.deviceName, "Adreno (TM) 730") != 0) {
        fprintf(stderr, "unexpected Vulkan device: %s\n",
                properties.deviceName);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    };
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    vkGetPhysicalDeviceFeatures2(physical_device, &features2);
    VkPhysicalDeviceMemoryProperties2 memory2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
    };
    vkGetPhysicalDeviceMemoryProperties2(physical_device, &memory2);
    if (strcmp(properties2.properties.deviceName, properties.deviceName) != 0 ||
        features2.features.samplerAnisotropy != VK_TRUE ||
        memory2.memoryProperties.memoryTypeCount == 0U ||
        memory2.memoryProperties.memoryHeapCount == 0U) {
        fputs("Vulkan 1.1 physical-device discovery failed\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    VkFormatProperties format_properties;
    memset(&format_properties, 0, sizeof(format_properties));
    vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
    VkFormatProperties2 format_properties2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
    };
    vkGetPhysicalDeviceFormatProperties2(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM, &format_properties2);
    if ((format_properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U ||
        format_properties2.formatProperties.optimalTilingFeatures !=
            format_properties.optimalTilingFeatures) {
        fputs("RGBA8 optimal tiling lacks sampled-image support\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkImageFormatProperties image_properties;
    memset(&image_properties, 0, sizeof(image_properties));
    result = vkGetPhysicalDeviceImageFormatProperties(
        physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0U,
        &image_properties);
    if (result != VK_SUCCESS || image_properties.maxExtent.width < 2800U ||
        image_properties.maxExtent.height < 1752U) {
        fprintf(stderr,
                "RGBA8 image-format query failed: %d max_extent=%ux%u\n",
                (int)result, image_properties.maxExtent.width,
                image_properties.maxExtent.height);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    uint32_t queue_family_count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_family_count, NULL);
    if (queue_family_count == 0U || queue_family_count > 16U) {
        fprintf(stderr, "invalid queue-family count: %u\n",
                queue_family_count);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkQueueFamilyProperties queue_families[16];
    memset(queue_families, 0, sizeof(queue_families));
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_family_count, queue_families);
    uint32_t queue_family_index = UINT32_MAX;
    for (uint32_t index = 0U; index < queue_family_count; ++index) {
        if (queue_families[index].queueCount != 0U &&
            (queue_families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
            queue_family_index = index;
            break;
        }
    }
    if (queue_family_index == UINT32_MAX) {
        fputs("no graphics queue family available\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    uint32_t device_extension_count = 0U;
    result = vkEnumerateDeviceExtensionProperties(
        physical_device, NULL, &device_extension_count, NULL);
    if (result != VK_SUCCESS || device_extension_count == 0U ||
        device_extension_count > 1024U) {
        fprintf(stderr, "device-extension count failed: %d count=%u\n",
                (int)result, device_extension_count);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkExtensionProperties *device_extensions = calloc(
        device_extension_count, sizeof(*device_extensions));
    if (device_extensions == NULL) {
        fputs("device-extension allocation failed\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    result = vkEnumerateDeviceExtensionProperties(
        physical_device, NULL, &device_extension_count, device_extensions);
    int swapchain_advertised = 0;
    if (result == VK_SUCCESS) {
        for (uint32_t index = 0U; index < device_extension_count; ++index) {
            swapchain_advertised |= strcmp(
                device_extensions[index].extensionName,
                VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        }
    }
    free(device_extensions);
    if (result != VK_SUCCESS || swapchain_advertised == 0) {
        fprintf(stderr, "required device extension unavailable: result=%d\n",
                (int)result);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_index,
        .queueCount = 1U,
        .pQueuePriorities = &queue_priority,
    };
    const VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1U,
        .pQueueCreateInfos = &queue_create_info,
        .enabledExtensionCount = 1U,
        .ppEnabledExtensionNames =
            (const char *const[]){VK_KHR_SWAPCHAIN_EXTENSION_NAME},
    };
    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(
        physical_device, &device_create_info, NULL, &device);
    if (result != VK_SUCCESS || device == VK_NULL_HANDLE) {
        fprintf(stderr, "logical-device creation failed: %d\n", (int)result);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queue_family_index, 0U, &queue);
    if (queue == VK_NULL_HANDLE || vkQueueWaitIdle(queue) != VK_SUCCESS ||
        vkDeviceWaitIdle(device) != VK_SUCCESS) {
        fputs("standard-loader queue/device idle path failed\n", stderr);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("PASS: Vulkan loader selected BVB ICD api=%u instance_extension=%s device=%s vendor=%u device_id=%u rgba8_optimal=%u max_extent=%ux%ux%u max_resource=%llu queue_family=%u enabled_extension=%s features2_anisotropy=%u memory2_types=%u memory2_heaps=%u device_idle=pass\n",
           api_version, enabled_instance_extension, properties.deviceName,
           properties.vendorID,
           properties.deviceID, format_properties.optimalTilingFeatures,
           image_properties.maxExtent.width,
           image_properties.maxExtent.height,
           image_properties.maxExtent.depth,
           (unsigned long long)image_properties.maxResourceSize,
           queue_family_index, VK_KHR_SWAPCHAIN_EXTENSION_NAME,
           features2.features.samplerAnisotropy,
           memory2.memoryProperties.memoryTypeCount,
           memory2.memoryProperties.memoryHeapCount);
    return 0;
}
