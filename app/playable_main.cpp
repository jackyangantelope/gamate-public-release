#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ff.h"
#include "hardware/psram.h"
#include "jack_wave1_audio.h"
#include "jack_wave1_runtime.h"
#include "jack_wave1_perf.h"

extern "C" {
#include "w65c02.h"
#include "emu2149.h"
}
#include "vdp.h"
#include "gamate_scaler.h"

extern "C" uint8_t Rd6502(uint16_t address);
extern "C" void Wr6502(uint16_t address, uint8_t value);

namespace {
constexpr size_t kMaxRomSize = 512u * 1024u;
constexpr uint32_t kAudioRate = 44100;
constexpr size_t kAudioFramesPerVideoFrame = kAudioRate / 60u;
constexpr uint32_t kCpuCyclesPerVideoFrame = 65536u;

uint8_t __uninitialized_psram("gamate_rom") g_rom[kMaxRomSize];
uint8_t g_ram[1024];
uint8_t g_bios[4096];
uint16_t g_lcd[GAMATE_SCREEN_WIDTH * GAMATE_SCREEN_HEIGHT];
int16_t g_audio[kAudioFramesPerVideoFrame * 2u];
w65c02_t g_cpu{};
uint64_t g_cpu_pins = 0;
PSG g_psg{};
size_t g_rom_size = 0;
uint32_t g_bank0 = 0;
uint32_t g_bank1 = 0x4000;
uint8_t g_protection = 0;
uint16_t g_buttons = 0;
uint32_t g_psg_writes = 0;
uint32_t g_psg_reads = 0;
uint32_t g_psg_active_volume_writes = 0;
uint32_t g_psg_report_start = 0;
size_t g_audio_frames_ready = 0;
uint32_t g_audio_cycles_elapsed = 0;
uint32_t g_next_segment_cycles = 32768;
uint32_t g_audio_segment_start = 0;
uint32_t g_audio_segment_position = 0;
bool g_audio_timing_active = false;
uint32_t g_psg_timed_flushes = 0;
#ifdef JACK_GAMATE_AUDIO_DIAGNOSTICS
uint32_t g_wave_changes[2]{};
uint32_t g_wave_nonzero[2]{};
int32_t g_wave_min[2] = {32767, 32767};
int32_t g_wave_max[2] = {-32768, -32768};
int16_t g_wave_previous[2]{};
#endif

uint8_t gamate_input_port(uint16_t buttons)
{
    uint8_t value = 0xff;
    if(buttons & JACK_BUTTON_UP) value ^= 0x01;
    if(buttons & JACK_BUTTON_DOWN) value ^= 0x02;
    if(buttons & JACK_BUTTON_LEFT) value ^= 0x04;
    if(buttons & JACK_BUTTON_RIGHT) value ^= 0x08;
    if(buttons & JACK_BUTTON_A) value ^= 0x10;
    if(buttons & JACK_BUTTON_B) value ^= 0x20;
    if(buttons & JACK_BUTTON_START) value ^= 0x40;
    if(buttons & JACK_BUTTON_SELECT) value ^= 0x80;
    return value;
}
void poll_frame_usb()
{
    g_buttons = static_cast<uint16_t>(g_buttons | jack_wave1_usb_poll_buttons());
}

bool load_rom(const char* path)
{
    FIL file{};
    if(f_open(&file, path, FA_READ) != FR_OK) return false;
    const FSIZE_t size = f_size(&file);
    if(size < 0x4000 || size > kMaxRomSize) {
        f_close(&file);
        return false;
    }
    std::memset(g_rom, 0xff, sizeof(g_rom));
    UINT read = 0;
    const bool ok = f_read(&file, g_rom, static_cast<UINT>(size), &read) == FR_OK &&
                    read == size;
    f_close(&file);
    if(ok) g_rom_size = static_cast<size_t>(size);
    return ok;
}

bool load_bios()
{
    FIL file{};
    if(f_open(&file, "/BIOS/GAMATE/gamate_bios.bin", FA_READ) != FR_OK)
        return false;
    const FSIZE_t size = f_size(&file);
    UINT read = 0;
    const bool ok = size == sizeof(g_bios) &&
                    f_read(&file, g_bios, sizeof(g_bios), &read) == FR_OK &&
                    read == sizeof(g_bios);
    f_close(&file);
    return ok;
}

uint32_t bank_offset(uint8_t bank)
{
    const uint32_t banks = static_cast<uint32_t>((g_rom_size + 0x3fff) / 0x4000);
    return (banks == 0 ? 0u : static_cast<uint32_t>(bank) % banks) * 0x4000u;
}

void render_frame()
{
    screen_update(g_lcd);
    static_assert(GAMATE_SCREEN_WIDTH == 160 && GAMATE_SCREEN_HEIGHT == 150);
    static_assert(JACK_WAVE1_SCREEN_WIDTH == 320);
    gamate_scale_frame(g_lcd, jack_wave1_framebuffer());
}

void render_audio_until(size_t target_frames)
{
    if(target_frames > kAudioFramesPerVideoFrame)
        target_frames = kAudioFramesPerVideoFrame;
    if(target_frames <= g_audio_frames_ready) return;
    const size_t frames = target_frames - g_audio_frames_ready;
    PSG_calc_stereo(&g_psg, g_audio + g_audio_frames_ready * 2u,
                    static_cast<int32_t>(frames * 2u));
    g_audio_frames_ready = target_frames;
}

void begin_audio_frame()
{
    g_audio_frames_ready = 0;
    g_audio_cycles_elapsed = 0;
}

void flush_audio_to_cpu_position()
{
    if(!g_audio_timing_active) return;
    const uint32_t cycle = g_audio_segment_start + g_audio_segment_position;
    const size_t target = static_cast<size_t>(
        (static_cast<uint64_t>(cycle) * kAudioFramesPerVideoFrame) /
        kCpuCyclesPerVideoFrame);
    render_audio_until(target);
    ++g_psg_timed_flushes;
}

void run_cpu_segment(uint32_t cycles)
{
    g_audio_segment_start = g_audio_cycles_elapsed;
    g_audio_segment_position = 0;
    g_audio_timing_active = true;
    for(uint32_t i = 0; i < cycles; ++i) {
        g_cpu_pins = w65c02_tick(&g_cpu, g_cpu_pins);
        if(g_cpu.brk_flags & W65C02_BRK_IRQ)
            g_cpu_pins &= ~W65C02_IRQ;
        const uint16_t address = W65C02_GET_ADDR(g_cpu_pins);
        g_audio_segment_position = i;
        if(g_cpu_pins & W65C02_RW) {
            W65C02_SET_DATA(g_cpu_pins, Rd6502(address));
        } else {
            Wr6502(address, W65C02_GET_DATA(g_cpu_pins));
        }
    }
    g_audio_timing_active = false;
    g_audio_cycles_elapsed += cycles;
}

void finish_audio_frame()
{
    render_audio_until(kAudioFramesPerVideoFrame);
#ifdef JACK_GAMATE_AUDIO_DIAGNOSTICS
    for(size_t frame = 0; frame < kAudioFramesPerVideoFrame; ++frame) {
        for(unsigned channel = 0; channel < 2; ++channel) {
            const int16_t value = g_audio[frame * 2u + channel];
            if(value != 0) ++g_wave_nonzero[channel];
            if(value != g_wave_previous[channel]) ++g_wave_changes[channel];
            if(value < g_wave_min[channel]) g_wave_min[channel] = value;
            if(value > g_wave_max[channel]) g_wave_max[channel] = value;
            g_wave_previous[channel] = value;
        }
    }
#endif
    jack_wave1_audio_submit(g_audio, kAudioFramesPerVideoFrame);
}

void report_psg()
{
    const uint32_t now = time_us_32();
    if(now - g_psg_report_start < 5000000u) return;
    g_psg_report_start = now;
    printf("GAMATE_PSG writes=%lu reads=%lu active_volume_writes=%lu timed_flushes=%lu "
           "mixer=%02x volume=%02x,%02x,%02x tone=%02x%02x,%02x%02x,%02x%02x\n",
           (unsigned long)g_psg_writes, (unsigned long)g_psg_reads,
           (unsigned long)g_psg_active_volume_writes,
           (unsigned long)g_psg_timed_flushes, g_psg.reg[7],
           g_psg.reg[8], g_psg.reg[9], g_psg.reg[10],
           g_psg.reg[1], g_psg.reg[0], g_psg.reg[3], g_psg.reg[2],
           g_psg.reg[5], g_psg.reg[4]);
#ifdef JACK_GAMATE_AUDIO_DIAGNOSTICS
    printf("GAMATE_WAVE nonzero=%lu,%lu changes=%lu,%lu range=%ld:%ld,%ld:%ld "
           "tone_mask=%lu,%lu,%lu noise_mask=%lu,%lu,%lu\n",
           (unsigned long)g_wave_nonzero[0], (unsigned long)g_wave_nonzero[1],
           (unsigned long)g_wave_changes[0], (unsigned long)g_wave_changes[1],
           (long)g_wave_min[0], (long)g_wave_max[0],
           (long)g_wave_min[1], (long)g_wave_max[1],
           (unsigned long)g_psg.tmask[0], (unsigned long)g_psg.tmask[1],
           (unsigned long)g_psg.tmask[2], (unsigned long)g_psg.nmask[0],
           (unsigned long)g_psg.nmask[1], (unsigned long)g_psg.nmask[2]);
    for(unsigned channel = 0; channel < 2; ++channel) {
        g_wave_nonzero[channel] = g_wave_changes[channel] = 0;
        g_wave_min[channel] = 32767;
        g_wave_max[channel] = -32768;
    }
#endif
}

void __attribute__((noinline)) run_psg_selftest()
{
    // Upstream PSG_reset leaves tmask/nmask intact. Probe a copy so the
    // mixer=0x3e test cannot leave game channels B/C disabled after reset.
    for(unsigned channel = 0; channel < 3; ++channel) {
        PSG probe = g_psg;
        int16_t samples[512];
        PSG_writeReg(&probe, channel * 2u, 32);
        PSG_writeReg(&probe, channel * 2u + 1u, 0);
        PSG_writeReg(&probe, 7, 0x3fu ^ (1u << channel));
        PSG_writeReg(&probe, channel + 8u, 0x0f);
        PSG_calc_stereo(&probe, samples, 512);
        uint32_t nonzero = 0, changes = 0;
        int32_t low = 32767, high = -32768;
        for(size_t frame = 0; frame < 256; ++frame) {
            const int32_t value = samples[frame * 2u];
            if(value != 0) ++nonzero;
            if(frame && value != samples[(frame - 1u) * 2u]) ++changes;
            if(value < low) low = value;
            if(value > high) high = value;
        }
        printf("GAMATE_PSG_SELFTEST channel=%u nonzero=%lu changes=%lu range=%ld:%ld pass=%u\n",
               channel, (unsigned long)nonzero, (unsigned long)changes,
               (long)low, (long)high, (unsigned)(changes > 0 && high > low));
    }
}

void play_output_test()
{
    // Keep the audible HDMI check isolated from the game's PSG as well.
    PSG probe = g_psg;
    PSG_writeReg(&probe, 7, 0x3e);
    printf("GAMATE_AUDIO_TEST begin output=HDMI channels=both tones=660,880Hz\n");
    for(unsigned frame = 0; frame < 66; ++frame) {
        const uint32_t video_frame = jack_wave1_frame_counter();
        const bool first = frame >= 12 && frame < 30;
        const bool second = frame >= 36 && frame < 54;
        PSG_writeReg(&probe, 0, second ? 79 : 105);
        PSG_writeReg(&probe, 1, 0);
        PSG_writeReg(&probe, 8, first || second ? 15 : 0);
        PSG_calc_stereo(&probe, g_audio,
                        static_cast<int32_t>(kAudioFramesPerVideoFrame * 2u));
        jack_wave1_audio_submit(g_audio, kAudioFramesPerVideoFrame);
        jack_wave1_audio_wait();
        jack_wave1_wait_frame_boundary(video_frame);
        jack_wave1_usb_service();
    }
    printf("GAMATE_AUDIO_TEST end\n");
}
}

extern "C" uint8_t Rd6502(uint16_t address)
{
    if(address <= 0x1fff) return g_ram[address & 1023u];
    if(address >= 0x4000 && address <= 0x43ff) {
        ++g_psg_reads;
        return PSG_readReg(&g_psg, address & 0x0f);
    }
    if(address >= 0x6000 && address <= 0x9fff) {
        if(g_protection < 8) return static_cast<uint8_t>(((0x47u >> (7u - g_protection++)) & 1u) << 1u);
        return g_rom[(g_bank0 + address - 0x6000u) % g_rom_size];
    }
    if(address >= 0xa000 && address <= 0xdfff)
        return g_rom[(g_bank1 + address - 0xa000u) % g_rom_size];
    if(address >= 0x5000 && address <= 0x53ff) return vdp_read();
    if(address >= 0x5a00 && address <= 0x5aff) return 0x5b;
    if(address == 0x4400) return gamate_input_port(g_buttons);
    if(address == 0x4800) return 0;
    if(address >= 0xe000) return g_bios[address & 4095u];
    return 0xff;
}

extern "C" void Wr6502(uint16_t address, uint8_t value)
{
    if(address <= 0x1fff) {
        g_ram[address & 1023u] = value;
    } else if(address >= 0x4000 && address <= 0x43ff) {
        const uint8_t reg = static_cast<uint8_t>(address & 0x0f);
        flush_audio_to_cpu_position();
        ++g_psg_writes;
        if(reg >= 8 && reg <= 10 && (value & 0x1f) != 0)
            ++g_psg_active_volume_writes;
        PSG_writeReg(&g_psg, reg, value);
    } else if(address >= 0x5000 && address <= 0x53ff) {
        vdp_write(address, value);
    } else if(address == 0x8000) {
        g_bank0 = bank_offset(value);
    } else if(address == 0xc000) {
        g_bank1 = bank_offset(value);
    }
}

int main()
{
    if(!jack_wave1_init("Gamate")) for(;;) tight_loop_contents();
    if(!jack_wave1_mount_sd()) {
        jack_wave1_show_error("GAMATE", "SD mount failed");
        for(;;) tight_loop_contents();
    }
    if(!psram_is_available() || psram_get_size() < 8u * 1024u * 1024u) {
        jack_wave1_show_error("GAMATE", "Fruit Jam 8MiB PSRAM not detected");
        for(;;) tight_loop_contents();
    }
    if(!load_bios()) {
        jack_wave1_show_error("GAMATE", "Need 4096-byte /BIOS/GAMATE/gamate_bios.bin");
        for(;;) tight_loop_contents();
    }

    char rom_path[JACK_WAVE1_PATH_MAX]{};
    if(!jack_wave1_select_file("SELECT GAMATE ROM", "/roms/GAMATE", ".bin .rom",
                               rom_path, sizeof(rom_path)) || !load_rom(rom_path)) {
        jack_wave1_show_error("GAMATE", "Need 16K-512K ROM image");
        for(;;) tight_loop_contents();
    }

    printf("GAMATE_BOOT version=0.1.0-public-candidate rom=%s bytes=%lu audio_rate=%lu audio_frames=%lu\n",
           rom_path, (unsigned long)g_rom_size, (unsigned long)kAudioRate,
           (unsigned long)kAudioFramesPerVideoFrame);
#ifdef JACK_GAMATE_AUDIO_DIAGNOSTICS
    printf("GAMATE_RENDER diagnostics=1 perf_interval_us=%u\n",
           JACK_WAVE1_PERF_INTERVAL_US);
#else
    printf("GAMATE_RENDER diagnostics=0 perf_interval_us=%u\n",
           JACK_WAVE1_PERF_INTERVAL_US);
#endif
    PSG_init(&g_psg, 4433000u / 4u, kAudioRate);
    PSG_setVolumeMode(&g_psg, 2);
    PSG_set_quality(&g_psg, 0);
    PSG_setFlags(&g_psg, 0); // Send all three voices to both HDMI channels.
    PSG_reset(&g_psg);
    run_psg_selftest();
    if(!jack_wave1_audio_begin(kAudioRate)) {
        jack_wave1_show_error("GAMATE", "HDMI audio init failed");
        for(;;) tight_loop_contents();
    }
    play_output_test();
    std::memset(g_ram, 0xff, sizeof(g_ram));
    const w65c02_desc_t cpu_desc{};
    g_cpu_pins = w65c02_init(&g_cpu, &cpu_desc);

    jack_wave1_clear(jack_wave1_rgb555(1, 2, 2));
    JackWave1Perf perf;
    perf.begin();
#ifdef JACK_GAMATE_AUDIO_DIAGNOSTICS
    uint16_t previous_buttons = 0;
#endif
    for(;;) {
        const uint32_t starting_video_frame = jack_wave1_frame_counter();
        g_buttons = jack_wave1_poll_buttons();
        const uint32_t core_start = time_us_32();
        begin_audio_frame();
        run_cpu_segment(g_next_segment_cycles);
        poll_frame_usb();
        g_cpu_pins |= W65C02_IRQ;
        run_cpu_segment(32768);
        poll_frame_usb();
        g_cpu_pins |= W65C02_IRQ;
        run_cpu_segment(7364);
        poll_frame_usb();
        g_next_segment_cycles = 32768 - 7364;
        const uint32_t core_end = time_us_32();
#ifdef JACK_GAMATE_AUDIO_DIAGNOSTICS
        if(g_buttons != previous_buttons) {
            jack_wave1_usb_status_t status{};
            jack_wave1_usb_get_status(&status);
            printf("GAMATE_INPUT buttons=%04x port4400=%02x reports=%lu\n",
                   g_buttons, gamate_input_port(g_buttons),
                   static_cast<unsigned long>(status.report_count));
            previous_buttons = g_buttons;
        }
#endif
        finish_audio_frame();
        render_frame();
        const uint32_t draw_end = time_us_32();
        const bool audio_waited = jack_wave1_audio_wait();
        const bool video_waited = jack_wave1_wait_frame_boundary(starting_video_frame);
        jack_wave1_audio_report();
        report_psg();
        perf.sample("GAMATE", core_end - core_start, draw_end - core_end,
                    time_us_32() - draw_end, g_buttons,
                    audio_waited || video_waited ? "running-audio" : "late-audio",
                    jack_wave1_perf_stdout);
    }
}
