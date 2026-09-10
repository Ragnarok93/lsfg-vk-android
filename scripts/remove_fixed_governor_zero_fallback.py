#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "src/context.cpp"
text = path.read_text(encoding="utf-8")
old = "    VkSemaphore lastPrevPostCopySemaphore = generatedFrameCount > 0\n        ? pass.prevPostCopySemaphores.at(generatedFrameCount - 1).handle()\n        : pass.preCopySemaphores.at(0).handle();\n    const VkPresentInfoKHR finalPresentInfo{\n        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,\n        .pNext = generatedFrameCount == 0 ? pNext : nullptr,\n"
new = "    const VkSemaphore lastPrevPostCopySemaphore =\n        pass.prevPostCopySemaphores.at(generatedFrameCount - 1).handle();\n    const VkPresentInfoKHR finalPresentInfo{\n        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,\n        .pNext = nullptr,\n"
if text.count(old) != 1:
    raise SystemExit("expected one final-present zero-count fallback")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
