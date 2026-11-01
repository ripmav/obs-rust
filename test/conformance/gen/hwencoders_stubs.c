/*
 * Conformance harness (area hwencoders, spec S-050): link stubs for graphics / libva symbols that are
 * referenced by code the vectors never execute (texture encode paths). Deliberately does not include
 * any OBS header. Test tooling, not part of OBS.
 */
#include <stddef.h>
void gs_texture_destroy(void *t) { (void)t; }
void *gs_texture_create_from_dmabuf(void) { return NULL; }
void gs_copy_texture(void *a, void *b) { (void)a, (void)b; }
void gs_flush(void) {}
void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}
int vaExportSurfaceHandle(void *a, unsigned b, unsigned c, unsigned d, void *e) { (void)a, (void)b, (void)c, (void)d, (void)e; return -1; }
const char *vaErrorStr(int e) { (void)e; return "vaerr"; }
void *os_opendir(const char *p) { (void)p; return NULL; }
void *os_readdir(void *d) { (void)d; return NULL; }
void os_closedir(void *d) { (void)d; }
