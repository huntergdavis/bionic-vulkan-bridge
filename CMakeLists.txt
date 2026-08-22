cmake_minimum_required(VERSION 3.20)

project(
  bionic-vulkan-bridge
  VERSION 0.1.0
  DESCRIPTION "A tested bridge toward Android-native Vulkan for glibc game stacks"
  LANGUAGES C
)

include(CTest)
include(FetchContent)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Vulkan-Headers v1.4.354, pinned to the peeled commit behind the signed tag.
FetchContent_Declare(
  VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG 01393c3df0e5285b54ee6527466513f9e614be94
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(VulkanHeaders)

function(bvb_enable_warnings target)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

add_library(bvb-vulkan-caps STATIC src/vulkan_caps.c)
target_include_directories(
  bvb-vulkan-caps
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(bvb-vulkan-caps PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
bvb_enable_warnings(bvb-vulkan-caps)

add_library(bvb-vulkan-selftest-core STATIC src/vulkan_selftest.c)
target_include_directories(
  bvb-vulkan-selftest-core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  bvb-vulkan-selftest-core
  PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS}
)
bvb_enable_warnings(bvb-vulkan-selftest-core)

set(
  bvb_vulkan_discovery_wire_include
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bvb_vulkan_discovery_wire.inc"
)
add_custom_command(
  OUTPUT "${bvb_vulkan_discovery_wire_include}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-vulkan-discovery-wire.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${bvb_vulkan_discovery_wire_include}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-vulkan-discovery-wire.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
  VERBATIM
)
add_library(
  bvb-vulkan-discovery STATIC
  src/vulkan_discovery.c
  "${bvb_vulkan_discovery_wire_include}"
)
target_include_directories(
  bvb-vulkan-discovery
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(bvb-vulkan-discovery PUBLIC Vulkan::Headers)
bvb_enable_warnings(bvb-vulkan-discovery)

add_library(bvb-vulkan-global STATIC src/vulkan_global.c)
target_include_directories(
  bvb-vulkan-global
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  bvb-vulkan-global
  PUBLIC bvb-vulkan-discovery
  PRIVATE bvb-command-batch Vulkan::Headers ${CMAKE_DL_LIBS}
)
bvb_enable_warnings(bvb-vulkan-global)

add_library(bvb-wsi-frame-ring STATIC src/wsi_frame_ring.c)
target_include_directories(
  bvb-wsi-frame-ring
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(bvb-wsi-frame-ring PUBLIC Threads::Threads)
bvb_enable_warnings(bvb-wsi-frame-ring)
target_link_libraries(bvb-vulkan-global PRIVATE bvb-wsi-frame-ring)

add_library(bvb-activity-frame-transport STATIC src/activity_frame_transport.c)
target_include_directories(
  bvb-activity-frame-transport
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(bvb-activity-frame-transport PRIVATE bvb-wire)
bvb_enable_warnings(bvb-activity-frame-transport)

add_executable(bvb-vulkan-probe src/vulkan_probe.c)
target_link_libraries(bvb-vulkan-probe PRIVATE bvb-vulkan-caps)
bvb_enable_warnings(bvb-vulkan-probe)

add_executable(bvb-vulkan-selftest src/vulkan_selftest_main.c)
target_link_libraries(bvb-vulkan-selftest PRIVATE bvb-vulkan-selftest-core)
bvb_enable_warnings(bvb-vulkan-selftest)

add_executable(
  bvb-vulkan-external-memory-selftest
  src/vulkan_external_memory_selftest_main.c
)
target_link_libraries(
  bvb-vulkan-external-memory-selftest PRIVATE bvb-vulkan-selftest-core
)
bvb_enable_warnings(bvb-vulkan-external-memory-selftest)

add_library(bvb-vulkan-surface-core STATIC src/vulkan_surface_probe.c)
target_include_directories(
  bvb-vulkan-surface-core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  bvb-vulkan-surface-core
  PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS}
)
bvb_enable_warnings(bvb-vulkan-surface-core)

add_executable(bvb-vulkan-surface-probe src/vulkan_surface_probe_main.c)
target_link_libraries(bvb-vulkan-surface-probe PRIVATE bvb-vulkan-surface-core)
bvb_enable_warnings(bvb-vulkan-surface-probe)

add_executable(bvb-vulkan-present-selftest src/vulkan_present_selftest.c)
target_link_libraries(
  bvb-vulkan-present-selftest
  PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS}
)
bvb_enable_warnings(bvb-vulkan-present-selftest)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  add_library(bvb-vulkan-resolve-trace SHARED src/vulkan_resolve_trace.c)
  target_link_libraries(
    bvb-vulkan-resolve-trace
    PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS}
  )
  set_target_properties(
    bvb-vulkan-resolve-trace
    PROPERTIES OUTPUT_NAME bvb-vulkan-resolve-trace
  )
  bvb_enable_warnings(bvb-vulkan-resolve-trace)
endif()

add_library(bvb-wire STATIC src/lifecycle.c src/protocol.c
  src/vulkan_descriptor_wire.c src/vulkan_pipeline_wire.c src/transport.c)
set_target_properties(bvb-wire PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  bvb-wire
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(bvb-wire PRIVATE Vulkan::Headers)
bvb_enable_warnings(bvb-wire)

add_library(bvb-frame-sync STATIC src/frame_sync.c)
set_target_properties(bvb-frame-sync PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  bvb-frame-sync
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
bvb_enable_warnings(bvb-frame-sync)

add_library(bvb-command-batch STATIC src/handle.c src/command_batch.c)
set_target_properties(bvb-command-batch PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  bvb-command-batch
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(bvb-command-batch PRIVATE bvb-wire)
bvb_enable_warnings(bvb-command-batch)
target_link_libraries(
  bvb-vulkan-selftest-core PUBLIC bvb-frame-sync PRIVATE bvb-command-batch
)

add_library(bvb-visible-batch STATIC src/visible_batch.c)
set_target_properties(bvb-visible-batch PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  bvb-visible-batch
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(bvb-visible-batch PRIVATE bvb-wire bvb-command-batch)
bvb_enable_warnings(bvb-visible-batch)

add_library(bvb-visible-ingress STATIC src/visible_ingress.c)
set_target_properties(bvb-visible-ingress PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  bvb-visible-ingress
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  bvb-visible-ingress PRIVATE bvb-visible-batch bvb-wire Threads::Threads
)
bvb_enable_warnings(bvb-visible-ingress)

set(
  bvb_triangle_dispatch_include
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bvb_triangle_dispatch.inc"
)
add_custom_command(
  OUTPUT "${bvb_triangle_dispatch_include}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-triangle-dispatch.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
    "${bvb_triangle_dispatch_include}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-triangle-dispatch.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
  VERBATIM
)
set(
  bvb_dxvk_dispatch_policy_include
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bvb_dxvk_dispatch_policy.inc"
)
set(
  bvb_dxvk_dispatch_policy_json
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bvb_dxvk_dispatch_policy.json"
)
add_custom_command(
  OUTPUT
    "${bvb_dxvk_dispatch_policy_include}"
    "${bvb_dxvk_dispatch_policy_json}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-dxvk-dispatch-policy.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
    "${bvb_triangle_dispatch_include}"
    "${bvb_dxvk_dispatch_policy_include}"
    "${bvb_dxvk_dispatch_policy_json}"
    --additional-executable
    "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
    --gate E069
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-dxvk-dispatch-policy.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
    "${bvb_triangle_dispatch_include}"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  VERBATIM
)
set(
  bvb_first_rejection_dispatch_include
  "${CMAKE_CURRENT_BINARY_DIR}/generated/bvb_first_rejection_dispatch.inc"
)
add_custom_command(
  OUTPUT "${bvb_first_rejection_dispatch_include}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-first-rejection-dispatch.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
    "${bvb_triangle_dispatch_include}"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
    "${bvb_first_rejection_dispatch_include}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-first-rejection-dispatch.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
    "${bvb_triangle_dispatch_include}"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  VERBATIM
)
add_library(
  bvb-vulkan-client-dispatch SHARED
  src/dxvk_dispatch_policy.c
  src/first_rejection.c
  src/global_dispatch.c
  src/triangle_dispatch.c
  "${bvb_first_rejection_dispatch_include}"
  "${bvb_dxvk_dispatch_policy_include}"
  "${bvb_triangle_dispatch_include}"
)
target_include_directories(
  bvb-vulkan-client-dispatch
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  bvb-vulkan-client-dispatch
  PRIVATE bvb-command-batch bvb-vulkan-discovery bvb-wire
          bvb-wsi-frame-ring Threads::Threads Vulkan::Headers
)
target_link_options(
  bvb-vulkan-client-dispatch PRIVATE "-Wl,-Bsymbolic-functions"
)
set_target_properties(
  bvb-vulkan-client-dispatch
  PROPERTIES C_VISIBILITY_PRESET hidden OUTPUT_NAME vulkan-bvb
)
bvb_enable_warnings(bvb-vulkan-client-dispatch)

add_library(bvb-triangle-batch-builder STATIC src/triangle_batch_builder.c)
target_include_directories(
  bvb-triangle-batch-builder
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  bvb-triangle-batch-builder
  PUBLIC bvb-vulkan-client-dispatch bvb-command-batch Vulkan::Headers
)
bvb_enable_warnings(bvb-triangle-batch-builder)

add_executable(bvb-bridge-service src/bridge_service.c)
target_link_libraries(
  bvb-bridge-service
  PRIVATE bvb-wire bvb-command-batch bvb-vulkan-caps bvb-vulkan-global
          bvb-vulkan-selftest-core bvb-activity-frame-transport Threads::Threads
)
bvb_enable_warnings(bvb-bridge-service)

add_executable(bvb-bridge-client src/bridge_client.c)
target_link_libraries(bvb-bridge-client PRIVATE bvb-wire bvb-command-batch)
bvb_enable_warnings(bvb-bridge-client)

add_executable(bvb-visible-triangle-client src/visible_triangle_client.c)
target_link_libraries(
  bvb-visible-triangle-client
  PRIVATE
    bvb-vulkan-client-dispatch
    bvb-triangle-batch-builder
    bvb-wire
)
bvb_enable_warnings(bvb-visible-triangle-client)

add_executable(bvb-shared-region-relay src/shared_region_relay.c)
target_link_libraries(
  bvb-shared-region-relay PRIVATE bvb-wire bvb-triangle-batch-builder
)
bvb_enable_warnings(bvb-shared-region-relay)

add_executable(bvb-external-memory-receiver src/external_memory_receiver.c)
target_link_libraries(
  bvb-external-memory-receiver PRIVATE bvb-wire bvb-vulkan-selftest-core
)
bvb_enable_warnings(bvb-external-memory-receiver)

add_executable(bvb-external-image-direct src/external_image_direct.c)
target_link_libraries(
  bvb-external-image-direct PRIVATE bvb-wire bvb-vulkan-selftest-core
)
bvb_enable_warnings(bvb-external-image-direct)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  add_executable(
    bvb-ahardwarebuffer-vulkan-probe src/ahardwarebuffer_vulkan_probe.c
  )
  target_link_libraries(
    bvb-ahardwarebuffer-vulkan-probe
    PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS}
  )
  target_compile_options(
    bvb-ahardwarebuffer-vulkan-probe PRIVATE --target=aarch64-linux-android29
  )
  target_link_options(
    bvb-ahardwarebuffer-vulkan-probe PRIVATE --target=aarch64-linux-android29
  )
  bvb_enable_warnings(bvb-ahardwarebuffer-vulkan-probe)
  install(TARGETS bvb-ahardwarebuffer-vulkan-probe RUNTIME DESTINATION bin)

  add_executable(bvb-external-image-binder src/external_image_binder.c)
  target_link_libraries(
    bvb-external-image-binder
    PRIVATE bvb-wire bvb-vulkan-selftest-core /system/lib64/libbinder_ndk.so
  )
  target_compile_options(
    bvb-external-image-binder PRIVATE --target=aarch64-linux-android29
  )
  target_link_options(
    bvb-external-image-binder PRIVATE --target=aarch64-linux-android29
  )
  bvb_enable_warnings(bvb-external-image-binder)
  install(TARGETS bvb-external-image-binder RUNTIME DESTINATION bin)
endif()

add_executable(
  bvb-vulkan-external-semaphore-probe
  src/vulkan_external_semaphore_probe.c
)
target_link_libraries(
  bvb-vulkan-external-semaphore-probe PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS}
)
bvb_enable_warnings(bvb-vulkan-external-semaphore-probe)

install(
  TARGETS bvb-vulkan-probe bvb-bridge-service bvb-bridge-client
          bvb-vulkan-selftest bvb-vulkan-surface-probe
          bvb-vulkan-external-memory-selftest
          bvb-vulkan-present-selftest bvb-vulkan-client-dispatch
          bvb-visible-triangle-client bvb-shared-region-relay
          bvb-external-memory-receiver bvb-vulkan-external-semaphore-probe
          bvb-external-image-direct
  RUNTIME DESTINATION bin
)

if(TARGET bvb-vulkan-resolve-trace)
  install(TARGETS bvb-vulkan-resolve-trace LIBRARY DESTINATION lib)
endif()

if(BUILD_TESTING)
  add_test(
    NAME private-turnip-maintenance56-patch-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_private_turnip_patch_contract.py"
      "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  add_executable(bvb-wsi-frame-ring-test tests/wsi_frame_ring.c)
  target_link_libraries(
    bvb-wsi-frame-ring-test PRIVATE bvb-wsi-frame-ring Threads::Threads
  )
  bvb_enable_warnings(bvb-wsi-frame-ring-test)
  add_test(NAME wsi-frame-ring-contract COMMAND bvb-wsi-frame-ring-test)

  add_executable(
    bvb-activity-frame-transport-test tests/activity_frame_transport.c
  )
  target_link_libraries(
    bvb-activity-frame-transport-test PRIVATE bvb-activity-frame-transport
  )
  bvb_enable_warnings(bvb-activity-frame-transport-test)
  add_test(
    NAME activity-frame-transport-wire-contract
    COMMAND bvb-activity-frame-transport-test
  )

  add_executable(bvb-frame-sync-test tests/frame_sync.c)
  target_link_libraries(bvb-frame-sync-test PRIVATE bvb-frame-sync)
  bvb_enable_warnings(bvb-frame-sync-test)
  add_test(NAME frame-sync-contract COMMAND bvb-frame-sync-test)

  add_library(bvb-fake-vulkan SHARED tests/fake_vulkan.c)
  target_link_libraries(bvb-fake-vulkan PRIVATE Vulkan::Headers)
  set_target_properties(
    bvb-fake-vulkan
    PROPERTIES
      C_VISIBILITY_PRESET hidden
      OUTPUT_NAME bvb-fake-vulkan
  )
  bvb_enable_warnings(bvb-fake-vulkan)

  add_executable(
    bvb-vulkan-swapchain-transport-test
    tests/vulkan_swapchain_transport.c
  )
  target_link_libraries(
    bvb-vulkan-swapchain-transport-test
    PRIVATE bvb-vulkan-global bvb-wsi-frame-ring Vulkan::Headers
  )
  bvb_enable_warnings(bvb-vulkan-swapchain-transport-test)
  add_test(
    NAME vulkan-swapchain-transport-contract
    COMMAND
      bvb-vulkan-swapchain-transport-test
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )
  add_test(
    NAME vulkan-swapchain-missing-entry-diagnostic-contract
    COMMAND
      bvb-vulkan-swapchain-transport-test
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )
  set_tests_properties(
    vulkan-swapchain-missing-entry-diagnostic-contract
    PROPERTIES ENVIRONMENT
      "BVB_FAKE_HIDE_GET_MEMORY_FD=1;BVB_EXPECT_MISSING_SWAPCHAIN_ENTRY_POINT=vkGetMemoryFdKHR"
  )
  add_test(
    NAME swapchain-service-transport-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_swapchain_service_transport.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )
  add_test(
    NAME frame-transport-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frame_transport_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/frame-transport-preparation-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/decisions/0005-persistent-game-frame-ring.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/wsi_frame_ring.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
  )
  add_test(
    NAME activity-frame-consumer-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_activity_frame_consumer_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/activity-frame-consumer-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/decisions/0005-persistent-game-frame-ring.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/activity_frame_transport.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/android/visible-host/java/io/github/huntergdavis/bvb/visiblehost/FrameTransportClient.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/android/visible-host/java/io/github/huntergdavis/bvb/visiblehost/SharedRegionReceiver.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/android/visible-host/java/io/github/huntergdavis/bvb/visiblehost/SharedRegionProvider.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/android/visible-host/native_main.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
  )
  add_test(
    NAME activity-frame-v40-runtime-gate-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_activity_frame_runtime_gate.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-activity-frame-import-v40-termux.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e074-activity-frame-v40-runtime-gate-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/activity-frame-v40-runtime-gate.md"
  )
  add_test(
    NAME virtual-swapchain-producer-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_virtual_swapchain_producer_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e060-virtual-swapchain-producer-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/decisions/0005-persistent-game-frame-ring.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
  )
  add_test(
    NAME device-buffer-requirements-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_device_buffer_requirements_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e066-dxvk-device-buffer-requirements-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e066-device-buffer-requirements-dispatch.txt"
  )
  add_test(
    NAME buffer-requirements-address-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_buffer_requirements_address_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e068-dxvk-global-buffer-address-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e068-buffer-address-dispatch.txt"
  )
  add_test(
    NAME init-image-commands-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_init_image_commands_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e069-dxvk-init-image-commands-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  )

  add_library(bvb-fake-media-ndk SHARED tests/fake_media_ndk.c)
  set_target_properties(
    bvb-fake-media-ndk
    PROPERTIES
      C_VISIBILITY_PRESET hidden
      OUTPUT_NAME bvb-fake-media-ndk
  )
  bvb_enable_warnings(bvb-fake-media-ndk)

  add_executable(bvb-protocol-test tests/protocol.c)
  target_link_libraries(bvb-protocol-test PRIVATE bvb-wire Vulkan::Headers)
  bvb_enable_warnings(bvb-protocol-test)

  add_test(NAME protocol-contract COMMAND bvb-protocol-test)

  add_executable(bvb-vulkan-discovery-test tests/vulkan_discovery.c)
  target_link_libraries(
    bvb-vulkan-discovery-test PRIVATE bvb-vulkan-discovery
  )
  bvb_enable_warnings(bvb-vulkan-discovery-test)
  add_test(NAME vulkan-discovery-wire-contract COMMAND bvb-vulkan-discovery-test)

  add_executable(bvb-transport-fd-test tests/transport_fd.c)
  target_link_libraries(bvb-transport-fd-test PRIVATE bvb-wire)
  bvb_enable_warnings(bvb-transport-fd-test)
  add_test(NAME transport-fd-contract COMMAND bvb-transport-fd-test)

  add_executable(bvb-command-batch-test tests/command_batch.c)
  target_link_libraries(bvb-command-batch-test PRIVATE bvb-command-batch)
  bvb_enable_warnings(bvb-command-batch-test)
  add_test(NAME command-batch-contract COMMAND bvb-command-batch-test)

  add_executable(bvb-visible-batch-test tests/visible_batch.c)
  target_link_libraries(
    bvb-visible-batch-test PRIVATE bvb-visible-batch bvb-command-batch
  )
  bvb_enable_warnings(bvb-visible-batch-test)
  add_test(NAME visible-batch-contract COMMAND bvb-visible-batch-test)

  add_executable(bvb-visible-ingress-host-test tests/visible_ingress_host.c)
  target_link_libraries(
    bvb-visible-ingress-host-test
    PRIVATE bvb-visible-ingress bvb-command-batch bvb-triangle-batch-builder
  )
  bvb_enable_warnings(bvb-visible-ingress-host-test)

  add_executable(bvb-triangle-dispatch-test tests/triangle_dispatch.c)
  target_link_libraries(
    bvb-triangle-dispatch-test
    PRIVATE bvb-vulkan-client-dispatch bvb-command-batch Vulkan::Headers
  )
  bvb_enable_warnings(bvb-triangle-dispatch-test)
  add_test(NAME triangle-dispatch-contract COMMAND bvb-triangle-dispatch-test)

  add_executable(bvb-global-dispatch-test tests/global_dispatch.c)
  target_link_libraries(
    bvb-global-dispatch-test
    PRIVATE bvb-vulkan-client-dispatch bvb-command-batch Vulkan::Headers
  )
  bvb_enable_warnings(bvb-global-dispatch-test)
  add_executable(bvb-first-rejection-test tests/first_rejection.c)
  target_link_libraries(
    bvb-first-rejection-test
    PRIVATE bvb-vulkan-client-dispatch bvb-command-batch Vulkan::Headers
  )
  bvb_enable_warnings(bvb-first-rejection-test)
  add_executable(
    bvb-first-rejection-global-test tests/first_rejection_global.c
  )
  target_link_libraries(
    bvb-first-rejection-global-test
    PRIVATE bvb-vulkan-client-dispatch bvb-command-batch Vulkan::Headers
  )
  bvb_enable_warnings(bvb-first-rejection-global-test)
  add_test(
    NAME first-real-rejection-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_first_rejection.py"
      "$<TARGET_FILE:bvb-first-rejection-test>"
  )
  add_test(
    NAME first-real-rejection-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_first_rejection_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e079a-first-rejection-corrective-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-first-rejection-dispatch.py"
      "${bvb_first_rejection_dispatch_include}"
      "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
      "${bvb_triangle_dispatch_include}"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/first_rejection.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/triangle_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-triangle-dispatch-glibc-termux.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/first_rejection.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_first_rejection.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/first_rejection_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
  )
  add_executable(
    bvb-device-extension-dependency-test
    tests/device_extension_dependency.c
  )
  target_link_libraries(
    bvb-device-extension-dependency-test
    PRIVATE bvb-vulkan-client-dispatch Vulkan::Headers
  )
  bvb_enable_warnings(bvb-device-extension-dependency-test)
  add_test(
    NAME device-extension-dependency-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_device_extension_dependency.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-device-extension-dependency-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )
  add_test(
    NAME global-dispatch-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )
  add_test(
    NAME vulkan-loader-tls-lifetime-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "loader-tls-lifetime"
  )
  add_test(
    NAME map-memory2-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_map_memory2_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e099-map-memory2-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  )
  add_test(
    NAME first-real-rejection-command-poison-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-first-rejection-global-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "first-rejection-command"
  )
  add_test(
    NAME first-real-rejection-protected-wsi-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-first-rejection-global-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "first-rejection-wsi"
  )
  add_test(
    NAME global-dispatch-hardware-mode-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "hardware"
  )
  add_test(
    NAME global-command-stream-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "shared-command-stream"
  )
  add_test(
    NAME global-command-stream-non-success-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "shared-command-stream-non-success"
  )
  add_test(
    NAME global-command-stream-concurrency-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "shared-command-stream-concurrency"
  )
  add_test(
    NAME command-stream-client-locking-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_command_stream_client_locking.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e078-command-stream-client-locking-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
  )
  add_test(
    NAME command-ownership-cache-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_command_ownership_cache.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e080-command-ownership-cache-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/global_dispatch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
  )
  add_test(
    NAME command-stream-correctness-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_command_stream_correctness_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e075a-command-stream-correctness-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
  )
  add_test(
    NAME rich-command-stream-animation-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rich_command_stream_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e076-rich-command-stream-animation-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-activity-frame-import-v40-termux.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-rich-frame-animation-v40-termux.sh"
  )
  add_test(
    NAME global-strict-mapped-memory-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "strict-mapped-memory"
  )
  add_test(
    NAME global-shared-mapped-memory-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "shared-mapped-memory"
  )
  add_test(
    NAME global-shared-noncoherent-memory-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "shared-noncoherent-memory"
  )
  add_test(
    NAME mapped-memory-selector-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mapped_memory_selector.py"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
  )
  add_test(
    NAME global-shared-memory-unmap-lost-ack-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "shared-memory-unmap-lost-ack"
  )
  add_test(
    NAME mapped-memory-mirror-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mapped_memory_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e077-persistent-memory-mirror-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
  )
  add_test(
    NAME e077-tablet-evidence-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e077_tablet_evidence.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e077-upload-memory-transport-tablet.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e077-predeployment-rollback-tablet.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-rich-frame-animation-v40-termux.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/EXPERIMENT_LOG.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/raw/e077-authoritative-strict-global.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/raw/e077-authoritative-shared-global.json"
  )
  add_test(
    NAME steam-icd-transactional-installer-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_steam_icd.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-steam-icd-termux.sh"
  )
  add_test(
    NAME global-activity-harness-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_activity_harness.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-global-dispatch-activity-harness.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-global-dispatch-test>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/synthetic-global-activity-harness-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-global-dispatch-glibc-termux.sh"
  )
  add_test(
    NAME dxvk-dispatch-policy-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dxvk_dispatch_policy.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-dxvk-dispatch-policy.py"
      "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
      "${bvb_triangle_dispatch_include}"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
      "${bvb_dxvk_dispatch_policy_json}"
  )

  add_test(
    NAME visible-triangle-client-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_visible_triangle_client.py"
      "$<TARGET_FILE:bvb-visible-triangle-client>"
  )

  add_test(
    NAME visible-ingress-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_visible_ingress.py"
      "$<TARGET_FILE:bvb-visible-ingress-host-test>"
      "$<TARGET_FILE:bvb-visible-triangle-client>"
  )

  add_test(
    NAME shared-region-relay-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shared_region_relay.py"
      "$<TARGET_FILE:bvb-shared-region-relay>"
  )

  if(TARGET bvb-vulkan-resolve-trace)
    add_library(bvb-fake-vulkan-resolve SHARED tests/fake_vulkan_resolve.c)
    target_link_libraries(bvb-fake-vulkan-resolve PRIVATE Vulkan::Headers)
    bvb_enable_warnings(bvb-fake-vulkan-resolve)

    add_executable(bvb-vulkan-resolve-consumer tests/vulkan_resolve_consumer.c)
    target_link_libraries(
      bvb-vulkan-resolve-consumer
      PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS}
    )
    bvb_enable_warnings(bvb-vulkan-resolve-consumer)

    add_test(
      NAME vulkan-resolve-trace
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_resolve_trace.py"
        "$<TARGET_FILE:bvb-vulkan-resolve-trace>"
        "$<TARGET_FILE:bvb-fake-vulkan-resolve>"
        "$<TARGET_FILE:bvb-vulkan-resolve-consumer>"
    )

    add_test(
      NAME vulkan-dispatch-manifest
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch_manifest.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/analyze-vulkan-resolve-trace.py"
        "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    )
  endif()

  add_test(
    NAME probe-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_probe.py"
      "$<TARGET_FILE:bvb-vulkan-probe>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )

  add_test(
    NAME vulkan-selftest-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_selftest.py"
      "$<TARGET_FILE:bvb-vulkan-selftest>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )

  add_test(
    NAME vulkan-external-memory-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_external_memory.py"
      "$<TARGET_FILE:bvb-vulkan-external-memory-selftest>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )

  add_test(
    NAME external-memory-receiver-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_external_memory_receiver.py"
      "$<TARGET_FILE:bvb-external-memory-receiver>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )

  add_test(
    NAME external-image-direct-channel-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_external_image_direct.py"
      "$<TARGET_FILE:bvb-external-image-direct>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )

  add_test(
    NAME external-semaphore-probe-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_external_semaphore_probe.py"
      "$<TARGET_FILE:bvb-vulkan-external-semaphore-probe>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )

  add_test(
    NAME vulkan-surface-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_surface_probe.py"
      "$<TARGET_FILE:bvb-vulkan-surface-probe>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "$<TARGET_FILE:bvb-fake-media-ndk>"
  )

  add_test(
    NAME vulkan-present-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_present_selftest.py"
      "$<TARGET_FILE:bvb-vulkan-present-selftest>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
      "$<TARGET_FILE:bvb-fake-media-ndk>"
  )

  add_test(
    NAME bridge-integration
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bridge_integration.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-bridge-client>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )
  add_test(
    NAME e101-render-bundle-architecture-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e101_render_bundle.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_service.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
  )
  add_test(
    NAME e102-low-32bit-mapped-address-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e102_low_map_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e102-low-32bit-mapped-address-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
  )
  add_test(
    NAME e103-format3-general-graphics-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e103_graphics_format_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e103-format3-general-graphics-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/vulkan_pipeline_wire.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
  )
  add_test(
    NAME e104-depth-clip-pipeline-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e104_depth_clip_pipeline_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e104-depth-clip-pipeline-state-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/vulkan_pipeline_wire.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
  )
  add_test(
    NAME e105-transfer-command-family-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e105_transfer_family_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e105-transfer-command-family-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  )
  add_test(
    NAME e106-general-dynamic-rendering-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e106_dynamic_rendering_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e106-general-dynamic-rendering-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
  )
  add_test(
    NAME e107-synchronization2-barrier-family-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e107_barrier_family_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e107-synchronization2-barrier-family-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
  )
  add_test(
    NAME e108-vertex-index-draw-family-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e108_vertex_draw_family_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e108-vertex-index-draw-family-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  )
  add_test(
    NAME e109-dynamic-state-family-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e109_dynamic_state_family_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e109-dynamic-state-family-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  )
  add_test(
    NAME e110-clear-family-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e110_clear_family_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e110-clear-family-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_vulkan.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  )
  add_test(
    NAME e111-compact-transfer-stream-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e111_compact_transfer_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e111-compact-transfer-stream-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/command_batch.c"
  )
  add_test(
    NAME e112-command-stream-headroom-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e112_stream_headroom_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e112-command-stream-headroom-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/command_batch.c"
  )
  add_test(
    NAME e113-large-synchronization2-batch-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e113_large_barrier_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e113-large-synchronization2-batches-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/command_batch.c"
  )
  add_test(
    NAME e114-upload-copy-command-family-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e114_upload_copy_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e114-upload-copy-command-family-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/command_batch.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/command_batch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e069-init-image-commands-dispatch.txt"
  )
  add_test(
    NAME e115-opaque-android-game-surface-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e115_opaque_surface_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e115-opaque-android-game-surface-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/android/visible-host/native_main.c"
  )
  add_test(
    NAME e116-wsi-frame-profile-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e116_wsi_frame_profile_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e116-wsi-frame-profile-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/android/visible-host/native_main.c"
  )
  add_test(
    NAME e117-rpc-profile-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e117_rpc_profile_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e117-rpc-profile-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/bvb/protocol.h"
  )
  add_test(
    NAME e118-buffer-delta-mirror-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e118_buffer_mirror_architecture.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e118-buffer-delta-mirror-host.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan_global.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/global_dispatch.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_global_dispatch.py"
  )
endif()
