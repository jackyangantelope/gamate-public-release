#include "hstx.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "jack_wave1_audio.h"
#include "jack_wave1_runtime.h"
#include "jack_pcm_ring.h"
#include <stdio.h>

static uint16_t framebuffer[320 * 240] __attribute__((aligned(4)));
static jack_pcm_ring_t pcm;
static _Atomic bool producer_started, muted;
static bool consumer_started;
static int audio_frame_counter;
static uint32_t submitted, dropped, nonzero, max_abs, wait_timeouts;
static _Atomic uint32_t encoded;
static uint32_t last_submit, last_drop, last_nonzero, last_encoded, last_under;
static uint32_t report_start;
volatile bool HSTX_vblank;
extern uint32_t __StackOneBottom[], __StackOneTop[];

uint8_t *hstx_getframebuffer(void) { return (uint8_t *)framebuffer; }
uint16_t *hstx_getlineFromFramebuffer(int line) { return framebuffer + line * 320; }
uint32_t hstx_getframecounter(void) { return video_frame_count; }
void hstx_waitForVSync(void)
{
    const uint32_t frame = video_frame_count;
    while (frame == video_frame_count) tight_loop_contents();
}

static void __not_in_flash_func(scanline)(uint32_t v, uint32_t active, uint32_t *out)
{
    (void)active;
    const int line = (int)v - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    if (line < 0 || line >= 480) return;
    const uint16_t *src = framebuffer + (line >> 1) * 320;
    for (unsigned x = 0; x < 320; ++x) {
        const uint32_t pixel = src[x] & 0x7fffu;
        out[x] = pixel | (pixel << 16);
    }
}

static void __not_in_flash_func(audio_pump)(void)
{
    if (!atomic_load_explicit(&producer_started, memory_order_acquire)) return;
    if (!consumer_started) {
        if (jack_pcm_level(&pcm) < 1024) return;
        consumer_started = true;
    }
    for (unsigned packet_index = 0; packet_index < 8; ++packet_index) {
        if (hstx_di_queue_get_level() >= 96 || jack_pcm_level(&pcm) < 4) return;
        audio_sample_t samples[4];
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t packed;
            if (!jack_pcm_pop(&pcm, &packed)) return;
            samples[i].left = (int16_t)packed;
            samples[i].right = (int16_t)(packed >> 16);
            if (atomic_load_explicit(&muted, memory_order_relaxed))
                samples[i].left = samples[i].right = 0;
        }
        hstx_packet_t packet;
        hstx_data_island_t island;
        const int next = hstx_packet_set_audio_samples_cs(
            &packet, samples, 4, audio_frame_counter);
        hstx_encode_data_island(&island, &packet, false, true);
        if (hstx_di_queue_push(&island)) {
            audio_frame_counter = next;
            atomic_fetch_add_explicit(&encoded, 4, memory_order_relaxed);
        }
    }
}

void hstx_init(bool dvi_only)
{
    video_output_set_dvi_mode(dvi_only);
    hstx_di_queue_init();
    video_output_init(640, 480);
    pico_hdmi_set_audio_sample_rate(44100);
    video_output_set_scanline_callback(scanline);
    video_output_set_background_task(audio_pump);
    multicore_launch_core1_with_stack(video_output_core1_run, __StackOneBottom,
        (uintptr_t)__StackOneTop - (uintptr_t)__StackOneBottom);
    printf("GAMATE_HDMI ready rate=44100 channels=2 bits=16\n");
}

bool jack_wave1_audio_begin(unsigned rate)
{
    if (rate != 44100) return false;
    last_under = hstx_di_queue_get_underrun_count();
    report_start = time_us_32();
    atomic_store_explicit(&producer_started, true, memory_order_release);
    return true;
}

size_t jack_wave1_audio_submit(const int16_t *samples, size_t frames)
{
    for (size_t i = 0; i < frames; ++i) {
        const int left = samples[i * 2], right = samples[i * 2 + 1];
        ++submitted;
        if (left || right) ++nonzero;
        unsigned peak = (unsigned)(left < 0 ? -left : left);
        const unsigned right_peak = (unsigned)(right < 0 ? -right : right);
        if (right_peak > peak) peak = right_peak;
        if (peak > max_abs) max_abs = peak;
        const uint32_t packed = (uint16_t)left | ((uint32_t)(uint16_t)right << 16);
        if (!jack_pcm_push(&pcm, packed)) ++dropped;
    }
    return frames;
}

bool jack_wave1_audio_wait(void)
{
    bool waited = false;
    const uint32_t start = time_us_32();
    uint32_t usb_at = start;
    while (jack_pcm_level(&pcm) + hstx_di_queue_get_level() * 4u > 1470u) {
        waited = true;
        const uint32_t now = time_us_32();
        if (now - start > 50000u) { ++wait_timeouts; break; }
        if (now - usb_at >= 1000u) { jack_wave1_usb_service(); usb_at = now; }
        tight_loop_contents();
    }
    return waited;
}

void jack_wave1_audio_mute(bool value)
{
    atomic_store_explicit(&muted, value, memory_order_relaxed);
}

void jack_wave1_audio_report(void)
{
    const uint32_t now = time_us_32();
    if (now - report_start < 5000000u) return;
    const uint32_t under = hstx_di_queue_get_underrun_count();
    const uint32_t sent = atomic_load_explicit(&encoded, memory_order_relaxed);
    printf("GAMATE_AUDIO elapsed_us=%lu generated=%lu encoded=%lu nonzero=%lu peak=%lu dropped=%lu underrun_packets=%lu queued_pcm=%lu queued_di=%lu wait_timeouts=%lu resync=%d\n",
        (unsigned long)(now - report_start),
        (unsigned long)(submitted - last_submit),
        (unsigned long)(sent - last_encoded),
        (unsigned long)(nonzero - last_nonzero),
        (unsigned long)max_abs, (unsigned long)(dropped - last_drop),
        (unsigned long)(under - last_under),
        (unsigned long)jack_pcm_level(&pcm),
        (unsigned long)hstx_di_queue_get_level(),
        (unsigned long)wait_timeouts, get_video_output_resync_count());
    last_submit = submitted;
    last_encoded = sent;
    last_nonzero = nonzero;
    last_drop = dropped;
    last_under = under;
    max_abs = 0;
    report_start = now;
}
