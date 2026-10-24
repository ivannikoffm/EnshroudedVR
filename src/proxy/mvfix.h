// EnshroudedVR S12 - per-eye DLSS motion-vector fix (host side). Shared by the mod (xr.cpp/camhook.cpp) and test_mvfix.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

struct MvFixStats { uint64_t calls, applied, calibs; int conv; float best_med, second_med; bool calibrated; char top[160]; };

// dev: game's VkDevice; gdpa: device proc addr; mp: memory props; storage_align: minStorageBufferOffsetAlignment
bool mvfix_init(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, const VkPhysicalDeviceMemoryProperties& mp, VkDeviceSize storage_align);
// Called at every DLSS evaluate, BEFORE the original call, on the game's recording command buffer.
//   cam32: render camera struct of THIS frame (r14+0x1f10: view 4x4 row-vector, proj 4x4), 32 floats
//   evalp: NGX eval params (depth res at +0x18, MV res at +0x20)
//   apply: 1 = write corrected MVs and return a replacement MV resource for DLSS (only once calibrated), 0 = observe/calibrate
// returns pointer to a 0x38-byte NVSDK_NGX_Resource_VK for the corrected MVs, or nullptr (keep the game's)
void* mvfix_pre(VkCommandBuffer cmd, void* evalp, const float* cam32, int apply);
void mvfix_stats(MvFixStats* s);
void mvfix_reset_history();   // cameras ring + calibration accumulators (after a mode switch)
