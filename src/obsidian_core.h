#ifndef OBSIDIAN_CORE_H
#define OBSIDIAN_CORE_H

#include <stddef.h>

#if defined(_WIN32) && defined(OBSIDIAN_BUILD_SHARED)
#  define OBSIDIAN_API __declspec(dllexport)
#elif defined(_WIN32) && defined(OBSIDIAN_USE_SHARED)
#  define OBSIDIAN_API __declspec(dllimport)
#else
#  define OBSIDIAN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Compile Obsidronium source to a C translation unit (free result with obsidian_free). */
OBSIDIAN_API char *obsidian_compile_c(const char *source, const char *filename,
                                      char **error_message);

/* Returns 1 for valid Obsidronium syntax, 0 otherwise. */
OBSIDIAN_API int obsidian_check(const char *source, const char *filename,
                                char **error_message);

/* In-process Obsidronium project runtime enabled via OBSIDIAN_EMBEDDED_RUNTIME. */
OBSIDIAN_API void *obsidian_project_open(const char *root_path, char **error_message);
OBSIDIAN_API char *obsidian_project_step(void *project, double dt);
OBSIDIAN_API char *obsidian_project_exec(void *project, const char *code);
OBSIDIAN_API void obsidian_project_set_viewport(void *project, int width, int height);
OBSIDIAN_API void obsidian_project_touch(void *project, int id, double x, double y, int down);
OBSIDIAN_API void obsidian_project_close(void *project);

OBSIDIAN_API const char *obsidian_version(void);
OBSIDIAN_API void obsidian_free(void *memory);

#ifdef __cplusplus
}
#endif
#endif
