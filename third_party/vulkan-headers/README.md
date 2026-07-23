# Vulkan-Headers subset

This directory contains the Khronos Vulkan C headers used by the Switch Vulkan
presenter and LSFG-VK integration. They are from Vulkan-Headers 1.4.321 and
retain their Apache-2.0 SPDX notices.

The unused C++ convenience headers and non-Switch platform headers are
intentionally omitted. The `vk_video` headers are retained because
`vulkan_core.h` includes their public extension structures.

Upstream: https://github.com/KhronosGroup/Vulkan-Headers
