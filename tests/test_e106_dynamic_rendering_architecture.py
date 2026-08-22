#!/usr/bin/env python3

import json
from pathlib import Path
import sys


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit(
            "usage: test_e106_dynamic_rendering_architecture.py "
            "EVIDENCE HEADER CODEC CLIENT NATIVE FAKE"
        )
    evidence_path, header_path, codec_path, client_path, native_path, fake_path = map(
        Path, sys.argv[1:]
    )
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert evidence["gate"] == "E106"
    assert evidence["implementation"]["maximum_color_attachments"] == 8
    assert evidence["implementation"]["resolve_attachments"] is True
    assert evidence["implementation"]["attachment_feedback_loop_info"] is True
    assert evidence["validation"]["visible_game_frame"] is False

    header = header_path.read_text(encoding="utf-8")
    codec = codec_path.read_text(encoding="utf-8")
    client = client_path.read_text(encoding="utf-8")
    native = native_path.read_text(encoding="utf-8")
    fake = fake_path.read_text(encoding="utf-8")
    require(header, "BVB_COMMAND_VULKAN_MAX_COLOR_ATTACHMENTS = 8", "color bound")
    require(header, "resolve_image_view_id", "typed resolve view")
    require(header, "has_depth_attachment", "depth presence")
    require(codec, "BVB_BEGIN_RENDERING_SIZE", "fixed rendering record")
    require(codec, "rendering_attachment_is_valid", "attachment validation")
    require(client, "rendering_attachment_to_command", "client reconstruction")
    require(client, "rendering_command_ownership_locked", "ownership cache")
    require(client, "BVB_ICD_BEGIN_RENDERING", "runtime shape diagnostic")
    require(native, "rendering_attachment_from_command", "Bionic reconstruction")
    require(native, "VkAttachmentFeedbackLoopInfoEXT", "feedback pNext")
    require(fake, "VK_RESOLVE_MODE_AVERAGE_BIT", "resolve contract")
    require(fake, "info->colorAttachmentCount == 2U", "multi-color contract")
    require(fake, "depth != NULL && stencil != NULL", "depth/stencil contract")
    print("PASS: E106 general dynamic-rendering architecture")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
