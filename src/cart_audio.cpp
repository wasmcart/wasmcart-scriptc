/* cart_audio.cpp - scriptc-game's sg_audio, with SDL replaced by the cart
 * ring buffer.
 *
 * The graph engine underneath is webaudio-node's, unchanged: the same
 * createAudioGraph / createNode / connectNodes / processGraph C ABI the
 * native shim drives. web/audio/context.ts is untouched.
 *
 * TWO THINGS DIFFER from shim/sg_audio.cpp, and both make this file
 * SMALLER rather than larger:
 *
 * 1. No SDL device and no audio thread. A cart renders audio inline, from
 *    wc_render, into the ring buffer the host drains after each frame.
 *
 * 2. No command ring. The native shim marshals every graph mutation through
 *    a lock-free queue because SDL calls its callback on another thread and
 *    the graph is not thread-safe. Here the game and the renderer are the
 *    same thread, so each call applies directly. That removes the entire
 *    CMD_* apply() switch, which is the bulk of the native file.
 *
 * The host reads INTERLEAVED STEREO FLOAT32 frames and a write cursor
 * counted in FRAMES, not samples or bytes. Getting that unit wrong yields
 * audio at half or double speed, which sounds like a resampling bug rather
 * than an indexing one.
 */
#include <stdint.h>
#include <string.h>

#define SG_OK        0
#define SG_EAUDIO   -6
#define SG_ERANGE   -4

/* webaudio-node's C ABI (built to wasm by build-audio.sh). */
extern "C" {
int  createAudioGraph(int sample_rate, int channels, int buffer_size, bool is_realtime);
void destroyAudioGraph(int graph_id);
int  createNode(int graph_id, const char* type_str);
void connectNodes(int graph_id, int source_id, int dest_id, int output_idx, int input_idx);
void disconnectNodes(int graph_id, int source_id, int dest_id);
void startNode(int graph_id, int node_id, double when);
void stopNode(int graph_id, int node_id, double when);
void setNodeParameter(int graph_id, int node_id, int param_id, float value);
void scheduleParamEvent(int graph_id, int node_id, int param_id, int kind,
                        float value, double time, float time_constant);
void processGraph(int graph_id, float* output, int frame_count);
void setGraphCurrentTime(int graph_id, double time);
void registerBuffer(int graph_id, int buffer_id, float* buffer_data,
                    int buffer_frames, int buffer_channels);
void setNodeBufferId(int graph_id, int node_id, int buffer_id);
void setWaveShaperCurve(int graph_id, int node_id, float* curve_data, int curve_length);
}

extern "C" void sg_mail_set(const char* s);   /* gl_support.c / cart_core.cpp */

/* ---- the cart's audio ring ------------------------------------------
 *
 * Owned here rather than by the ABI shell so wc_get_info can publish the
 * pointers without knowing how the graph works. Capacity is in FRAMES. */
#define AUDIO_CAP     8192            /* frames; ~170ms at 48kHz */
#define AUDIO_CHANS   2

float    g_audio_ring[AUDIO_CAP * AUDIO_CHANS];
uint32_t g_audio_write;               /* frame cursor, wraps at AUDIO_CAP */

static int    g_graph = -1;
static int    g_rate  = 48000;
static double g_time;                 /* graph clock, in seconds */
static int    g_next_buffer_id = 1;

/* Scratch for one render slice. Rendering straight into the ring would
 * have to handle the wrap mid-call; a slice plus a copy is simpler and the
 * copy is trivial next to the graph work. */
#define SLICE 128
static float g_slice[SLICE * AUDIO_CHANS];

/* ---- lifecycle ------------------------------------------------------ */

extern "C" int32_t sg_audio_init(uint32_t sample_rate, uint32_t buffer_frames) {
    (void)buffer_frames;              /* the cart's slice size is fixed */
    if (g_graph >= 0) return SG_OK;   /* idempotent, as in the native shim */

    if (sample_rate > 0) g_rate = (int)sample_rate;
    g_graph = createAudioGraph(g_rate, AUDIO_CHANS, SLICE, true);
    if (g_graph < 0) { sg_mail_set("audio graph creation failed"); return SG_EAUDIO; }
    return SG_OK;
}

extern "C" void sg_audio_quit(int32_t unused) {
    (void)unused;
    if (g_graph >= 0) { destroyAudioGraph(g_graph); g_graph = -1; }
}

extern "C" uint32_t sg_audio_rate(int32_t unused)     { (void)unused; return (uint32_t)g_rate; }
extern "C" uint32_t sg_audio_channels(int32_t unused) { (void)unused; return AUDIO_CHANS; }
extern "C" double   sg_audio_time(int32_t unused)     { (void)unused; return g_time; }
extern "C" uint32_t sg_audio_dropped(int32_t unused)  { (void)unused; return 0; }

/* No device to suspend: a cart stops producing when it stops calling
 * sg_cart_audio_pump. Reported as success so a game that suspends on blur
 * is not told the call failed. */
extern "C" int32_t sg_audio_suspend(uint32_t suspend) { (void)suspend; return SG_OK; }

extern "C" int32_t sg_audio_graph_id(int32_t unused) { (void)unused; return g_graph; }

extern "C" int32_t sg_audio_next_buffer_id(int32_t unused) {
    (void)unused;
    return g_next_buffer_id++;
}

/* ---- graph mutation -------------------------------------------------
 *
 * Direct calls, no command ring: see the header comment. */

extern "C" int32_t sg_audio_create_node(const uint8_t* type, uint32_t len) {
    if (g_graph < 0) return SG_EAUDIO;
    char buf[64];
    if (len >= sizeof(buf)) { sg_mail_set("node type name too long"); return SG_ERANGE; }
    memcpy(buf, type, len);
    buf[len] = 0;
    return createNode(g_graph, buf);
}

extern "C" int32_t sg_audio_connect(uint32_t src, uint32_t dst, uint32_t oidx, uint32_t iidx) {
    if (g_graph < 0) return SG_EAUDIO;
    connectNodes(g_graph, (int)src, (int)dst, (int)oidx, (int)iidx);
    return SG_OK;
}

extern "C" int32_t sg_audio_disconnect(uint32_t src, uint32_t dst) {
    if (g_graph < 0) return SG_EAUDIO;
    disconnectNodes(g_graph, (int)src, (int)dst);
    return SG_OK;
}

extern "C" int32_t sg_audio_start(uint32_t node, double when) {
    if (g_graph < 0) return SG_EAUDIO;
    startNode(g_graph, (int)node, when);
    return SG_OK;
}

extern "C" int32_t sg_audio_stop(uint32_t node, double when) {
    if (g_graph < 0) return SG_EAUDIO;
    stopNode(g_graph, (int)node, when);
    return SG_OK;
}

extern "C" int32_t sg_audio_set_param(uint32_t node, uint32_t param, double value) {
    if (g_graph < 0) return SG_EAUDIO;
    setNodeParameter(g_graph, (int)node, (int)param, (float)value);
    return SG_OK;
}

/* Argument order matches the native shim exactly: (kind, VALUE, TIME,
 * timeConstant). The natural reading is (time, value), and swapping them
 * silently corrupts every automation curve. */
extern "C" int32_t sg_audio_schedule_param(uint32_t node, uint32_t param, uint32_t kind,
                                           double time, double value, double extra) {
    if (g_graph < 0) return SG_EAUDIO;
    scheduleParamEvent(g_graph, (int)node, (int)param, (int)kind,
                       (float)value, time, (float)extra);
    return SG_OK;
}

extern "C" int32_t sg_audio_register_buffer(const uint8_t* data, uint32_t len,
                                            uint32_t frames, uint32_t channels) {
    if (g_graph < 0) return SG_EAUDIO;
    size_t need = (size_t)frames * channels * sizeof(float);
    if (len < need) { sg_mail_set("sample buffer shorter than frames*channels*4"); return SG_ERANGE; }
    int id = g_next_buffer_id++;
    registerBuffer(g_graph, id, (float*)(void*)data, (int)frames, (int)channels);
    return id;   /* the ID, as the native shim does -- not SG_OK */
}

extern "C" int32_t sg_audio_set_node_buffer(uint32_t node, uint32_t buffer_id) {
    if (g_graph < 0) return SG_EAUDIO;
    setNodeBufferId(g_graph, (int)node, (int)buffer_id);
    return SG_OK;
}

extern "C" int32_t sg_audio_set_curve(uint32_t node, const uint8_t* data, uint32_t len) {
    if (g_graph < 0) return SG_EAUDIO;
    setWaveShaperCurve(g_graph, (int)node, (float*)(void*)data, (int)(len / sizeof(float)));
    return SG_OK;
}

/* Offline rendering writes a file, which a cart has no filesystem for. */
extern "C" int32_t sg_audio_render_offline(const uint8_t* path, uint32_t path_len,
                                           uint32_t frames) {
    (void)path; (void)path_len; (void)frames;
    sg_mail_set("offline audio rendering is unavailable in a cart");
    return SG_EAUDIO;
}

extern "C" int32_t sg_audio_init_offline(uint32_t sample_rate, uint32_t channels) {
    (void)sample_rate; (void)channels;
    sg_mail_set("offline audio rendering is unavailable in a cart");
    return SG_EAUDIO;
}

/* ---- the pump -------------------------------------------------------
 *
 * Called once per frame from wc_render. Renders `frames` stereo frames
 * through the graph and into the ring, advancing the write cursor the host
 * reads.
 *
 * The cursor counts FRAMES and wraps at AUDIO_CAP, matching what
 * CartHost._drainAudio expects. Publishing a sample count or a byte offset
 * here plays back at the wrong rate rather than failing. */
extern "C" void sg_cart_audio_pump(uint32_t frames) {
    if (g_graph < 0) return;

    uint32_t left = frames;
    while (left > 0) {
        uint32_t n = left < SLICE ? left : SLICE;

        setGraphCurrentTime(g_graph, g_time);
        memset(g_slice, 0, sizeof(float) * n * AUDIO_CHANS);
        processGraph(g_graph, g_slice, (int)n);

        for (uint32_t i = 0; i < n; i++) {
            uint32_t slot = ((g_audio_write + i) % AUDIO_CAP) * AUDIO_CHANS;
            g_audio_ring[slot]     = g_slice[i * AUDIO_CHANS];
            g_audio_ring[slot + 1] = g_slice[i * AUDIO_CHANS + 1];
        }

        g_audio_write = (g_audio_write + n) % AUDIO_CAP;
        g_time += (double)n / (double)g_rate;
        left -= n;
    }
}
