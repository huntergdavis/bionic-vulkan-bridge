#!/usr/bin/env python3

from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: test_e101_render_bundle.py PROTOCOL COMMAND_HEADER "
            "COMMAND_SOURCE GLOBAL_SOURCE SERVICE_SOURCE NATIVE_SOURCE TEST"
        )
    protocol, header, command, client, service, native, test = (
        Path(value).read_text() for value in sys.argv[1:]
    )
    require(protocol, "BVB_OPCODE_VULKAN_COMMAND_BUFFER_IMMEDIATE_RECORD = 114",
            "strict immediate opcode")
    require(header, "BVB_COMMAND_VULKAN_PUSH_CONSTANTS = 13",
            "push-constant record")
    require(command, "bvb_command_batch_append_vulkan_push_constants",
            "push encoder")
    for name in (
        "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdBindPipeline",
        "vkCmdPushConstants", "vkCmdSetViewportWithCount",
        "vkCmdSetScissorWithCount", "vkCmdDraw",
    ):
        require(client, f'BVB_DEVICE_MATCH("{name}"', f"client {name}")
    require(client, "submit_single_render_record", "shared/strict router")
    require(service, "answer_vulkan_command_buffer_immediate_record",
            "service immediate answer")
    require(native, "validate_render_command_record", "native validation")
    require(native, "replay_render_command_record", "native replay")
    require(native, "descriptor_template_resolve_image_view",
            "image-view lineage")
    require(test, "recording_rtts == (shared_command_stream ? 0U : 47U)",
            "strict/shared A/B")
    require(test, "VK_ATTACHMENT_LOAD_OP_DONT_CARE",
            "pinned swapchain attachment shape")
    print("PASS: E101 DXVK render bundle has strict and shared paths")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
