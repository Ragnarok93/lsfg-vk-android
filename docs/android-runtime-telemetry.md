# Android runtime generation telemetry

`stats.txt` is published by the existing background runtime I/O worker. It exposes both rates and measured work for the most recent reporting interval:

- `fps`: presented output FPS.
- `source_fps`: real/source FPS.
- `generated_fps`: generated-frame FPS.
- `source_frames`: source frames observed in the latest interval.
- `generated_frames`: generated frames presented in the latest interval.
- `generated_per_source`: `generated_frames / source_frames` for the interval, or `0` when no source frame was observed.
- `source_frames_total` / `generated_frames_total`: swapchain-context lifetime totals.

These interval fields are intended for GameNative's pacing/thermal/clock coordinator. They must not be inferred from the selected multiplier because Adaptive Frame Generation can emit a fractional and time-varying number of generated frames.

The present hook only queues a snapshot. Filesystem writes remain on the background worker; `vkQueuePresentKHR` must not perform `stats.txt` or diagnostics file I/O.
