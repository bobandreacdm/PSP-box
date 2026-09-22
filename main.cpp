#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

PSP_MODULE_INFO("PSPBox", 0, 1, 9);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define MAX_FILES 100
#define AUDIO_BUF_SAMPLES 1152

struct Deck {
    char title[64];
    char full_path[256];
    int handle;
    long file_size;
    long current_pos;
    float base_bpm;
    float current_bpm;
    float pitch;
    float pitch_bend;
    float progress;
    int sample_rate;
    int channels;
    int is_playing;
    int bpm_found;
    char status_msg[128];
};

struct FileEntry {
    char name[64];
    char fullpath[256];
};

static Deck deckA;
static FileEntry file_list[MAX_FILES];
static int file_count = 0;
static int selected_file = 0;
static int in_browser = 0;
static int audio_channel = -1;

static mp3dec_t mp3d;
static mp3dec_frame_info_t info;
static unsigned char input_buf[2048];
static short pcm_output[MINIMP3_MAX_SAMPLES_PER_FRAME];

int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}

int CallbackThread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

void SetupCallbacks(void) {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

void ScanMusicDirectory() {
    file_count = 0;
    int dfd = sceIoDopen("ms0:/MUSIC");
    if (dfd < 0) return;

    SceIoDirent dir;
    memset(&dir, 0, sizeof(SceIoDirent));

    while (sceIoDread(dfd, &dir) > 0 && file_count < MAX_FILES) {
        if (FIO_S_ISREG(dir.d_stat.st_mode)) {
            char *ext = strrchr(dir.d_name, '.');
            if (ext && (strcasecmp(ext, ".mp3") == 0)) {
                snprintf(file_list[file_count].name, 64, "%s", dir.d_name);
                snprintf(file_list[file_count].fullpath, 256, "ms0:/MUSIC/%s", dir.d_name);
                file_count++;
            }
        }
    }
    sceIoDclose(dfd);
}

void StopAndCloseAudio() {
    deckA.is_playing = 0;
    if (deckA.handle >= 0) {
        sceIoClose(deckA.handle);
        deckA.handle = -1;
    }
    if (audio_channel >= 0) {
        sceAudioChRelease(audio_channel);
        audio_channel = -1;
    }
}

void LoadTrack(const char* filename, const char* fullpath) {
    StopAndCloseAudio();

    snprintf(deckA.title, 64, "%s", filename);
    snprintf(deckA.full_path, 256, "%s", fullpath);

    deckA.base_bpm = 120.0f;
    deckA.bpm_found = 0;
    deckA.pitch = 0.0f;
    deckA.pitch_bend = 0.0f;
    deckA.current_bpm = deckA.base_bpm;
    deckA.progress = 0.0f;

    deckA.handle = sceIoOpen(fullpath, PSP_O_RDONLY, 0777);
    if (deckA.handle < 0) {
        snprintf(deckA.status_msg, 128, "ERRORE: Impossibile aprire file");
        return;
    }

    deckA.file_size = sceIoLseek(deckA.handle, 0, PSP_SEEK_END);
    sceIoLseek(deckA.handle, 0, PSP_SEEK_SET);

    mp3dec_init(&mp3d);

    int read_bytes = sceIoRead(deckA.handle, input_buf, sizeof(input_buf));
    if (read_bytes > 0) {
        int samples = mp3dec_decode_frame(&mp3d, input_buf, read_bytes, pcm_output, &info);
        if (samples > 0) {
            deckA.sample_rate = info.hz;
            deckA.channels = info.channels;
        } else {
            deckA.sample_rate = 44100;
            deckA.channels = 2;
        }
        sceIoLseek(deckA.handle, 0, PSP_SEEK_SET);
    }

    if (audio_channel < 0) {
        audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_BUF_SAMPLES, PSP_AUDIO_FORMAT_STEREO);
    }

    if (audio_channel < 0) {
        snprintf(deckA.status_msg, 128, "Err AudioChReserve (%d)", audio_channel);
        return;
    }

    snprintf(deckA.status_msg, 128, "Pronto (%d Hz, %d ch)", deckA.sample_rate, deckA.channels);
}

void UpdateAudio() {
    if (!deckA.is_playing || deckA.handle < 0 || audio_channel < 0) return;

    int read_bytes = sceIoRead(deckA.handle, input_buf, sizeof(input_buf));
    if (read_bytes <= 0) {
        deckA.is_playing = 0;
        snprintf(deckA.status_msg, 128, "Fine Traccia");
        return;
    }

    int samples = mp3dec_decode_frame(&mp3d, input_buf, read_bytes, pcm_output, &info);
    if (samples > 0 && info.frame_bytes > 0) {
        long cur = sceIoLseek(deckA.handle, 0, PSP_SEEK_CUR);
        sceIoLseek(deckA.handle, cur - (read_bytes - info.frame_bytes), PSP_SEEK_SET);

        sceAudioOutputPannedBlocking(audio_channel, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, pcm_output);

        deckA.current_pos = sceIoLseek(deckA.handle, 0, PSP_SEEK_CUR);
        if (deckA.file_size > 0) {
            deckA.progress = ((float)deckA.current_pos / (float)deckA.file_size) * 100.0f;
        }
    }
}

void RenderUI() {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenSetTextColor(0x00FFFFFF);

    printf("==================================================\n");
    printf("         PSPBox DJ - v1.9.2 (SOFTWARE DECODER)     \n");
    printf("==================================================\n\n");

    if (in_browser) {
        printf("--- BROWSER FILE (ms0:/MUSIC) ---\n\n");
        if (file_count == 0) {
            printf("  Nessun file MP3 trovato in ms0:/MUSIC!\n");
        } else {
            for (int i = 0; i < file_count; i++) {
                if (i == selected_file) {
                    pspDebugScreenSetTextColor(0x0000FF00);
                    printf(" > %s <\n", file_list[i].name);
                    pspDebugScreenSetTextColor(0x00FFFFFF);
                } else {
                    printf("   %s\n", file_list[i].name);
                }
            }
        }
        printf("\n--------------------------------------------------\n");
        printf(" [DPAD]: Scorri | [X]: Carica | [TRIANGOLO]: Deck \n");
    } else {
        printf(" TRACCIA : %s\n", deckA.title[0] ? deckA.title : "Nessuna");
        printf(" STATO   : %s\n\n", deckA.status_msg);

        printf(" BPM     : %.1f  (Base: %.1f)\n", deckA.current_bpm, deckA.base_bpm);
        printf(" PITCH   : %+.1f%%\n\n", deckA.pitch + deckA.pitch_bend);
        printf(" AUDIO   : [%s]\n\n", deckA.is_playing ? "PLAYING" : "PAUSED / CUE");

        int bar_width = 30;
        int filled = (int)((deckA.progress / 100.0f) * bar_width);
        printf(" [");
        for (int i = 0; i < bar_width; i++) {
            if (i < filled) printf("=");
            else if (i == filled) printf(">");
            else printf(" ");
        }
        printf("] %.1f%%\n", deckA.progress);

        printf("\n--------------------------------------------------\n");
        printf(" [X]: Play | [QUADRATO]: Cue | [STICK UP/DN]: Pitch\n");
        printf(" [STICK L/R]: Pitch Bend | [TRIANGOLO]: Browser   \n");
    }
}

int main(void) {
    pspDebugScreenInit();
    SetupCallbacks();

    memset(&deckA, 0, sizeof(Deck));
    deckA.handle = -1;
    snprintf(deckA.status_msg, 128, "Premi TRIANGOLO per selezionare un brano");

    SceCtrlData pad;
    unsigned int last_buttons = 0;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~last_buttons;

        if (in_browser) {
            if ((pressed & PSP_CTRL_DOWN) && selected_file < file_count - 1) {
                selected_file++;
            }
            if ((pressed & PSP_CTRL_UP) && selected_file > 0) {
                selected_file--;
            }
            if ((pressed & PSP_CTRL_CROSS) && file_count > 0) {
                LoadTrack(file_list[selected_file].name, file_list[selected_file].fullpath);
                in_browser = 0;
            }
            if (pressed & PSP_CTRL_TRIANGLE) {
                in_browser = 0;
            }
        } else {
            if (pressed & PSP_CTRL_TRIANGLE) {
                ScanMusicDirectory();
                in_browser = 1;
            }
            if (pressed & PSP_CTRL_CROSS) {
                if (deckA.handle >= 0) {
                    deckA.is_playing = !deckA.is_playing;
                    snprintf(deckA.status_msg, 128, deckA.is_playing ? "In riproduzione" : "Pausa");
                }
            }
            if (pressed & PSP_CTRL_SQUARE) {
                deckA.is_playing = 0;
                if (deckA.handle >= 0) {
                    sceIoLseek(deckA.handle, 0, PSP_SEEK_SET);
                    deckA.progress = 0.0f;
                    snprintf(deckA.status_msg, 128, "CUE (Inizio traccia)");
                }
            }

            if (pad.Ly < 80) {
                deckA.pitch += 0.1f;
                if (deckA.pitch > 16.0f) deckA.pitch = 16.0f;
            } else if (pad.Ly > 170) {
                deckA.pitch -= 0.1f;
                if (deckA.pitch < -16.0f) deckA.pitch = -16.0f;
            }

            if (pad.Lx < 80) {
                deckA.pitch_bend = -4.0f;
            } else if (pad.Lx > 170) {
                deckA.pitch_bend = 4.0f;
            } else {
                deckA.pitch_bend = 0.0f;
            }

            float total_pitch = deckA.pitch + deckA.pitch_bend;
            deckA.current_bpm = deckA.base_bpm * (1.0f + (total_pitch / 100.0f));
        }

        last_buttons = pad.Buttons;

        UpdateAudio();
        RenderUI();

        sceKernelDelayThread(10000);
    }

    StopAndCloseAudio();
    return 0;
}
