#ifndef GL_VK_PROVISION_H
#define GL_VK_PROVISION_H

#include <windows.h>

/* Logging callback (the agent's agent_log); without one nothing is logged. */
typedef void (*GlVkLogFn)(const char *fmt, ...);
void gl_vk_set_log(GlVkLogFn fn);

BOOL gpu_prefers_system_opengl(void);
BOOL gl_vk_provision_runtime(const wchar_t *dir, const wchar_t *native_dir,
                             const wchar_t *sys, BOOL *native_runtime);
BOOL nvidia_runtime_provision(const wchar_t *native_dir);
BOOL nvidia_opencl_copy_target(const wchar_t *path, wchar_t target[MAX_PATH]);
BOOL nvidia_opencl_commit_copy(const wchar_t *source, const wchar_t *target);

#endif
