#!/usr/bin/env python3
"""Keep the private Turnip patch narrow, pinned, and runtime-testable."""

import pathlib
import re
import sys


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def changed_paths(patch: str) -> set[str]:
    return set(re.findall(r"^\+\+\+ b/(.+)$", patch, re.MULTILINE))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_private_turnip_patch_contract.py REPO")
    root = pathlib.Path(sys.argv[1]).resolve()
    patches = root / "patches/mesa-26.2.0"
    resolver = (patches / "0001-export-private-icd-resolver.patch").read_text()
    maintenance = (
        patches / "0002-private-api34-maintenance56.patch"
    ).read_text()
    build = (root / "scripts/build-private-turnip-maintenance56.sh").read_text()
    probe = (root / "tools/private_turnip_maintenance56_probe.c").read_text()

    check(
        changed_paths(resolver)
        == {
            "src/vulkan/vulkan-android.sym",
            "src/vulkan/vulkan-icd-android-symbols.txt",
        },
        "resolver patch touches unexpected Mesa files",
    )
    check(
        changed_paths(maintenance) == {"src/vulkan/util/vk_extensions.py"},
        "maintenance patch touches unexpected Mesa files",
    )
    check(maintenance.count('"VK_KHR_maintenance5": 34') == 1,
          "maintenance5 API34 exception is missing or duplicated")
    check(maintenance.count('"VK_KHR_maintenance6": 34') == 1,
          "maintenance6 API34 exception is missing or duplicated")
    check('"VK_KHR_maintenance7": 34' not in maintenance,
          "maintenance7 must remain filtered on API34")
    check("mesa_archive_sha256=efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef" in build,
          "Mesa archive is not pinned")
    check("ndk_archive_sha1=87e2bb7e9be5d6a1c6cdf5ec40dd4e0c6d07c30b" in build,
          "Android NDK archive is not pinned")
    for option in (
        "-Dplatform-sdk-version=34",
        "-Dandroid-stub=true",
        "-Dandroid-libbacktrace=disabled",
        "-Dvulkan-drivers=freedreno",
        "-Dfreedreno-kmds=kgsl",
    ):
        check(option in build, f"missing build option: {option}")
    for call in (
        "vkEnumerateDeviceExtensionProperties",
        "vkGetPhysicalDeviceFeatures2",
        "vkCreateDevice",
        "vkDeviceWaitIdle",
    ):
        check(call in probe, f"native probe does not validate {call}")
    check("VK_KHR_MAINTENANCE_5_EXTENSION_NAME" in probe,
          "native probe does not enable maintenance5")
    check("VK_KHR_MAINTENANCE_6_EXTENSION_NAME" in probe,
          "native probe does not enable maintenance6")
    print("PASS: private Turnip maintenance5/6 patch remains narrow and pinned")


if __name__ == "__main__":
    main()
