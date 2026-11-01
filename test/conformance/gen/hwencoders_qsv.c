/*
 * Conformance harness (area hwencoders, spec S-050): wrapper for plugins/obs-qsv11/obs-qsv11.c
 * (create gates / texture fallbacks, header assembly, timestamp conversion, packet post-processing,
 * ROI forwarding) and obs-qsv11-plugin-main.c (module load / registration). The QuickSync library
 * (qsv_encoder_*) and the adapter probe are recording / scripted fakes; the oneVPL HEADERS are the
 * system ones (/usr/include/vpl). Settings translation is specified by S-025 and not repeated.
 * Test tooling, not part of OBS.
 */
#include "hwencoders_api.h"
#include "../../../plugins/obs-qsv11/obs-qsv11.c"
/* keep obs_module_text from hwencoders_host.c (identity) */
#include <obs-module.h>
#undef OBS_MODULE_USE_DEFAULT_LOCALE
#define OBS_MODULE_USE_DEFAULT_LOCALE(a, b)
#include "../../../plugins/obs-qsv11/obs-qsv11-plugin-main.c"
#include <stdio.h>
#include "script.h"

struct adapter_info adapters[MAX_ADAPTERS];
size_t adapter_count;
size_t adapter_index;
static struct adapter_info g_scripted[MAX_ADAPTERS];
static size_t g_scripted_n;
static int g_ovi_adapter;
static int g_cpu_platform;
static int g_open_ok = 1;
static int g_ctx_dummy;
static uint8_t g_vps[64], g_sps[64], g_pps[64];
static uint16_t g_nvps, g_nsps, g_npps;
static mfxBitstream g_bs;
static uint8_t g_bsdata[512];
static int g_bs_present;
static int g_enc_ret;

void check_adapters(struct adapter_info *a, size_t *n)
{
	printf("qsv.check_adapters\n");
	for (size_t i = 0; i < g_scripted_n; i++)
		a[i] = g_scripted[i];
	*n = g_scripted_n;
}
enum qsv_cpu_platform qsv_get_cpu_platform()
{
	return (enum qsv_cpu_platform)g_cpu_platform;
}
void obs_get_video_info_adapter(void) {}
bool obs_get_video_info(struct obs_video_info *ovi)
{
	memset(ovi, 0, sizeof(*ovi));
	ovi->adapter = g_ovi_adapter;
	return true;
}
os_performance_token_t *os_request_high_performance(const char *r)
{
	printf("os.request_high_performance \"%s\"\n", r);
	return (os_performance_token_t *)&g_ctx_dummy;
}
void os_end_high_performance(os_performance_token_t *t)
{
	if (t)
		printf("os.end_high_performance\n");
}
qsv_t *qsv_encoder_open(qsv_param_t *p, enum qsv_codec codec, bool tex)
{
	(void)p;
	printf("qsv.encoder_open codec=%d tex=%d -> %s\n", (int)codec, tex, g_open_ok ? "ok" : "NULL");
	return g_open_ok ? (qsv_t *)&g_ctx_dummy : NULL;
}
int qsv_encoder_close(qsv_t *c)
{
	(void)c;
	printf("qsv.encoder_close\n");
	return 0;
}
void qsv_encoder_version(unsigned short *a, unsigned short *b)
{
	*a = 2;
	*b = 9;
}
int qsv_encoder_headers(qsv_t *c, uint8_t **sps, uint8_t **pps, uint16_t *nsps, uint16_t *npps)
{
	(void)c;
	*sps = g_sps; *pps = g_pps; *nsps = g_nsps; *npps = g_npps;
	return 0;
}
int qsv_hevc_encoder_headers(qsv_t *c, uint8_t **vps, uint8_t **sps, uint8_t **pps, uint16_t *nvps, uint16_t *nsps,
			     uint16_t *npps)
{
	(void)c;
	*vps = g_vps; *sps = g_sps; *pps = g_pps; *nvps = g_nvps; *nsps = g_nsps; *npps = g_npps;
	return 0;
}
int qsv_encoder_reconfig(qsv_t *c, qsv_param_t *p)
{
	(void)c, (void)p;
	return 1;
}
void qsv_encoder_add_roi(qsv_t *c, const struct obs_encoder_roi *r)
{
	(void)c;
	printf("qsv.add_roi left=%u top=%u right=%u bottom=%u prio=%g\n", r->left, r->top, r->right, r->bottom, (double)r->priority);
}
void qsv_encoder_clear_roi(qsv_t *c)
{
	(void)c;
	printf("qsv.clear_roi\n");
}
int qsv_encoder_encode(qsv_t *c, uint64_t ts, uint8_t *y, uint8_t *uv, uint32_t sy, uint32_t suv, mfxBitstream **pBS)
{
	(void)c, (void)y, (void)uv, (void)sy, (void)suv;
	printf("qsv.encode ts=%llu\n", (unsigned long long)ts);
	*pBS = g_bs_present ? &g_bs : NULL;
	return g_enc_ret;
}
int qsv_encoder_encode_tex(qsv_t *c, uint64_t ts, void *tex, uint64_t lk, uint64_t *nk, mfxBitstream **pBS)
{
	(void)c, (void)tex, (void)lk, (void)nk;
	printf("qsv.encode_tex ts=%llu\n", (unsigned long long)ts);
	*pBS = g_bs_present ? &g_bs : NULL;
	return g_enc_ret;
}

/* ---- entry points ---- */
void hw_q_adapter(int idx, int intel, int dgpu, int av1, int hevc)
{
	g_scripted[idx].is_intel = intel; g_scripted[idx].is_dgpu = dgpu;
	g_scripted[idx].supports_av1 = av1; g_scripted[idx].supports_hevc = hevc;
	if ((size_t)idx >= g_scripted_n) g_scripted_n = idx + 1;
	adapters[idx] = g_scripted[idx];
}
void hw_q_set(const char *k, int v)
{
	if (!strcmp(k, "ovi_adapter")) g_ovi_adapter = v;
	else if (!strcmp(k, "open_ok")) g_open_ok = v;
	else if (!strcmp(k, "enc_ret")) g_enc_ret = v;
	else if (!strcmp(k, "bs_present")) g_bs_present = v;
	else if (!strcmp(k, "cpu_platform")) g_cpu_platform = v;
}
void hw_q_headers(const uint8_t *vps, size_t nv, const uint8_t *sps, size_t ns, const uint8_t *pps, size_t np)
{
	memcpy(g_vps, vps, nv); g_nvps = (uint16_t)nv;
	memcpy(g_sps, sps, ns); g_nsps = (uint16_t)ns;
	memcpy(g_pps, pps, np); g_npps = (uint16_t)np;
}
void hw_q_bitstream(const uint8_t *d, size_t n, unsigned frame_type, long long ts, long long dts)
{
	memcpy(g_bsdata, d, n);
	g_bs.Data = g_bsdata; g_bs.DataOffset = 0; g_bs.DataLength = (mfxU32)n; g_bs.MaxLength = sizeof(g_bsdata);
	g_bs.FrameType = (mfxU16)frame_type; g_bs.TimeStamp = (mfxU64)ts; g_bs.DecodeTimeStamp = (mfxI64)dts;
}
void *hw_q_create(int codec, bool tex, obs_data_t *s)
{
	/* codec: 0 h264 (v2 texture encoder), 1 av1, 2 hevc */
	static const char *fb[] = {"obs_qsv11_soft_v2", "obs_qsv11_av1_soft", "obs_qsv11_hevc_soft"};
	static const enum qsv_codec cd[] = {QSV_CODEC_AVC, QSV_CODEC_AV1, QSV_CODEC_HEVC};
	if (tex) return obs_qsv_create_tex(cd[codec], s, (obs_encoder_t *)&g_hw, fb[codec]);
	return obs_qsv_create(cd[codec], s, (obs_encoder_t *)&g_hw, false);
}
void hw_q_destroy(void *e) { obs_qsv_destroy(e); }
bool hw_q_encode(void *e, long long pts, int tex, struct encoder_packet *p, bool *got)
{
	struct encoder_frame f;
	memset(&f, 0, sizeof(f));
	f.pts = pts;
	if (tex == 2) {
		struct encoder_texture t = {0};
		uint64_t nk = 0;
		bool r = obs_qsv_encode_tex(e, &t, pts, 77, &nk, p, got);
		printf("bad texture: next_key=%llu\n", (unsigned long long)nk);
		return r;
	}
	if (tex) {
		struct encoder_texture t = {0};
		uint64_t nk = 0;
		t.tex[0] = (gs_texture_t *)&g_ctx_dummy;
		t.tex[1] = (gs_texture_t *)&g_ctx_dummy;
		return obs_qsv_encode_tex(e, &t, pts, 0, &nk, p, got);
	}
	return obs_qsv_encode(e, &f, p, got);
}
void hw_q_extra(void *e)
{
	uint8_t *p = NULL; size_t n = 0;
	bool r = obs_qsv_extra_data(e, &p, &n);
	printf("extra_data -> %d ", r);
	if (r) script_print_hex(p, n);
	uint8_t *s = NULL; size_t sn = 0;
	r = obs_qsv_sei(e, &s, &sn);
	printf("\nsei -> %d ", r);
	if (r) script_print_hex(s, sn);
	printf("\n");
}
void hw_q_ts(long long ts, int is_obs)
{
	struct video_output_info voi = {.fps_num = g_hw.fps_num, .fps_den = g_hw.fps_den};
	if (is_obs) printf("ts_obs_to_mfx(%lld) = %llu\n", ts, (unsigned long long)ts_obs_to_mfx(ts, &voi));
	else printf("ts_mfx_to_obs(%lld) = %lld\n", ts, (long long)ts_mfx_to_obs(ts, &voi));
}
void hw_q_load(void)
{
	g_hw_nregistered = 0;
	obs_module_load();
}
int hw_q_priority_bits(void) { return 0; }
