#include <obs-nix.h>
#include <obs-nix-x11.h>
/* No-op hotkeys vtable (the real X11 vtable needs libX11; the harness
 * registers no hotkeys, so every function is a harmless no-op). */
static bool nix_hk_init(struct obs_core_hotkeys *h) { (void)h; return true; }
static void nix_hk_free(struct obs_core_hotkeys *h) { (void)h; }
static bool nix_hk_is_pressed(obs_hotkeys_platform_t *c, obs_key_t k) { (void)c; (void)k; return false; }
static void nix_hk_key_to_str(obs_key_t k, struct dstr *d) { (void)k; (void)d; }
static obs_key_t nix_hk_from_vk(int s) { (void)s; return 0; }
static int nix_hk_to_vk(obs_key_t k) { (void)k; return -1; }
static const struct obs_nix_hotkeys_vtable nix_hk_stub_vtable = {nix_hk_init, nix_hk_free, nix_hk_is_pressed,
															   nix_hk_key_to_str, nix_hk_from_vk, nix_hk_to_vk};
const struct obs_nix_hotkeys_vtable *obs_nix_x11_get_hotkeys_vtable(void) { return &nix_hk_stub_vtable; }
void obs_nix_x11_log_info(void) {}
