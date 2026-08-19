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

add_executable(bvb-vulkan-probe src/vulkan_probe.c)
target_link_libraries(bvb-vulkan-probe PRIVATE bvb-vulkan-caps)
bvb_enable_warnings(bvb-vulkan-probe)

add_executable(bvb-vulkan-selftest src/vulkan_selftest_main.c)
target_link_libraries(bvb-vulkan-selftest PRIVATE bvb-vulkan-selftest-core)
bvb_enable_warnings(bvb-vulkan-selftest)

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

add_library(bvb-wire STATIC src/lifecycle.c src/protocol.c src/transport.c)
set_target_properties(bvb-wire PROPERTIES C_VISIBILITY_PRESET hidden)
target_include_directories(
  bvb-wire
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
bvb_enable_warnings(bvb-wire)

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
target_link_libraries(bvb-vulkan-selftest-core PRIVATE bvb-command-batch)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/config/e034-mapped-memory-dispatch.txt"
    --gate E034
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-dxvk-dispatch-policy.py"
    "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
    "${bvb_triangle_dispatch_include}"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/e034-mapped-memory-dispatch.txt"
  VERBATIM
)
add_library(
  bvb-vulkan-client-dispatch SHARED
  src/dxvk_dispatch_policy.c
  src/global_dispatch.c
  src/triangle_dispatch.c
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
  PRIVATE bvb-command-batch bvb-vulkan-discovery bvb-wire Threads::Threads
          Vulkan::Headers
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
          bvb-vulkan-selftest-core
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

install(
  TARGETS bvb-vulkan-probe bvb-bridge-service bvb-bridge-client
          bvb-vulkan-selftest bvb-vulkan-surface-probe
          bvb-vulkan-present-selftest bvb-vulkan-client-dispatch
          bvb-visible-triangle-client bvb-shared-region-relay
  RUNTIME DESTINATION bin
)

if(TARGET bvb-vulkan-resolve-trace)
  install(TARGETS bvb-vulkan-resolve-trace LIBRARY DESTINATION lib)
endif()

if(BUILD_TESTING)
  add_library(bvb-fake-vulkan SHARED tests/fake_vulkan.c)
  target_link_libraries(bvb-fake-vulkan PRIVATE Vulkan::Headers)
  set_target_properties(
    bvb-fake-vulkan
    PROPERTIES
      C_VISIBILITY_PRESET hidden
      OUTPUT_NAME bvb-fake-vulkan
  )
  bvb_enable_warnings(bvb-fake-vulkan)

  add_library(bvb-fake-media-ndk SHARED tests/fake_media_ndk.c)
  set_target_properties(
    bvb-fake-media-ndk
    PROPERTIES
      C_VISIBILITY_PRESET hidden
      OUTPUT_NAME bvb-fake-media-ndk
  )
  bvb_enable_warnings(bvb-fake-media-ndk)

  add_executable(bvb-protocol-test tests/protocol.c)
  target_link_libraries(bvb-protocol-test PRIVATE bvb-wire)
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
    NAME dxvk-dispatch-policy-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dxvk_dispatch_policy.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-dxvk-dispatch-policy.py"
      "${vulkanheaders_SOURCE_DIR}/registry/vk.xml"
      "${CMAKE_CURRENT_SOURCE_DIR}/docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json"
      "${bvb_triangle_dispatch_include}"
      "${CMAKE_CURRENT_SOURCE_DIR}/config/e034-mapped-memory-dispatch.txt"
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
endif()
