#pragma once

#include <vulkan/vulkan_core.h>

#include <filesystem>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Config {

    /// lsfg-vk configuration
    struct Configuration {
        /// Whether frame generation is currently enabled for this target.
        bool enable{false};
        /// Whether this executable matched an explicit LSFG game target and the
        /// layer must stay resident even while frame generation is disabled.
        bool targeted{false};
        /// Path to Lossless.dll.
        std::string dll;

        /// The frame generation multiplier.
        size_t multiplier{2};
        /// The internal flow scale factor.
        float flowScale{1.0F};
        /// Whether performance mode is enabled.
        bool performance{false};
        /// Whether HDR is enabled.
        bool hdr{false};
        /// Vary generated frame count to approach the limiter-pegged target.
        bool adaptiveFramegen{false};
        /// Canonical Adaptive target. When Adaptive is active this is resolved
        /// from sourceFpsLimit and cannot remain an independent target.
        uint32_t fpsLimit{0};
        /// GameNative's authoritative real/source FPS limiter.
        uint32_t sourceFpsLimit{0};

        /// Experimental flag for overriding the synchronization method.
        VkPresentModeKHR e_present;

        /// Path to the configuration file.
        std::filesystem::path config_file;
        /// File timestamp of the configuration file.
        std::chrono::time_point<std::chrono::file_clock> timestamp;
    };

    /// Active configuration. Must be set in main.cpp.
    extern Configuration activeConf;

    ///
    /// Read the configuration file while preserving the previous configuration
    /// in case of an error.
    ///
    /// @param file The path to the configuration file.
    ///
    /// @throws std::runtime_error if an error occurs while loading the configuration file.
    ///
    void updateConfig(const std::string& file);

    ///
    /// Get the configuration for a game.
    /// @param name The name of the executable to fetch.
    /// @return The configuration for the game or global configuration.
    /// @throws std::runtime_error if the configuration is invalid.
    ///
    Configuration getConfig(const std::pair<std::string, std::string>& name);

}
