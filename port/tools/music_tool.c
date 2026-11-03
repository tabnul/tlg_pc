/* tlg-music-tool: plays the game's own menu music (sce_sys/snd0.at9, the tune the PS4 home screen
 * and shadPS4's game list play) on a loop while the launcher is open. The file comes from the
 * player's game folder; nothing of the game is shipped. Windows and Linux (SDL3 audio, LibAtrac9).
 *   tlg-music-tool <snd0.at9> [volume 0-100]
 * Input (stdin): "volume <0-100>", "quit". End of stdin also quits (the launcher went away).
 * Exit 1 when the file cannot be read or decoded: the launcher then stays silent. */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libatrac9.h"

static SDL_AtomicInt quit_flag, volume;

static int stdin_thread(void *unused) {
    (void)unused;
    char line[64];
    int v;
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "quit", 4)) break;
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

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    SDL_SetAtomicInt(&volume, argc > 2 ? atoi(argv[2]) : 60);
    int channels = 0, rate = 0;
    size_t frames = 0;
    short *pcm = decode(argv[1], &channels, &rate, &frames);
    if (!pcm || !SDL_Init(SDL_INIT_AUDIO)) return 1;
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, channels, rate};
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) return 1;
    SDL_DetachThread(SDL_CreateThread(stdin_thread, "stdin", NULL));
    const int bytes_per_frame = channels * (int)sizeof(short);
    const size_t chunk = (size_t)rate / 10; /* feed 0.1 s at a time so volume changes act quickly */
    size_t at = 0;
    float gain = 0.0f; /* fades in from silence */
    SDL_ResumeAudioStreamDevice(stream);
    while (!SDL_GetAtomicInt(&quit_flag)) {
        const float target = SDL_GetAtomicInt(&volume) / 100.0f;
        if (gain != target) gain = gain < target ? SDL_min(target, gain + 0.02f) : SDL_max(target, gain - 0.05f);
        SDL_SetAudioStreamGain(stream, gain);
        while (SDL_GetAudioStreamQueued(stream) < (int)(chunk * 3) * bytes_per_frame) {
            const size_t n = SDL_min(chunk, frames - at);
            SDL_PutAudioStreamData(stream, pcm + at * channels, (int)n * bytes_per_frame);
            at = (at + n) % frames; /* the tune loops seamlessly from its end to its start */
        }
        SDL_Delay(20);
    }
    SDL_DestroyAudioStream(stream);
    SDL_Quit();
    free(pcm);
    return 0;
}
