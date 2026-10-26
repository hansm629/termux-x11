#pragma once
#include <stdbool.h>

/* NDK-side optional GPU warm-up controller for any EGL/GLES-capable GPU.
 * This never changes Vulkan WSI, CPU affinity, device power policy or the Android scheduler.
 * Modes/foreground/thermal arrive via JNI; frame notifications arrive from the renderer. */
void gpuPerformanceSetMode(int mode);
void gpuPerformanceSetForeground(bool foreground);
void gpuPerformanceSetThermal(int status);
void gpuPerformanceNoteFrame(void);
