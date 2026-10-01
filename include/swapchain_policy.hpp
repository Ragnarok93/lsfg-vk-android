#pragma once

namespace lsfg::wsi {

template <typename Mode>
[[nodiscard]] constexpr Mode modeRequestForCreate(
        bool hasOldSwapchain,
        Mode previousConfiguredMode,
        Mode configuredMode,
        Mode gameRequestedMode) noexcept {
    return hasOldSwapchain && previousConfiguredMode == configuredMode
        ? gameRequestedMode
        : configuredMode;
}

template <typename Mode>
[[nodiscard]] constexpr Mode applyResidentFifoBackend(
        Mode effectiveMode,
        bool targetedAndroid,
        Mode configuredMode,
        Mode fifoMode,
        Mode mailboxMode,
        bool mailboxSupported) noexcept {
    return targetedAndroid && configuredMode == fifoMode && mailboxSupported
        ? mailboxMode
        : effectiveMode;
}

template <typename Mode>
[[nodiscard]] constexpr bool needsPhysicalRecreation(
        Mode currentEffectiveMode,
        Mode desiredEffectiveMode) noexcept {
    return currentEffectiveMode != desiredEffectiveMode;
}

} // namespace lsfg::wsi
