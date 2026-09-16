#!/usr/bin/env python3
"""Harden B12 timestamp evidence so a build can never fail silently.

Applied only to B12 evidence builds, after the dual-stage profiler and timestamp
capability fallback transforms. The hardening does three things:

* adds a checked timestamp readback API with an optional WAIT_BIT path;
* consumes pending stage queries only after the existing slot synchronization,
  accounting for every readback attempt and reporting the first failure;
* emits an explicit availability record on the first present and periodic status
  records based on attempts rather than successful samples.

The clean runtime remains unchanged because this transform is B12-only.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from adreno_evidence_common import replace_exact

HEADER = Path("framegen/include/core/timestampquerypool.hpp")
SOURCE = Path("framegen/src/core/timestampquerypool.cpp")
CONTEXT_HEADERS = (
    Path("framegen/v3.1_include/v3_1/context.hpp"),
    Path("framegen/v3.1p_include/v3_1p/context.hpp"),
)
CONTEXT_SOURCES = (
    Path("framegen/v3.1_src/context.cpp"),
    Path("framegen/v3.1p_src/context.cpp"),
)


def replace_one_between(text: str, start: str, end: str, replacement: str, label: str) -> str:
    first = text.find(start)
    if first < 0:
        raise RuntimeError(f"{label}: start anchor not found")
    second = text.find(start, first + 1)
    if second >= 0:
        raise RuntimeError(f"{label}: start anchor is not unique")
    finish = text.find(end, first)
    if finish < 0:
        raise RuntimeError(f"{label}: end anchor not found")
    return text[:first] + replacement + text[finish:]


def patch_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "durationsMsChecked" in text:
        return
    text = replace_exact(
        text,
        "        [[nodiscard]] std::vector<double> durationsMs(const Core::Device& device) const;\n",
        "        [[nodiscard]] VkResult durationsMsChecked(\n"
        "            const Core::Device& device, std::vector<double>& durations,\n"
        "            bool waitForResults) const;\n"
        "        [[nodiscard]] std::vector<double> durationsMs(const Core::Device& device) const;\n",
        count=1,
        label=f"{path}: checked B12 readback API",
    )
    path.write_text(text, encoding="utf-8")


def patch_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "TimestampQueryPool::durationsMsChecked" not in text:
        old = '''std::vector<double> TimestampQueryPool::durationsMs(
        const Core::Device& device) const {
    if (!this->supported())
        return {};

    std::vector<uint64_t> timestamps(this->queryCount_);
    const VkResult result = vkGetQueryPoolResults(
        device.handle(),
        *this->queryPool,
        0,
        this->queryCount_,
        timestamps.size() * sizeof(uint64_t),
        timestamps.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
        return {};

    const uint32_t validBits = std::min<uint32_t>(this->timestampValidBits_, 64);
    const uint64_t mask = validBits == 64
        ? std::numeric_limits<uint64_t>::max()
        : ((uint64_t{1} << validBits) - 1);

    std::vector<double> durations;
    durations.reserve(this->queryCount_ - 1);
    for (uint32_t i = 0; i + 1 < this->queryCount_; ++i) {
        const uint64_t start = timestamps.at(i) & mask;
        const uint64_t end = timestamps.at(i + 1) & mask;
        const uint64_t delta = (end - start) & mask;
        durations.emplace_back(
            (static_cast<double>(delta) * this->timestampPeriodNs_) / 1000000.0);
    }

    return durations;
}
'''
        new = '''VkResult TimestampQueryPool::durationsMsChecked(
        const Core::Device& device, std::vector<double>& durations,
        bool waitForResults) const {
    durations.clear();
    if (!this->supported())
        return VK_ERROR_FEATURE_NOT_PRESENT;

    std::vector<uint64_t> timestamps(this->queryCount_);
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
    if (waitForResults)
        flags |= VK_QUERY_RESULT_WAIT_BIT;
    const VkResult result = vkGetQueryPoolResults(
        device.handle(),
        *this->queryPool,
        0,
        this->queryCount_,
        timestamps.size() * sizeof(uint64_t),
        timestamps.data(),
        sizeof(uint64_t),
        flags);
    if (result != VK_SUCCESS)
        return result;

    const uint32_t validBits = std::min<uint32_t>(this->timestampValidBits_, 64);
    const uint64_t mask = validBits == 64
        ? std::numeric_limits<uint64_t>::max()
        : ((uint64_t{1} << validBits) - 1);

    durations.reserve(this->queryCount_ - 1);
    for (uint32_t i = 0; i + 1 < this->queryCount_; ++i) {
        const uint64_t start = timestamps.at(i) & mask;
        const uint64_t end = timestamps.at(i + 1) & mask;
        const uint64_t delta = (end - start) & mask;
        durations.emplace_back(
            (static_cast<double>(delta) * this->timestampPeriodNs_) / 1000000.0);
    }

    return VK_SUCCESS;
}

std::vector<double> TimestampQueryPool::durationsMs(
        const Core::Device& device) const {
    std::vector<double> durations;
    (void)this->durationsMsChecked(device, durations, false);
    return durations;
}
'''
        text = replace_exact(
            text,
            old,
            new,
            count=1,
            label=f"{path}: checked timestamp readback",
        )

    if "b12-query-pool-create-failure" not in text:
        text = replace_exact(
            text,
            "    if (this->timestampPeriodNs_ <= 0.0f)\n"
            "        return;\n"
            "    if (reportedBits == 0 && !allowUnreportedTimestamps)\n"
            "        return;\n",
            "    if (this->timestampPeriodNs_ <= 0.0f) {\n"
            "        std::cerr << \"lsfg-vk: b12-query-pool-unavailable reason=invalid-timestamp-period\" << '\\n';\n"
            "        return;\n"
            "    }\n"
            "    if (reportedBits == 0 && !allowUnreportedTimestamps) {\n"
            "        std::cerr << \"lsfg-vk: b12-query-pool-unavailable reason=timestamp-bits-unreported\" << '\\n';\n"
            "        return;\n"
            "    }\n",
            count=1,
            label=f"{path}: timestamp capability failure reporting",
        )
        text = replace_exact(
            text,
            "    if (result != VK_SUCCESS || queryPoolHandle == VK_NULL_HANDLE)\n"
            "        return;\n",
            "    if (result != VK_SUCCESS || queryPoolHandle == VK_NULL_HANDLE) {\n"
            "        std::cerr << \"lsfg-vk: b12-query-pool-create-failure result=\"\n"
            "                  << static_cast<int>(result)\n"
            "                  << \" handle_valid=\" << (queryPoolHandle != VK_NULL_HANDLE ? 1 : 0)\n"
            "                  << '\\n';\n"
            "        return;\n"
            "    }\n",
            count=1,
            label=f"{path}: query-pool creation failure reporting",
        )

    path.write_text(text, encoding="utf-8")


def patch_context_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "b12MipmapsAttempts" in text:
        return
    old = (
        "        double b12MipmapsTotalMs{0.0};\n"
        "        double b12Beta4TotalMs{0.0};\n"
        "        uint32_t b12MipmapsSamples{0};\n"
        "        uint32_t b12Beta4Samples{0};\n"
    )
    new = (
        old
        + "        uint32_t b12MipmapsAttempts{0};\n"
        + "        uint32_t b12Beta4Attempts{0};\n"
        + "        uint32_t b12MipmapsFailures{0};\n"
        + "        uint32_t b12Beta4Failures{0};\n"
        + "        VkResult b12MipmapsLastResult{VK_SUCCESS};\n"
        + "        VkResult b12Beta4LastResult{VK_SUCCESS};\n"
        + "        bool b12AvailabilityLogged{false};\n"
    )
    text = replace_exact(
        text,
        old,
        new,
        count=1,
        label=f"{path}: B12 attempt/failure counters",
    )
    path.write_text(text, encoding="utf-8")


HARDENED_COLLECTION = r'''    if (data.b12MipmapsPending) {
        std::vector<double> durations;
        const VkResult result = data.b12MipmapsQueryPool.durationsMsChecked(
            vk.device, durations, true);
        ++this->b12MipmapsAttempts;
        this->b12MipmapsLastResult = result;
        if (result == VK_SUCCESS && durations.size() == 1) {
            this->b12MipmapsTotalMs += durations.front();
            ++this->b12MipmapsSamples;
        } else {
            ++this->b12MipmapsFailures;
            if (this->b12MipmapsFailures == 1) {
                std::cout << "lsfg-vk: b12-readback-failure stage=mipmaps result="
                    << static_cast<int>(result)
                    << " durations=" << durations.size() << std::endl;
            }
        }
        data.b12MipmapsPending = false;
    }
    if (data.b12Beta4Pending) {
        std::vector<double> durations;
        const VkResult result = data.b12Beta4QueryPool.durationsMsChecked(
            vk.device, durations, true);
        ++this->b12Beta4Attempts;
        this->b12Beta4LastResult = result;
        if (result == VK_SUCCESS && durations.size() == 1) {
            this->b12Beta4TotalMs += durations.front();
            ++this->b12Beta4Samples;
        } else {
            ++this->b12Beta4Failures;
            if (this->b12Beta4Failures == 1) {
                std::cout << "lsfg-vk: b12-readback-failure stage=beta4 result="
                    << static_cast<int>(result)
                    << " durations=" << durations.size() << std::endl;
            }
        }
        data.b12Beta4Pending = false;
    }
    if (this->b12MipmapsAttempts >= 120) {
        const bool clean = this->b12MipmapsFailures == 0
            && this->b12Beta4Failures == 0;
        std::cout << "lsfg-vk: b12-stage-profile status="
            << (clean ? "ok" : "degraded")
            << " mipmaps_attempts=" << this->b12MipmapsAttempts
            << " mipmaps_samples=" << this->b12MipmapsSamples
            << " mipmaps_failures=" << this->b12MipmapsFailures
            << " mipmaps_last_result=" << static_cast<int>(this->b12MipmapsLastResult)
            << " mipmaps_avg_ms="
            << (this->b12MipmapsSamples > 0
                ? this->b12MipmapsTotalMs / static_cast<double>(this->b12MipmapsSamples)
                : 0.0)
            << " beta4_attempts=" << this->b12Beta4Attempts
            << " beta4_samples=" << this->b12Beta4Samples
            << " beta4_failures=" << this->b12Beta4Failures
            << " beta4_last_result=" << static_cast<int>(this->b12Beta4LastResult)
            << " beta4_avg_ms="
            << (this->b12Beta4Samples > 0
                ? this->b12Beta4TotalMs / static_cast<double>(this->b12Beta4Samples)
                : 0.0)
            << std::endl;
        this->b12MipmapsTotalMs = 0.0;
        this->b12Beta4TotalMs = 0.0;
        this->b12MipmapsSamples = 0;
        this->b12Beta4Samples = 0;
        this->b12MipmapsAttempts = 0;
        this->b12Beta4Attempts = 0;
        this->b12MipmapsFailures = 0;
        this->b12Beta4Failures = 0;
        this->b12MipmapsLastResult = VK_SUCCESS;
        this->b12Beta4LastResult = VK_SUCCESS;
    }
'''


def patch_context_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if "b12-readback-failure" in text:
        return

    present_anchor = (
        "    const size_t generationCount = std::min(activeGenerationCount, vk.generationCount);\n"
        "    auto& data = this->data.at(this->frameIdx % 8);\n\n"
    )
    availability_block = present_anchor + r'''    if (!this->b12AvailabilityLogged) {
        const bool mipmapsSupported = this->data.at(0).b12MipmapsQueryPool.supported();
        const bool beta4Supported = this->data.at(0).b12Beta4QueryPool.supported();
        std::cout << "lsfg-vk: b12-stage-profile-init mipmaps_supported="
            << (mipmapsSupported ? 1 : 0)
            << " beta4_supported=" << (beta4Supported ? 1 : 0) << std::endl;
        if (!mipmapsSupported || !beta4Supported) {
            std::cout << "lsfg-vk: b12-stage-profile status=unavailable"
                << " reason=query-pool-unsupported"
                << " mipmaps_supported=" << (mipmapsSupported ? 1 : 0)
                << " beta4_supported=" << (beta4Supported ? 1 : 0)
                << std::endl;
        }
        this->b12AvailabilityLogged = true;
    }

'''
    text = replace_exact(
        text,
        present_anchor,
        availability_block,
        count=1,
        label=f"{path}: B12 first-present availability status",
    )

    text = replace_one_between(
        text,
        "    if (data.b12MipmapsPending) {\n",
        "    data.shouldWait = generationCount > 0;\n",
        HARDENED_COLLECTION,
        label=f"{path}: B12 checked delayed result collection",
    )
    path.write_text(text, encoding="utf-8")


def apply(root: Path) -> None:
    patch_header(root / HEADER)
    patch_source(root / SOURCE)
    for rel in CONTEXT_HEADERS:
        patch_context_header(root / rel)
    for rel in CONTEXT_SOURCES:
        patch_context_source(root / rel)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    apply(args.root.resolve())


if __name__ == "__main__":
    main()
