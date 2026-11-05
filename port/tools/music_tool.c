/* tlg-music-tool: plays the game's own menu music (sce_sys/snd0.at9, the tune the PS4 home screen
 * and shadPS4's game list play) on a loop while the launcher is open. The file comes from the
 * player's game folder; nothing of the game is shipped. Windows and Linux (SDL3 audio, LibAtrac9).
 *   tlg-music-tool <snd0.at9> [volume 0-100] [--track [stereo|surround]]
 * Input (stdin): "volume <0-100>", "pause", "resume", "seek <seconds>", "quit". End of stdin also quits (the
 * launcher went away).
 * --track: for the launcher's Soundtrack page. Plays the file once instead of looping. The game's
 * music is surround: 4 channels (front L/R, back L/R) or 6 (5.1: front L/R, center, sub, back L/R).
 * "stereo" (default) mixes that down to two speakers; "surround" sends every channel to its own
 * speaker (Windows/SDL mixes down if the speakers are fewer). Raises it to full level (the music
 * is mastered very quietly), prints "pos <seconds> <total>" every half second and
 * "end" when done.
 * Exit 1 when the file cannot be read or decoded: the launcher then stays silent. */
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libatrac9.h"

static SDL_AtomicInt quit_flag, volume, paused, seek_ms; /* seek_ms -1 = no seek asked */

static int stdin_thread(void *unused) {
    (void)unused;
    char line[64];
    int v;
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "quit", 4)) break;
        if (!strncmp(line, "pause", 5)) SDL_SetAtomicInt(&paused, 1);
        if (!strncmp(line, "resume", 6)) SDL_SetAtomicInt(&paused, 0);
        double sec;
        if (sscanf(line, "seek %lf", &sec) == 1) SDL_SetAtomicInt(&seek_ms, sec < 0 ? 0 : (int)(sec * 1000));
        if (sscanf(line, "volume %d", &v) == 1) SDL_SetAtomicInt(&volume, v < 0 ? 0 : v > 100 ? 100 : v);
    }
    SDL_SetAtomicInt(&quit_flag, 1);
    return 0;
}

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* RIFF/WAVE with an ATRAC9 extensible fmt chunk: the 4 config bytes sit at offset 44 of fmt,
 * fact holds the sample count and the encoder delay, data holds whole superframes. */
static short *decode(const char *path, int *channels, int *rate, size_t *frames) {
    size_t size;
    unsigned char *file = SDL_LoadFile(path, &size);
    if (!file) return NULL;
    short *pcm = NULL;
    const unsigned char *config = NULL, *data = NULL;
    size_t data_size = 0, total = 0, delay = 0;
    if (size >= 12 && !memcmp(file, "RIFF", 4) && !memcmp(file + 8, "WAVE", 4)) {
        for (size_t at = 12; at + 8 <= size;) {
            const uint32_t n = le32(file + at + 4);
            const unsigned char *body = file + at + 8;
            if (n > size - at - 8) break;
            if (!memcmp(file + at, "fmt ", 4) && n >= 48) config = body + 44;
            if (!memcmp(file + at, "fact", 4) && n >= 8) { total = le32(body); delay = le32(body + 4); }
            if (!memcmp(file + at, "data", 4)) { data = body; data_size = n; }
            at += 8 + n + (n & 1);
        }
    }
    void *at9 = Atrac9GetHandle();
    Atrac9CodecInfo info;
    if (config && data && at9 && !Atrac9InitDecoder(at9, (unsigned char *)config) && !Atrac9GetCodecInfo(at9, &info) &&
        info.channels > 0 && info.channels <= 8 && info.superframeSize > 0) {
        const size_t superframes = data_size / info.superframeSize;
        const size_t per = (size_t)info.frameSamples * info.framesInSuperframe;
        pcm = malloc(superframes * per * info.channels * sizeof(short));
        size_t done = 0;
        for (size_t s = 0; pcm && s < superframes; ++s) {
            const unsigned char *in = data + s * info.superframeSize;
            for (int f = 0; f < info.framesInSuperframe; ++f) {
                int used = 0;
                if (Atrac9Decode(at9, in, pcm + done * info.channels, &used) || used <= 0) break;
                in += used;
                done += info.frameSamples;
            }
        }
        if (delay >= done) delay = 0;
        if (!total || total > done - delay) total = done - delay;
        if (pcm && total) {
            memmove(pcm, pcm + delay * info.channels, total * info.channels * sizeof(short));
            *channels = info.channels;
            *rate = info.samplingRate;
            *frames = total;
        } else {
            free(pcm);
            pcm = NULL;
        }
    }
    if (at9) Atrac9ReleaseHandle(at9);
    SDL_free(file);
    return pcm;
}

/* One output frame from one input frame. Stereo: the usual downmix (back to its side, center to
 * both at -3 dB, sub to both at -6 dB). Surround: channels pass through unchanged. */
static int mix_frame(const short *in, int channels, bool surround, float *out) {
    if (surround || channels <= 2) {
        for (int c = 0; c < channels; ++c) out[c] = in[c];
        return channels; /* mono stays mono: SDL plays it on both speakers */
    }
    float l = in[0], r = in[1];
    if (channels == 4) {
        l += in[2], r += in[3];
    } else if (channels >= 6) {
        const float center = 0.7071f * in[2] + 0.5f * in[3];
        l += center + in[4], r += center + in[5];
        for (int c = 6; c < channels; ++c) *(c & 1 ? &r : &l) += in[c];
    } else {
        for (int c = 2; c < channels; ++c) *(c & 1 ? &r : &l) += in[c];
    }
    out[0] = l, out[1] = r;
    return 2;
}

/* Mix as chosen, then scale so the loudest sample is at about -1 dB. Works in place: the output
 * never has more channels than the input. */
static void mix_and_normalize(short *pcm, int *channels, size_t frames, bool surround) {
    const int in = *channels;
    float out[8], peak = 1.0f;
    int n = in;
    for (size_t f = 0; f < frames; ++f) {
        n = mix_frame(pcm + f * in, in, surround, out);
        for (int c = 0; c < n; ++c) peak = SDL_max(peak, SDL_fabsf(out[c]));
    }
    const float scale = 29000.0f / peak;
    for (size_t f = 0; f < frames; ++f) {
        mix_frame(pcm + f * in, in, surround, out);
        for (int c = 0; c < n; ++c) pcm[f * n + c] = (short)SDL_clamp(out[c] * scale, -32767.0f, 32767.0f);
    }
    *channels = n;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const bool track = argc > 3 && !strcmp(argv[3], "--track");
    SDL_SetAtomicInt(&volume, argc > 2 ? atoi(argv[2]) : 60);
    int channels = 0, rate = 0;
    size_t frames = 0;
    short *pcm = decode(argv[1], &channels, &rate, &frames);
    if (!pcm || !SDL_Init(SDL_INIT_AUDIO)) return 1;
    if (track) mix_and_normalize(pcm, &channels, frames, argc > 4 && !strcmp(argv[4], "surround"));
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, channels, rate};
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) return 1;
    SDL_DetachThread(SDL_CreateThread(stdin_thread, "stdin", NULL));
    const int bytes_per_frame = channels * (int)sizeof(short);
    const size_t chunk = (size_t)rate / 10; /* feed 0.1 s at a time so volume changes act quickly */
    size_t at = 0;
    float gain = 0.0f; /* fades in from silence */
    if (track) gain = SDL_GetAtomicInt(&volume) / 100.0f; /* a chosen piece starts at once */
    SDL_ResumeAudioStreamDevice(stream);
    bool was_paused = false, fed_all = false;
    SDL_SetAtomicInt(&seek_ms, -1);
    Uint64 next_report = 0;
    while (!SDL_GetAtomicInt(&quit_flag)) {
        const int seek = SDL_SetAtomicInt(&seek_ms, -1);
        if (seek >= 0) { /* jump: drop what is queued and feed from the new spot */
            SDL_ClearAudioStream(stream);
            at = SDL_min((size_t)((double)seek * rate / 1000.0), frames ? frames - 1 : 0);
            fed_all = false;
            next_report = 0;
        }
        const bool pause = SDL_GetAtomicInt(&paused) != 0;
        if (pause != was_paused) {
            if (pause) SDL_PauseAudioStreamDevice(stream); else SDL_ResumeAudioStreamDevice(stream);
            was_paused = pause;
        }
        const float target = SDL_GetAtomicInt(&volume) / 100.0f;
        if (gain != target) gain = gain < target ? SDL_min(target, gain + 0.02f) : SDL_max(target, gain - 0.05f);
        SDL_SetAudioStreamGain(stream, gain);
        while (!fed_all && SDL_GetAudioStreamQueued(stream) < (int)(chunk * 3) * bytes_per_frame) {
            const size_t n = SDL_min(chunk, frames - at);
            SDL_PutAudioStreamData(stream, pcm + at * channels, (int)n * bytes_per_frame);
            at += n;
            if (at >= frames) {
                if (track) fed_all = true; /* a soundtrack piece plays once */
                else at = 0;               /* the menu tune loops seamlessly from its end to its start */
            }
        }
        if (track) {
            const int queued = SDL_GetAudioStreamQueued(stream) / bytes_per_frame;
            if (fed_all && queued <= 0) {
                printf("end\n");
                fflush(stdout);
                break;
            }
            if (SDL_GetTicks() >= next_report) {
                const size_t played = at > (size_t)queued ? at - (size_t)queued : 0;
                printf("pos %.1f %.1f\n", (double)played / rate, (double)frames / rate);
                fflush(stdout);
                next_report = SDL_GetTicks() + 500;
            }
        }
        SDL_Delay(20);
    }
    SDL_DestroyAudioStream(stream);
    SDL_Quit();
    free(pcm);
    return 0;
}
