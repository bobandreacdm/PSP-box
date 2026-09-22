#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspmp3.h>
#include <psputility.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("PSPBox", 0, 1, 9);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define MP3_BUF_SIZE (16 * 1024)
#define PCM_BUF_SIZE (16 * 1024)
#define MAX_FILES 100

// Buffer audio allineati a 64-byte per il chip Media Engine della PSP
static unsigned char mp3_buf[MP3_BUF_SIZE] __attribute__((aligned(64)));
static short pcm_buf[PCM_BUF_SIZE / sizeof(short)] __attribute__((aligned(64)));

struct Deck {
    char title[64];
    char full_path[256];
    int handle;
    int mp3_handle;
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

Deck deckA;
FileEntry file_list[MAX_FILES];
int file_count = 0;
int selected_file = 0;
int in_browser = 0;
int audio_channel = -1;
int mp3_decoder_inited = 0;

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

float ReadRekordboxBPM(const char* fullpath) {
    int fd = sceIoOpen(fullpath, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0.0f;

    unsigned char buffer[1024];
    int read_bytes = sceIoRead(fd, buffer, sizeof(buffer) - 1);
    sceIoClose(fd);

    if (read_bytes <= 0) return 0.0f;

    for (int i = 0; i < read_bytes - 4; i++) {
        if (buffer[i] == 'T' && buffer[i+1] == 'B' && buffer[i+2] == 'P' && buffer[i+3] == 'M') {
            int frame_size = (buffer[i+4] << 21) | (buffer[i+5] << 14) | (buffer[i+6] << 7) | buffer[i+7];
            if (frame_size > 0 && frame_size < 32 && (i + 10 + frame_size) <= read_bytes) {
                char bpm_str[32];
                memset(bpm_str, 0, sizeof(bpm_str));
                int text_offset = i + 10;
                if (buffer[text_offset] == 0) text_offset++;
                
                int len = 0;
                for (int j = text_offset; j < i + 10 + frame_size && len < 31; j++) {
                    if (buffer[j] >= 32 && buffer[j] <= 126) {
                        bpm_str[len++] = (char)buffer[j];
                    }
                }
                bpm_str[len] = '\0';
                float parsed_bpm = (float)atof(bpm_str);
                if (parsed_bpm > 30.0f && parsed_bpm < 300.0f) {
                    return parsed_bpm;
                }
            }
        }
    }
    return 0.0f;
}

long GetAudioStartDataOffset(int fd) {
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    unsigned char header[10];
    if (sceIoRead(fd, header, 10) < 10) return 0;

    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        long tag_size = ((header[6] & 0x7F) << 21) |
                        ((header[7] & 0x7F) << 14) |
                        ((header[8] & 0x7F) << 7)  |
                         (header[9] & 0x7F);
        return tag_size + 10;
    }
    return 0;
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
    
    if (deckA.mp3_handle >= 0) {
        sceMp3ReleaseMp3Handle(deckA.mp3_handle);
        deckA.mp3_handle = -1;
    }
    if (deckA.handle >= 0) {
        sceIoClose(deckA.handle);
        deckA.handle = -1;
    }
    if (audio_channel >= 0) {
        sceAudioChRelease(audio_channel);
        audio_channel = -1;
    }
}

void FillMp3Buffer(int fd, int mp3Handle) {
    SceUChar8* dst = NULL;
    SceInt32 towrite = 0;
    SceInt32 srcpos = 0;

    int ret = sceMp3GetInfoToAddStreamData(mp3Handle, &dst, &towrite, &srcpos);
    if (ret >= 0 && towrite > 0) {
        sceIoLseek(fd, srcpos, PSP_SEEK_SET);
        int read = sceIoRead(fd, dst, towrite);
        if (read > 0) {
            sceMp3NotifyAddStreamData(mp3Handle, read);
        }
    }
}

void LoadTrack(const char* filename, const char* fullpath) {
    StopAndCloseAudio();

    snprintf(deckA.title, 64, "%s", filename);
    snprintf(deckA.full_path, 256, "%s", fullpath);

    float rekordbox_bpm = ReadRekordboxBPM(fullpath);
    deckA.base_bpm = (rekordbox_bpm > 0.0f) ? rekordbox_bpm : 120.0f;
    deckA.bpm_found = (rekordbox_bpm > 0.0f) ? 1 : 0;

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
    long data_start = GetAudioStartDataOffset(deckA.handle);
    sceIoLseek(deckA.handle, data_start, PSP_SEEK_SET);
    deckA.current_pos = data_start;

    int res_init = sceMp3InitResource();
    if (res_init < 0 && res_init != (int)0x80671001) {
        sceMp3TermResource();
        sceMp3InitResource();
    }
    mp3_decoder_inited = 1;

    SceMp3InitArg mp3Init;
    memset(&mp3Init, 0, sizeof(SceMp3InitArg));
    mp3Init.mp3StreamStart = data_start;
    mp3Init.mp3StreamEnd = deckA.file_size;
    mp3Init.mp3Buf = mp3_buf;
    mp3Init.mp3BufSize = MP3_BUF_SIZE;
    mp3Init.pcmBuf = (unsigned char*)pcm_buf;
    mp3Init.pcmBufSize = PCM_BUF_SIZE;

    deckA.mp3_handle = sceMp3ReserveMp3Handle(&mp3Init);
    if (deckA.mp3_handle < 0) {
        snprintf(deckA.status_msg, 128, "Err ReserveHandle: 0x%08X", deckA.mp3_handle);
        return;
    }

    int init_res = sceMp3Init(deckA.mp3_handle);
    if (init_res < 0) {
        snprintf(deckA.status_msg, 128, "Err sceMp3Init: 0x%08X", init_res);
        return;
    }

    FillMp3Buffer(deckA.handle, deckA.mp3_handle);

    deckA.sample_rate = sceMp3GetSamplingRate(deckA.mp3_handle);
    deckA.channels = sceMp3GetMp3ChannelNum(deckA.mp3_handle);
    if (deckA.sample_rate <= 0) deckA.sample_rate = 44100;
    if (deckA.channels <= 0) deckA.channels = 2;

    if (audio_channel < 0) {
        audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1152, PSP_AUDIO_FORMAT_STEREO);
    }

    if (audio_channel < 0) {
        snprintf(deckA.status_msg, 128, "Err AudioChReserve (%d)", audio_channel);
        return;
    }

    snprintf(deckA.status_msg, 128, "Pronto (%d Hz, %d ch)", deckA.sample_rate, deckA.channels);
}

void UpdateAudio() {
    if (!deckA.is_playing || deckA.mp3_handle < 0 || audio_channel < 0) return;

    if (sceMp3CheckStreamDataNeeded(deckA.mp3_handle)) {
        FillMp3Buffer(deckA.handle, deckA.mp3_handle);
    }

    short* decoded_pcm = NULL;
    int decoded_bytes = sceMp3Decode(deckA.mp3_handle, &decoded_pcm);

    if (decoded_bytes > 0 && decoded_pcm != NULL) {
        int samples = decoded_bytes / (sizeof(short) * deckA.channels);
        sceAudioOutputPannedBlocking(audio_channel, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, decoded_pcm);

        deckA.current_pos = sceIoLseek(deckA.handle, 0, PSP_SEEK_CUR);
        if (deckA.file_size > 0) {
            deckA.progress = ((float)deckA.current_pos / (float)deckA.file_size) * 100.0f;
        }
    } else if (decoded_bytes == 0) {
        deckA.is_playing = 0;
        snprintf(deckA.status_msg, 128, "Fine Traccia");
    }
}

void RenderUI() {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenSetTextColor(0x00FFFFFF);

    printf("==================================================\n");
    printf("         PSPBox DJ - v1.9 (FIX RESOURCE)          \n");
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

        if (deckA.bpm_found) {
            printf(" BPM     : %.1f  (Base: %.1f) [REKORDBOX OK]\n", deckA.current_bpm, deckA.base_bpm);
        } else {
            printf(" BPM     : %.1f  (Base: %.1f) [DEFAULT 120]\n", deckA.current_bpm, deckA.base_bpm);
        }

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
    deckA.mp3_handle = -1;
    snprintf(deckA.status_msg, 128, "Seleziona una traccia dal Browser");

    ScanMusicDirectory();

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
                if (deckA.mp3_handle >= 0) {
                    deckA.is_playing = !deckA.is_playing;
                    snprintf(deckA.status_msg, 128, deckA.is_playing ? "In riproduzione" : "Pausa");
                }
            }
            if (pressed & PSP_CTRL_SQUARE) {
                deckA.is_playing = 0;
                if (deckA.handle >= 0 && deckA.mp3_handle >= 0) {
                    long data_start = GetAudioStartDataOffset(deckA.handle);
                    sceIoLseek(deckA.handle, data_start, PSP_SEEK_SET);
                    sceMp3ResetPlayPosition(deckA.mp3_handle);
                    FillMp3Buffer(deckA.handle, deckA.mp3_handle);
                    deckA.progress = 0.0f;
                    snprintf(deckA.status_msg, 128, "CUE (Inizio traccia)");
                }
            }

            // Regolazione Pitch con lo Analog Stick
            if (pad.Ly < 80) {
                deckA.pitch += 0.1f;
                if (deckA.pitch > 16.0f) deckA.pitch = 16.0f;
            } else if (pad.Ly > 170) {
                deckA.pitch -= 0.1f;
                if (deckA.pitch < -16.0f) deckA.pitch = -16.0f;
            }

            // Pitch Bend temporaneo (Sinistra / Destra)
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

        sceKernelDelayThread(10000); // Sleep per evitare di saturare la CPU
    }

    StopAndCloseAudio();
    if (mp3_decoder_inited) {
        sceMp3TermResource();
    }

    return 0;
}
