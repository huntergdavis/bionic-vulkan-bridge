cmake_minimum_required(VERSION 3.20)

project(
  bionic-vulkan-bridge
  VERSION 0.1.0
  DESCRIPTION "A tested bridge toward Android-native Vulkan for glibc game stacks"
  LANGUAGES C
)

include(CTest)
include(FetchContent)

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

add_executable(bvb-vulkan-probe src/vulkan_probe.c)
target_link_libraries(bvb-vulkan-probe PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
bvb_enable_warnings(bvb-vulkan-probe)

add_library(bvb-wire STATIC src/protocol.c src/transport.c)
target_include_directories(
  bvb-wire
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
bvb_enable_warnings(bvb-wire)

add_executable(bvb-bridge-service src/bridge_service.c)
target_link_libraries(bvb-bridge-service PRIVATE bvb-wire)
bvb_enable_warnings(bvb-bridge-service)

add_executable(bvb-bridge-client src/bridge_client.c)
target_link_libraries(bvb-bridge-client PRIVATE bvb-wire)
bvb_enable_warnings(bvb-bridge-client)

install(
  TARGETS bvb-vulkan-probe bvb-bridge-service bvb-bridge-client
  RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_library(bvb-fake-vulkan SHARED tests/fake_vulkan.c)
  target_link_libraries(bvb-fake-vulkan PRIVATE Vulkan::Headers)
  set_target_properties(
    bvb-fake-vulkan
    PROPERTIES
      C_VISIBILITY_PRESET hidden
      OUTPUT_NAME bvb-fake-vulkan
  )
  bvb_enable_warnings(bvb-fake-vulkan)

  add_executable(bvb-protocol-test tests/protocol.c)
  target_link_libraries(bvb-protocol-test PRIVATE bvb-wire)
  bvb_enable_warnings(bvb-protocol-test)

  add_test(NAME protocol-contract COMMAND bvb-protocol-test)

  add_test(
    NAME probe-contract
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_probe.py"
      "$<TARGET_FILE:bvb-vulkan-probe>"
      "$<TARGET_FILE:bvb-fake-vulkan>"
  )

  add_test(
    NAME bridge-integration
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bridge_integration.py"
      "$<TARGET_FILE:bvb-bridge-service>"
      "$<TARGET_FILE:bvb-bridge-client>"
  )
endif()
