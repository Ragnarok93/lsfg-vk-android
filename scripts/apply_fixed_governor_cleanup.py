#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def sub_once(text: str, pattern: str, repl: str, label: str, flags: int = 0) -> str:
    result, count = re.subn(pattern, repl, text, count=1, flags=flags)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one regex match, found {count}")
    return result


FIXED_HEADER = r'''#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

struct FixedFrameGovernorTelemetry {
    double sourceFps{};
    double smoothedSourceFps{};
    std::size_t requestedGeneratedFrames{};
    std::size_t costLimit{};
    std::size_t generatedFrames{};
    bool sourceRateSnapped{false};
    bool costRaised{false};
    bool costBackedOff{false};
    bool costProbe{false};
    bool refreshLimited{false};
    bool discontinuityReset{false};
};

/// Internal governor for fixed Off/2x/3x/4x modes. It can reduce the amount of
/// interpolation work beneath the requested fixed multiplier when that work is
/// not sustainable, but it never owns source pacing and never turns an enabled
/// fixed mode into a zero-generation/source-only cycle.
class FixedFrameGovernor {
public:
    void configure(bool enabled, std::size_t requestedMultiplier,
        uint32_t displayRefreshHz = 0);
    std::size_t plan(std::chrono::nanoseconds sourceInterval);
    void reset();

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] std::size_t requestedGeneratedFrames() const {
        return requestedGeneratedFrames_;
    }
    [[nodiscard]] uint32_t displayRefreshHz() const { return displayRefreshHz_; }
    [[nodiscard]] const FixedFrameGovernorTelemetry& telemetry() const {
        return telemetry_;
    }

private:
    void resetMeasurements(bool resetCostLimit);
    void resetRateCandidates();
    bool updateSourceRate(double intervalSeconds);
    [[nodiscard]] std::size_t refreshGenerationCeiling(double sourceFps) const;
    [[nodiscard]] std::size_t safeOutput() const;

    bool enabled_{false};
    std::size_t requestedGeneratedFrames_{};
    uint32_t displayRefreshHz_{};

    double smoothedSourceIntervalSeconds_{};
    bool hasSmoothedInterval_{false};
    unsigned slowRateSamples_{};
    unsigned fastRateSamples_{};
    double slowIntervalSum_{};
    double fastIntervalSum_{};

    double observedSeconds_{};
    double stableSinceSeconds_{};
    std::size_t costLimit_{};
    bool pendingRaise_{false};
    bool probeAfterBackoff_{false};
    bool pendingRaiseWasProbe_{false};
    double pendingRaiseBaselineFps_{};
    double pendingRaiseSeconds_{};
    double holdUntilSeconds_{};

    FixedFrameGovernorTelemetry telemetry_{};
};
'''


FIXED_SOURCE = r'''#include "fixed_frame_governor.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kIntervalSmoothing = 0.15;
constexpr double kSlowIntervalHigh = 1.40;
constexpr double kFastIntervalLow = 0.70;
constexpr unsigned kSlowSamplesRequired = 3;
constexpr unsigned kFastSamplesRequired = 6;
constexpr double kDiscontinuitySeconds = 0.250;
constexpr double kStableBeforeRaiseSeconds = 0.750;
constexpr double kRaiseBlameWindowSeconds = 1.250;
constexpr double kSourceDropRatio = 0.90;
constexpr double kBackoffHoldSeconds = 5.0;
constexpr double kRecoveryRatio = 0.97;
constexpr double kRefreshRaiseHeadroom = 0.98;
} // namespace

void FixedFrameGovernor::configure(bool enabled, std::size_t requestedMultiplier,
        uint32_t displayRefreshHz) {
    const std::size_t requestedGenerated = requestedMultiplier > 1
        ? std::min<std::size_t>(requestedMultiplier - 1, 3)
        : 0;
    if (enabled_ == enabled
            && requestedGeneratedFrames_ == requestedGenerated
            && displayRefreshHz_ == displayRefreshHz)
        return;

    enabled_ = enabled;
    requestedGeneratedFrames_ = requestedGenerated;
    displayRefreshHz_ = displayRefreshHz;
    resetMeasurements(true);
}

std::size_t FixedFrameGovernor::plan(std::chrono::nanoseconds sourceInterval) {
    telemetry_.sourceRateSnapped = false;
    telemetry_.costRaised = false;
    telemetry_.costBackedOff = false;
    telemetry_.costProbe = false;
    telemetry_.refreshLimited = false;
    telemetry_.discontinuityReset = false;
    telemetry_.requestedGeneratedFrames = requestedGeneratedFrames_;

    if (requestedGeneratedFrames_ == 0) {
        telemetry_.costLimit = 0;
        telemetry_.generatedFrames = 0;
        return 0;
    }

    if (!enabled_) {
        telemetry_.costLimit = requestedGeneratedFrames_;
        telemetry_.generatedFrames = requestedGeneratedFrames_;
        return requestedGeneratedFrames_;
    }

    costLimit_ = std::clamp<std::size_t>(costLimit_, 1, requestedGeneratedFrames_);
    const double intervalSeconds = std::chrono::duration<double>(sourceInterval).count();
    if (!(intervalSeconds > 0.0) || !std::isfinite(intervalSeconds)) {
        const auto output = safeOutput();
        telemetry_.costLimit = costLimit_;
        telemetry_.generatedFrames = output;
        return output;
    }

    if (intervalSeconds >= kDiscontinuitySeconds) {
        resetMeasurements(false);
        telemetry_.discontinuityReset = true;
        const auto output = safeOutput();
        telemetry_.costLimit = costLimit_;
        telemetry_.generatedFrames = output;
        return output;
    }

    observedSeconds_ += intervalSeconds;
    const bool rateSnapped = updateSourceRate(intervalSeconds);
    telemetry_.sourceRateSnapped = rateSnapped;
    const double sourceFps = telemetry_.smoothedSourceFps;

    if (pendingRaise_) {
        const double elapsed = observedSeconds_ - pendingRaiseSeconds_;
        const bool sourceDropped = pendingRaiseBaselineFps_ > 0.0
            && sourceFps < pendingRaiseBaselineFps_ * kSourceDropRatio;
        if (elapsed <= kRaiseBlameWindowSeconds && sourceDropped) {
            if (costLimit_ > 1)
                --costLimit_;
            pendingRaise_ = false;
            probeAfterBackoff_ = true;
            pendingRaiseWasProbe_ = false;
            holdUntilSeconds_ = observedSeconds_ + kBackoffHoldSeconds;
            stableSinceSeconds_ = observedSeconds_;
            telemetry_.costBackedOff = true;
        } else if (elapsed >= kRaiseBlameWindowSeconds) {
            pendingRaise_ = false;
            pendingRaiseWasProbe_ = false;
            stableSinceSeconds_ = observedSeconds_;
        }
    }

    const std::size_t refreshCeiling = refreshGenerationCeiling(sourceFps);
    if (refreshCeiling < costLimit_) {
        costLimit_ = std::max<std::size_t>(1, refreshCeiling);
        pendingRaise_ = false;
        pendingRaiseWasProbe_ = false;
        stableSinceSeconds_ = observedSeconds_;
        telemetry_.refreshLimited = true;
    }

    if (rateSnapped)
        stableSinceSeconds_ = observedSeconds_;

    if (costLimit_ < requestedGeneratedFrames_
            && !pendingRaise_
            && observedSeconds_ >= holdUntilSeconds_
            && observedSeconds_ - stableSinceSeconds_ >= kStableBeforeRaiseSeconds) {
        const std::size_t candidate = costLimit_ + 1;
        const bool refreshAllows = displayRefreshHz_ == 0 || sourceFps <= 0.0
            || sourceFps * static_cast<double>(candidate + 1)
                <= static_cast<double>(displayRefreshHz_) * kRefreshRaiseHeadroom;
        const bool recovered = !probeAfterBackoff_
            || pendingRaiseBaselineFps_ <= 0.0
            || sourceFps >= pendingRaiseBaselineFps_ * kRecoveryRatio;
        if (refreshAllows && recovered) {
            ++costLimit_;
            pendingRaise_ = true;
            pendingRaiseWasProbe_ = probeAfterBackoff_;
            probeAfterBackoff_ = false;
            pendingRaiseBaselineFps_ = sourceFps;
            pendingRaiseSeconds_ = observedSeconds_;
            telemetry_.costRaised = true;
            telemetry_.costProbe = pendingRaiseWasProbe_;
        } else if (!refreshAllows) {
            telemetry_.refreshLimited = true;
        }
    }

    const auto output = safeOutput();
    telemetry_.costLimit = costLimit_;
    telemetry_.generatedFrames = output;
    return output;
}

void FixedFrameGovernor::resetRateCandidates() {
    slowRateSamples_ = 0;
    fastRateSamples_ = 0;
    slowIntervalSum_ = 0.0;
    fastIntervalSum_ = 0.0;
}

bool FixedFrameGovernor::updateSourceRate(double intervalSeconds) {
    telemetry_.sourceFps = 1.0 / intervalSeconds;
    bool snapped = false;
    if (!hasSmoothedInterval_) {
        smoothedSourceIntervalSeconds_ = intervalSeconds;
        hasSmoothedInterval_ = true;
        stableSinceSeconds_ = observedSeconds_;
        resetRateCandidates();
    } else if (intervalSeconds > smoothedSourceIntervalSeconds_ * kSlowIntervalHigh) {
        ++slowRateSamples_;
        slowIntervalSum_ += intervalSeconds;
        fastRateSamples_ = 0;
        fastIntervalSum_ = 0.0;
        if (slowRateSamples_ >= kSlowSamplesRequired) {
            smoothedSourceIntervalSeconds_ = slowIntervalSum_ / slowRateSamples_;
            resetRateCandidates();
            snapped = true;
        }
    } else if (intervalSeconds < smoothedSourceIntervalSeconds_ * kFastIntervalLow) {
        ++fastRateSamples_;
        fastIntervalSum_ += intervalSeconds;
        slowRateSamples_ = 0;
        slowIntervalSum_ = 0.0;
        if (fastRateSamples_ >= kFastSamplesRequired) {
            smoothedSourceIntervalSeconds_ = fastIntervalSum_ / fastRateSamples_;
            resetRateCandidates();
            snapped = true;
        }
    } else {
        resetRateCandidates();
        smoothedSourceIntervalSeconds_ +=
            kIntervalSmoothing * (intervalSeconds - smoothedSourceIntervalSeconds_);
    }

    telemetry_.smoothedSourceFps = smoothedSourceIntervalSeconds_ > 0.0
        ? 1.0 / smoothedSourceIntervalSeconds_
        : 0.0;
    return snapped;
}

std::size_t FixedFrameGovernor::refreshGenerationCeiling(double sourceFps) const {
    if (displayRefreshHz_ == 0 || !(sourceFps > 0.0))
        return requestedGeneratedFrames_;

    const double slots = static_cast<double>(displayRefreshHz_) / sourceFps;
    const auto wholeSlots = static_cast<std::size_t>(std::floor(slots + 1e-6));
    if (wholeSlots <= 1)
        return 1;
    return std::clamp<std::size_t>(wholeSlots - 1, 1, requestedGeneratedFrames_);
}

std::size_t FixedFrameGovernor::safeOutput() const {
    if (requestedGeneratedFrames_ == 0)
        return 0;
    return std::clamp<std::size_t>(costLimit_, 1, requestedGeneratedFrames_);
}

void FixedFrameGovernor::resetMeasurements(bool resetCostLimit) {
    smoothedSourceIntervalSeconds_ = 0.0;
    hasSmoothedInterval_ = false;
    resetRateCandidates();
    observedSeconds_ = 0.0;
    stableSinceSeconds_ = 0.0;
    pendingRaise_ = false;
    probeAfterBackoff_ = false;
    pendingRaiseWasProbe_ = false;
    pendingRaiseBaselineFps_ = 0.0;
    pendingRaiseSeconds_ = 0.0;
    holdUntilSeconds_ = 0.0;
    if (resetCostLimit)
        costLimit_ = requestedGeneratedFrames_ == 0 ? 0 : 1;
    telemetry_ = {};
    telemetry_.requestedGeneratedFrames = requestedGeneratedFrames_;
    telemetry_.costLimit = costLimit_;
    telemetry_.generatedFrames = safeOutput();
}

void FixedFrameGovernor::reset() {
    resetMeasurements(true);
}
'''


FIXED_TEST = r'''#include "fixed_frame_governor.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>

using namespace std::chrono_literals;

static std::size_t runStable(FixedFrameGovernor& governor,
        std::chrono::nanoseconds interval, int frames) {
    std::size_t out = 0;
    for (int i = 0; i < frames; ++i)
        out = governor.plan(interval);
    return out;
}

int main() {
    {
        FixedFrameGovernor governor;
        governor.configure(false, 2, 0);
        assert(governor.plan(16ms) == 1);
        governor.configure(false, 3, 0);
        assert(governor.plan(16ms) == 2);
        governor.configure(false, 4, 0);
        assert(governor.plan(16ms) == 3);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 2, 0);
        assert(governor.plan(0ns) == 1);
        assert(governor.plan(500ms) == 1);
        assert(runStable(governor, 16ms, 300) == 1);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 0);
        for (int i = 0; i < 500; ++i) {
            const auto out = governor.plan(16ms);
            assert(out >= 1 && out <= 3);
        }
        assert(governor.telemetry().costLimit == 3);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 120);
        const auto out = runStable(governor, 16'666'667ns, 400);
        assert(out == 1);
        assert(governor.telemetry().costLimit == 1);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 0);
        runStable(governor, 16ms, 200);
        assert(governor.plan(400ms) >= 1);
        assert(governor.telemetry().discontinuityReset);
        assert(governor.plan(16ms) >= 1);
    }
    {
        FixedFrameGovernor governor;
        governor.configure(true, 4, 0);
        runStable(governor, 16ms, 100);
        governor.configure(true, 3, 0);
        const auto out = governor.plan(16ms);
        assert(out == 1);
        assert(out <= 2);
    }
}
'''


def main() -> None:
    write("include/fixed_frame_governor.hpp", FIXED_HEADER)
    write("src/fixed_frame_governor.cpp", FIXED_SOURCE)
    write("tests/fixed_frame_governor_test.cpp", FIXED_TEST)

    path = "include/config/config.hpp"
    text = read(path)
    text = sub_once(
        text,
        r"\n        /// Vary the generated frame count.*?uint32_t fpsLimit\{0\};\n",
        "\n        /// Allow an internal sustainability governor beneath a fixed 2x/3x/4x\n"
        "        /// request. Defaults off so standalone behavior remains unchanged.\n"
        "        bool fixedGovernor{false};\n"
        "        /// Optional physical display refresh used only to avoid generating slots\n"
        "        /// that cannot scan out. Zero means unknown and applies no refresh clamp.\n"
        "        uint32_t displayRefreshHz{0};\n",
        "config header fixed-governor block",
        re.S,
    )
    write(path, text)

    path = "src/config/config.cpp"
    text = read(path)
    text = sub_once(
        text,
        r"\n    struct GameNativeAdaptiveOverride \{.*?\n    \}\n\}",
        "\n}",
        "remove GameNative Adaptive overlay reader",
        re.S,
    )
    text = text.replace("\n    const auto gameNativeAdaptive = read_gamenative_adaptive_override(file);\n", "\n")
    text = replace_once(
        text,
        "            .adaptiveFramegen = toml::find_or(gameTable, \"adaptive_framegen\", false),\n"
        "            .fpsLimit = toml::find_or(gameTable, \"fps_limit\", 0U),\n",
        "            .fixedGovernor = toml::find_or(gameTable, \"fixed_governor\", false),\n"
        "            .displayRefreshHz = toml::find_or(gameTable, \"display_refresh_hz\", 0U),\n",
        "game config governor fields",
    )
    text = sub_once(
        text,
        r"\n        if \(gameNativeAdaptive\.has_value\(\)\) \{.*?\n        \}\n",
        "\n",
        "remove Adaptive overlay assignment",
        re.S,
    )
    text = text.replace(
        "        if (game.adaptiveFramegen && game.fpsLimit == 0)\n"
        "            throw std::runtime_error(\"Adaptive frame generation requires a positive fps_limit\");\n",
        "",
    )
    text = text.replace(
        "        const char* adaptive = std::getenv(\"LSFG_ADAPTIVE_FRAMEGEN\");\n"
        "        if (adaptive) conf.adaptiveFramegen = std::string(adaptive) == \"1\";\n"
        "        const char* fpsLimit = std::getenv(\"LSFG_FPS_LIMIT\");\n"
        "        if (fpsLimit) conf.fpsLimit = std::stoul(fpsLimit);\n",
        "        const char* fixedGovernor = std::getenv(\"LSFG_FIXED_GOVERNOR\");\n"
        "        if (fixedGovernor) conf.fixedGovernor = std::string(fixedGovernor) == \"1\";\n"
        "        const char* displayRefresh = std::getenv(\"LSFG_DISPLAY_REFRESH_HZ\");\n"
        "        if (displayRefresh) conf.displayRefreshHz = std::stoul(displayRefresh);\n",
    )
    if re.search(r"adaptive|fps_limit|fpsLimit|LSFG_FPS_LIMIT|LSFG_ADAPTIVE", text, re.I):
        raise RuntimeError("vestigial Adaptive config code remains")
    write(path, text)

    path = "include/context.hpp"
    text = read(path)
    text = replace_once(text, '#include "adaptive_scheduler.hpp"\n', '#include "fixed_frame_governor.hpp"\n', "context governor include")
    text = replace_once(text, "    AdaptiveFrameScheduler adaptiveScheduler_;\n", "    FixedFrameGovernor fixedGovernor_;\n", "context governor member")
    text = sub_once(
        text,
        r"\n        uint64_t windowAdaptiveZeroGenerationCycles\{0\};.*?uint64_t totalAdaptiveDiscontinuities\{0\};",
        "\n        uint64_t windowGovernorRateSnaps{0};\n"
        "        uint64_t totalGovernorRateSnaps{0};\n"
        "        uint64_t windowGovernorCostRaises{0};\n"
        "        uint64_t totalGovernorCostRaises{0};\n"
        "        uint64_t windowGovernorCostBackoffs{0};\n"
        "        uint64_t totalGovernorCostBackoffs{0};\n"
        "        uint64_t windowGovernorCostProbes{0};\n"
        "        uint64_t totalGovernorCostProbes{0};\n"
        "        uint64_t windowGovernorRefreshLimits{0};\n"
        "        uint64_t totalGovernorRefreshLimits{0};\n"
        "        uint64_t windowGovernorDiscontinuities{0};\n"
        "        uint64_t totalGovernorDiscontinuities{0};",
        "context governor metrics",
        re.S,
    )
    if "adaptive" in text.lower():
        raise RuntimeError("vestigial Adaptive context header code remains")
    write(path, text)

    path = "src/context.cpp"
    text = read(path)
    text = text.replace(
        "        std::cerr << \"  Adaptive FrameGen: \" << (conf.adaptiveFramegen ? \"Enabled\" : \"Disabled\") << '\\n';\n"
        "        if (conf.adaptiveFramegen) std::cerr << \"  Output FPS Cap: \" << conf.fpsLimit << '\\n';\n",
        "        std::cerr << \"  Fixed Governor: \" << (conf.fixedGovernor ? \"Enabled\" : \"Disabled\") << '\\n';\n"
        "        if (conf.displayRefreshHz > 0) std::cerr << \"  Display Refresh: \" << conf.displayRefreshHz << \" Hz\\n\";\n",
    )
    governor_block = r'''    this->fixedGovernor_.configure(
        conf.fixedGovernor, conf.multiplier, conf.displayRefreshHz);
    std::chrono::nanoseconds sourceInterval{};
    if (metrics.hasLastSourcePresent) {
        sourceInterval = std::chrono::duration_cast<std::chrono::nanoseconds>(
            cycleStart - metrics.lastSourcePresent);
        const double sourceIntervalMs = std::chrono::duration<double, std::milli>(
            sourceInterval).count();
        metrics.windowSourceIntervalMs += sourceIntervalMs;
        if (sourceIntervalMs > metrics.windowSourceIntervalMaxMs)
            metrics.windowSourceIntervalMaxMs = sourceIntervalMs;
        metrics.windowSourceIntervals++;
    }
    metrics.lastSourcePresent = cycleStart;
    metrics.hasLastSourcePresent = true;
    const size_t generatedFrameCount = this->fixedGovernor_.plan(sourceInterval);
    const auto& governorTelemetry = this->fixedGovernor_.telemetry();
    const bool warmupSourceHistory = this->requiresSourceHistoryWarmup_;
    this->lastGeneratedFrameCount_ = generatedFrameCount;

    if (conf.fixedGovernor) {
        if (governorTelemetry.sourceRateSnapped) {
            metrics.windowGovernorRateSnaps++;
            metrics.totalGovernorRateSnaps++;
        }
        if (governorTelemetry.costRaised) {
            metrics.windowGovernorCostRaises++;
            metrics.totalGovernorCostRaises++;
        }
        if (governorTelemetry.costBackedOff) {
            metrics.windowGovernorCostBackoffs++;
            metrics.totalGovernorCostBackoffs++;
        }
        if (governorTelemetry.costProbe) {
            metrics.windowGovernorCostProbes++;
            metrics.totalGovernorCostProbes++;
        }
        if (governorTelemetry.refreshLimited) {
            metrics.windowGovernorRefreshLimits++;
            metrics.totalGovernorRefreshLimits++;
        }
        if (governorTelemetry.discontinuityReset) {
            metrics.windowGovernorDiscontinuities++;
            metrics.totalGovernorDiscontinuities++;
        }
        if (governorTelemetry.sourceRateSnapped || governorTelemetry.costRaised
                || governorTelemetry.costBackedOff || governorTelemetry.costProbe
                || governorTelemetry.refreshLimited || governorTelemetry.discontinuityReset) {
            std::cerr << "lsfg-vk: fixed-governor-event"
                      << " source_fps=" << governorTelemetry.sourceFps
                      << " smoothed_source_fps=" << governorTelemetry.smoothedSourceFps
                      << " requested_generated=" << governorTelemetry.requestedGeneratedFrames
                      << " cost_limit=" << governorTelemetry.costLimit
                      << " final_generated=" << governorTelemetry.generatedFrames
                      << " rate_snap=" << (governorTelemetry.sourceRateSnapped ? 1 : 0)
                      << " cost_raise=" << (governorTelemetry.costRaised ? 1 : 0)
                      << " cost_backoff=" << (governorTelemetry.costBackedOff ? 1 : 0)
                      << " cost_probe=" << (governorTelemetry.costProbe ? 1 : 0)
                      << " refresh_limited=" << (governorTelemetry.refreshLimited ? 1 : 0)
                      << " discontinuity=" << (governorTelemetry.discontinuityReset ? 1 : 0)
                      << "\n";
        }
    }

    const bool firstPresentDiagnostic'''
    text = sub_once(
        text,
        r"    this->adaptiveScheduler_\.configure\(.*?\n    const bool firstPresentDiagnostic",
        governor_block,
        "replace Adaptive planning block",
        re.S,
    )
    text = text.replace(
        "                  << \" adaptive=\" << (conf.adaptiveFramegen ? 1 : 0)\n"
        "                  << \" target_fps=\" << conf.fpsLimit\n",
        "                  << \" fixed_governor=\" << (conf.fixedGovernor ? 1 : 0)\n"
        "                  << \" display_refresh_hz=\" << conf.displayRefreshHz\n",
    )
    text = sub_once(
        text,
        r"\n                      << \" adaptive_source_fps=\".*?\n                      << \" performance=\"",
        "\n                      << \" governor_source_fps=\" << governorTelemetry.sourceFps\n"
        "                      << \" governor_smoothed_source_fps=\" << governorTelemetry.smoothedSourceFps\n"
        "                      << \" governor_requested_generated=\" << governorTelemetry.requestedGeneratedFrames\n"
        "                      << \" governor_cost_limit=\" << governorTelemetry.costLimit\n"
        "                      << \" governor_final_generated=\" << governorTelemetry.generatedFrames\n"
        "                      << \" governor_rate_snaps=\" << metrics.windowGovernorRateSnaps\n"
        "                      << \" governor_rate_snaps_total=\" << metrics.totalGovernorRateSnaps\n"
        "                      << \" governor_cost_raises=\" << metrics.windowGovernorCostRaises\n"
        "                      << \" governor_cost_raises_total=\" << metrics.totalGovernorCostRaises\n"
        "                      << \" governor_cost_backoffs=\" << metrics.windowGovernorCostBackoffs\n"
        "                      << \" governor_cost_backoffs_total=\" << metrics.totalGovernorCostBackoffs\n"
        "                      << \" governor_cost_probes=\" << metrics.windowGovernorCostProbes\n"
        "                      << \" governor_cost_probes_total=\" << metrics.totalGovernorCostProbes\n"
        "                      << \" governor_refresh_limits=\" << metrics.windowGovernorRefreshLimits\n"
        "                      << \" governor_refresh_limits_total=\" << metrics.totalGovernorRefreshLimits\n"
        "                      << \" governor_discontinuities=\" << metrics.windowGovernorDiscontinuities\n"
        "                      << \" governor_discontinuities_total=\" << metrics.totalGovernorDiscontinuities\n"
        "                      << \" source_history_valid=\" << (this->requiresSourceHistoryWarmup_ ? 0 : 1)\n"
        "                      << \" multiplier=\" << conf.multiplier\n"
        "                      << \" fixed_governor=\" << (conf.fixedGovernor ? 1 : 0)\n"
        "                      << \" display_refresh_hz=\" << conf.displayRefreshHz\n"
        "                      << \" performance=\"",
        "replace Adaptive metrics output",
        re.S,
    )
    text = re.sub(r"\n            metrics\.windowAdaptive[^;]+;", "", text)
    reset_anchor = "            metrics.windowSyncHandoffs = 0;\n"
    if reset_anchor not in text:
        raise RuntimeError("governor reset anchor not found")
    text = text.replace(
        reset_anchor,
        "            metrics.windowGovernorRateSnaps = 0;\n"
        "            metrics.windowGovernorCostRaises = 0;\n"
        "            metrics.windowGovernorCostBackoffs = 0;\n"
        "            metrics.windowGovernorCostProbes = 0;\n"
        "            metrics.windowGovernorRefreshLimits = 0;\n"
        "            metrics.windowGovernorDiscontinuities = 0;\n"
        + reset_anchor,
        1,
    )
    text = text.replace(
        "    // 1. Copy every active Adaptive source frame into frame_0/frame_1, even on\n"
        "    // a zero-generation cadence cycle. That zero is cadence, not lifecycle: it\n"
        "    // must refresh temporal history instead of entering the Off/source-only path.\n",
        "    // 1. Copy every active fixed-mode source frame into frame_0/frame_1.\n"
        "    // The governor never returns zero while frame generation is enabled.\n",
    )
    text = text.replace(
        "    // Warm-up and zero-generation cycles deliberately retain the proven host\n"
        "    // fence path. Only ordinary generated cycles can use the optional dedicated\n"
        "    // cross-device semaphore, keeping source-only/history transitions unchanged.\n",
        "    // Re-enable warm-up deliberately retains the proven host-fence path.\n"
        "    // Ordinary generated cycles can use the optional dedicated cross-device\n"
        "    // semaphore without changing source-only/history transitions.\n",
    )
    text = sub_once(
        text,
        r"\n    if \(adaptiveZeroGeneration\) \{.*?\n    \}\n\n    if \(warmupSourceHistory\)",
        "\n    if (warmupSourceHistory)",
        "remove zero-generation Adaptive history branch",
        re.S,
    )
    text = replace_once(
        text,
        "    this->lastGeneratedFrameCount_ = 0;\n    this->requiresSourceHistoryWarmup_ = true;\n    this->previousSourceCopySignalValid_ = false;\n",
        "    this->lastGeneratedFrameCount_ = 0;\n    this->fixedGovernor_.reset();\n    this->requiresSourceHistoryWarmup_ = true;\n    this->previousSourceCopySignalValid_ = false;\n",
        "reset governor on source-only bypass",
    )
    if re.search(r"adaptive|fpsLimit|target_fps", text, re.I):
        raise RuntimeError("vestigial Adaptive context code remains")
    write(path, text)

    path = "src/hooks.cpp"
    text = read(path)
    text = text.replace("adaptiveFramegen", "fixedGovernor")
    text = text.replace("fpsLimit", "displayRefreshHz")
    text = text.replace("bool adaptive, uint32_t targetFps", "bool fixedGovernor, uint32_t displayRefreshHz")
    text = text.replace("adaptive, targetFps", "fixedGovernor, displayRefreshHz")
    text = text.replace('"adaptive=" << (adaptive ? 1 : 0)', '"fixed_governor=" << (fixedGovernor ? 1 : 0)')
    text = text.replace('"target_fps=" << targetFps', '"display_refresh_hz=" << displayRefreshHz')
    text = text.replace('" adaptive="', '" fixed_governor="')
    text = text.replace('" targetFps="', '" displayRefreshHz="')
    text = text.replace('<< "adaptive=" << (fixedGovernor ? 1 : 0)', '<< "fixed_governor=" << (fixedGovernor ? 1 : 0)')
    text = text.replace('<< "target_fps=" << displayRefreshHz', '<< "display_refresh_hz=" << displayRefreshHz')
    text = text.replace("adaptive, displayRefreshHz", "fixedGovernor, displayRefreshHz")
    if re.search(r"adaptive|targetFps|fpsLimit|target_fps", text, re.I):
        raise RuntimeError("vestigial Adaptive hook code remains")
    write(path, text)

    path = ".github/workflows/android-bionic.yml"
    text = read(path)
    text = text.replace("          python3 tests/android_adaptive_history_test.py\n", "")
    text = replace_once(
        text,
        "            src/adaptive_scheduler.cpp \\\n            tests/adaptive_scheduler_test.cpp \\\n            -o /tmp/lsfg-adaptive-scheduler-test\n          /tmp/lsfg-adaptive-scheduler-test\n",
        "            src/fixed_frame_governor.cpp \\\n            tests/fixed_frame_governor_test.cpp \\\n            -o /tmp/lsfg-fixed-frame-governor-test\n          /tmp/lsfg-fixed-frame-governor-test\n",
        "CI fixed governor test",
    )
    write(path, text)

    path = "tests/android_runtime_stability_test.py"
    text = read(path)
    text = re.sub(
        r"\n    def test_[^\n]*adaptive[^\n]*\n.*?(?=\n    def |\n\nif __name__ ==)",
        "",
        text,
        flags=re.S | re.I,
    )
    text = text.replace(
        '"""Regression: adaptive 4x left generationCount=3 active for fixed 2x\'s one AHB."""',
        '"""A higher fixed-mode capacity must not leak into a lower fixed-mode request."""',
    )
    new_contract = r'''
    def test_fixed_governor_never_turns_enabled_mode_into_source_only(self) -> None:
        header = (ROOT / "include/fixed_frame_governor.hpp").read_text(encoding="utf-8")
        source = (ROOT / "src/fixed_frame_governor.cpp").read_text(encoding="utf-8")
        context = (ROOT / "src/context.cpp").read_text(encoding="utf-8")
        config = (ROOT / "src/config/config.cpp").read_text(encoding="utf-8")

        self.assertIn("never turns an enabled", header)
        self.assertIn("std::clamp<std::size_t>(costLimit_, 1, requestedGeneratedFrames_)", source)
        self.assertIn("fixedGovernor_.plan(sourceInterval)", context)
        self.assertNotIn("generatedFrameCount == 0", context)
        self.assertNotIn("delayUntilNextSourceOutput", context)
        self.assertIn('"fixed_governor"', config)
        self.assertIn('"display_refresh_hz"', config)
'''
    marker = '\n\nif __name__ == "__main__":\n'
    if marker not in text:
        raise RuntimeError("runtime stability test insertion point missing")
    text = text.replace(marker, new_contract + marker, 1)
    write(path, text)

    for obsolete in (
        "include/adaptive_scheduler.hpp",
        "src/adaptive_scheduler.cpp",
        "tests/adaptive_scheduler_test.cpp",
        "tests/android_adaptive_history_test.py",
        "tests/gamenative_adaptive_config_test.py",
    ):
        p = ROOT / obsolete
        if p.exists():
            p.unlink()

    leftovers = []
    for base in ("include", "src", "tests", ".github/workflows"):
        for p in (ROOT / base).rglob("*"):
            if not p.is_file():
                continue
            try:
                body = p.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            if re.search(
                r"adaptive_framegen|AdaptiveFrameScheduler|gamenative-adaptive|target_output_fps|LSFG_ADAPTIVE_FRAMEGEN|LSFG_FPS_LIMIT",
                body,
            ):
                leftovers.append(str(p.relative_to(ROOT)))
    if leftovers:
        raise RuntimeError("vestigial Adaptive code remains: " + ", ".join(leftovers))


if __name__ == "__main__":
    main()
