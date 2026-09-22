#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <pspmp3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

PSP_MODULE_INFO("PSPBox", 0, 1, 9);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define MP3_BUF_SIZE (16 * 1024)
#define PCM_BUF_SIZE (2048 * 4)

typedef struct {
    char name[64];
    int is_dir;
} FileItem;

typedef struct {
    char title[64];
    char full_path[256];
    float base_bpm;
    float current_bpm;
    float pitch;            // Pitch fisso impostato
    float pitch_bend;       // Spinta temporanea da analogico Sinistra/Destra
    int is_playing;
    float progress;
    int handle;
    int mp3_handle;
    int bpm_found;
} DeckState;

DeckState deckA = {"Nessuna Traccia", "", 120.0f, 120.0f, 0.0f, 0.0f, 0, 0.0f, -1, -1, 0};

int browser_open = 0;
char current_path[256] = "ms0:/MUSIC";
FileItem file_list[30];
int file_count = 0;
int selected_index = 0;

int audio_channel = -1;
int mp3_decoder_inited = 0;
unsigned char mp3_buf[MP3_BUF_SIZE] __attribute__((aligned(64)));
short pcm_buf[PCM_BUF_SIZE] __attribute__((aligned(64)));

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

void SetupCallbacks() {
    int thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
}

// Scansione robusta del tag TBPM di Rekordbox
float ReadRekordboxBPM(const char* fullpath) {
    int fd = sceIoOpen(fullpath, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0.0f;

    unsigned char buffer[8192];
    int read_bytes = sceIoRead(fd, buffer, sizeof(buffer));
    sceIoClose(fd);

    if (read_bytes < 10) return 0.0f;

    for (int i = 0; i < read_bytes - 12; i++) {
        if (buffer[i] == 'T' && buffer[i+1] == 'B' && buffer[i+2] == 'P' && buffer[i+3] == 'M') {
            char bpm_str[16] = {0};
            int idx = 0;
            for (int j = i + 6; j < i + 30 && j < read_bytes; j++) {
                char c = buffer[j];
                if ((c >= '0' && c <= '9') || c == '.') {
                    bpm_str[idx++] = c;
                    if (idx >= 15) break;
                }
            }
            if (idx > 0) {
                float detected_bpm = atof(bpm_str);
                if (detected_bpm > 30.0f && detected_bpm < 300.0f) {
                    return detected_bpm;
                }
            }
        }
    }
    return 0.0f;
}

void StopAndCloseAudio() {
    if (deckA.mp3_handle >= 0) {
        sceMp3ReleaseMp3Handle(deckA.mp3_handle);
        deckA.mp3_handle = -1;
    }
    if (mp3_decoder_inited) {
        sceMp3TermResource();
        mp3_decoder_inited = 0;
    }
    if (deckA.handle >= 0) {
        sceIoClose(deckA.handle);
        deckA.handle = -1;
    }
}

void ScanPath(const char* path) {
    file_count = 0;
    
    if (strcmp(path, "ms0:") != 0 && strcmp(path, "ms0:/") != 0 && strcmp(path, "ms0:/MUSIC") != 0) {
        snprintf(file_list[0].name, 64, "..");
        file_list[0].is_dir = 1;
        file_count = 1;
    }

    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && file_count < 30) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            
            snprintf(file_list[file_count].name, 64, "%s", ent->d_name);
            
            char full_item_path[300];
            snprintf(full_item_path, sizeof(full_item_path), "%s/%s", path, ent->d_name);
            
            SceIoStat stat;
            memset(&stat, 0, sizeof(SceIoStat));
            if (sceIoGetstat(full_item_path, &stat) >= 0) {
                file_list[file_count].is_dir = FIO_SO_ISDIR(stat.st_attr) ? 1 : 0;
            } else {
                file_list[file_count].is_dir = 0;
            }

            file_count++;
        }
        closedir(dir);
    }
    
    if (file_count == 0) {
        snprintf(file_list[0].name, 64, "Nessun file presente");
        file_list[0].is_dir = 0;
        file_count = 1;
    }
    selected_index = 0;
}

void FillMp3Buffer(int fd, int mp3_handle) {
    if (sceMp3CheckStreamDataNeeded(mp3_handle) > 0) {
        int read_bytes = sceIoRead(fd, mp3_buf, MP3_BUF_SIZE);
        if (read_bytes > 0) {
            sceMp3NotifyAddStreamData(mp3_handle, read_bytes);
        }
    }
}

void LoadTrack(const char* filename, const char* fullpath) {
    StopAndCloseAudio();

    snprintf(deckA.title, 64, "%s", filename);
    snprintf(deckA.full_path, 256, "%s", fullpath);
    
    float rekordbox_bpm = ReadRekordboxBPM(fullpath);

    if (rekordbox_bpm > 0.0f) {
        deckA.base_bpm = rekordbox_bpm;
        deckA.bpm_found = 1;
    } else {
        deckA.base_bpm = 120.0f;
        deckA.bpm_found = 0;
    }

    deckA.pitch = 0.0f;
    deckA.pitch_bend = 0.0f;
    deckA.current_bpm = deckA.base_bpm;
    deckA.progress = 0.0f;

    deckA.handle = sceIoOpen(fullpath, PSP_O_RDONLY, 0777);
    if (deckA.handle >= 0) {
        sceMp3InitResource();
        mp3_decoder_inited = 1;

        SceMp3InitArg mp3Init;
        memset(&mp3Init, 0, sizeof(SceMp3InitArg));
        mp3Init.mp3StreamStart = 0;
        mp3Init.mp3StreamEnd = sceIoLseek(deckA.handle, 0, PSP_SEEK_END);
        sceIoLseek(deckA.handle, 0, PSP_SEEK_SET);
        mp3Init.mp3Buf = mp3_buf;
        mp3Init.mp3BufSize = MP3_BUF_SIZE;
        mp3Init.pcmBuf = (unsigned char*)pcm_buf;
        mp3Init.pcmBufSize = PCM_BUF_SIZE;

        deckA.mp3_handle = sceMp3ReserveMp3Handle(&mp3Init);
        if (deckA.mp3_handle >= 0) {
            sceMp3Init(deckA.mp3_handle);
            FillMp3Buffer(deckA.handle, deckA.mp3_handle);
            deckA.is_playing = 0;
        } else {
            deckA.is_playing = 0;
        }
    } else {
        deckA.is_playing = 0;
    }
}

int main() {
    SetupCallbacks();

    pspAudioInit();
    audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 2048, PSP_AUDIO_FORMAT_STEREO);

    pspDebugScreenInit();
    ScanPath(current_path);

    SceCtrlData pad;
    unsigned int last_buttons = 0;

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~last_buttons;

        if (pressed & PSP_CTRL_TRIANGLE) {
            browser_open = !browser_open;
        }

        if (browser_open) {
            if (pressed & PSP_CTRL_DOWN) {
                if (selected_index < file_count - 1) selected_index++;
            }
            if (pressed & PSP_CTRL_UP) {
                if (selected_index > 0) selected_index--;
            }
            if (pressed & PSP_CTRL_CROSS) {
                if (file_list[selected_index].is_dir) {
                    if (strcmp(file_list[selected_index].name, "..") == 0) {
                        char *last_slash = strrchr(current_path, '/');
                        if (last_slash && last_slash != current_path + 3) {
                            *last_slash = '\0';
                        }
                    } else {
                        char new_path[256];
                        snprintf(new_path, sizeof(new_path), "%s/%s", current_path, file_list[selected_index].name);
                        snprintf(current_path, sizeof(current_path), "%s", new_path);
                    }
                    ScanPath(current_path);
                } else {
                    char full_path[300];
                    snprintf(full_path, sizeof(full_path), "%s/%s", current_path, file_list[selected_index].name);
                    LoadTrack(file_list[selected_index].name, full_path);
                    browser_open = 0;
                }
            }
        } else {
            int shift = (pad.Buttons & PSP_CTRL_CIRCLE) ? 1 : 0;
            float step = shift ? 1.0f : 0.1f;

            // PITCH FISSO: Tasti L/R
            if (pressed & PSP_CTRL_LTRIGGER) {
                deckA.pitch -= step;
                if (deckA.pitch < -16.0f) deckA.pitch = -16.0f;
            }
            if (pressed & PSP_CTRL_RTRIGGER) {
                deckA.pitch += step;
                if (deckA.pitch > 16.0f) deckA.pitch = 16.0f;
            }

            // PITCH FISSO: Analogico SU / GIU
            if (pad.Ly < 80) {
                deckA.pitch += 0.05f;
                if (deckA.pitch > 16.0f) deckA.pitch = 16.0f;
            } else if (pad.Ly > 175) {
                deckA.pitch -= 0.05f;
                if (deckA.pitch < -16.0f) deckA.pitch = -16.0f;
            }

            // PITCH BEND TEMPORANEO: Analogico SINISTRA / DESTRA
            if (pad.Lx > 175) {
                deckA.pitch_bend = 4.0f;   // Spinta temporanea +4% BPM
            } else if (pad.Lx < 80) {
                deckA.pitch_bend = -4.0f;  // Freno temporaneo -4% BPM
            } else {
                deckA.pitch_bend = 0.0f;   // Rilasciato: torna ai BPM di pitch fisso
            }

            if (pressed & PSP_CTRL_CROSS) {
                deckA.is_playing = !deckA.is_playing;
            }

            if (pressed & PSP_CTRL_SQUARE) {
                deckA.is_playing = 0;
                deckA.progress = 0.0f;
                if (deckA.handle >= 0 && deckA.mp3_handle >= 0) {
                    sceIoLseek(deckA.handle, 0, PSP_SEEK_SET);
                    sceMp3Init(deckA.mp3_handle);
                    FillMp3Buffer(deckA.handle, deckA.mp3_handle);
                }
            }

            // Calcolo finale BPM includendo il Pitch Bend
            deckA.current_bpm = deckA.base_bpm * (1.0f + ((deckA.pitch + deckA.pitch_bend) / 100.0f));
        }

        // DECODIFICA AUDIO MP3 REALE
        if (deckA.is_playing && mp3_decoder_inited && deckA.mp3_handle >= 0) {
            FillMp3Buffer(deckA.handle, deckA.mp3_handle);
            
            short* pcm_ptr = pcm_buf;
            int decoded_samples = sceMp3Decode(deckA.mp3_handle, &pcm_ptr);
            
            if (decoded_samples > 0) {
                sceAudioOutputPannedBlocking(audio_channel, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, pcm_ptr);
                deckA.progress += 0.05f;
                if (deckA.progress > 100.0f) deckA.progress = 0.0f;
            } else {
                // Se la traccia è arrivata alla fine
                FillMp3Buffer(deckA.handle, deckA.mp3_handle);
            }
        }

        last_buttons = pad.Buttons;

        pspDebugScreenSetXY(0, 0);
        
        if (browser_open) {
            pspDebugScreenPrintf("==================================================\n");
            pspDebugScreenPrintf("  PSPBox BROWSER                                  \n");
            pspDebugScreenPrintf("  PATH: %s\n", current_path);
            pspDebugScreenPrintf("==================================================\n\n");

            for (int i = 0; i < file_count; i++) {
                if (i == selected_index) {
                    pspDebugScreenPrintf(" > %-7s %s <\n", file_list[i].is_dir ? "[DIR]" : "[TRACCIA]", file_list[i].name);
                } else {
                    pspDebugScreenPrintf("   %-7s %s  \n", file_list[i].is_dir ? "[DIR]" : "[TRACCIA]", file_list[i].name);
                }
            }
            
            pspDebugScreenPrintf("\n--------------------------------------------------\n");
            pspDebugScreenPrintf(" [D-PAD]: Scorri  |  [X]: Seleziona  |  [TRIANGOLO]: DECK\n");
        } else {
            pspDebugScreenPrintf("==================================================\n");
            pspDebugScreenPrintf("      PSPBox DJ - v1.9 PITCH BEND & MP3 PLAY      \n");
            pspDebugScreenPrintf("==================================================\n\n\n");

            pspDebugScreenPrintf("   TRACCIA :  %s\n\n", deckA.title);
            pspDebugScreenPrintf("   BPM     :  %.1f  (Base: %.1f) [%s]\n\n", 
                                 deckA.current_bpm, 
                                 deckA.base_bpm, 
                                 deckA.bpm_found ? "REKORDBOX OK" : "DEFAULT 120");
            pspDebugScreenPrintf("   PITCH   :  %+.1f%%  %s\n\n", 
                                 deckA.pitch, 
                                 deckA.pitch_bend > 0 ? "[BEND +]" : (deckA.pitch_bend < 0 ? "[BEND -]" : ""));
            pspDebugScreenPrintf("   AUDIO   :  [%s]\n\n\n", deckA.is_playing ? " PLAYING " : " PAUSED / CUE ");

            pspDebugScreenPrintf("   [");
            int bar_pos = (int)((deckA.progress / 100.0f) * 30);
            for (int i = 0; i < 30; i++) {
                if (i == bar_pos) pspDebugScreenPrintf(">");
                else if (i < bar_pos) pspDebugScreenPrintf("=");
                else pspDebugScreenPrintf(" ");
            }
            pspDebugScreenPrintf("]  %.0f%%\n\n\n", deckA.progress);

            pspDebugScreenPrintf("--------------------------------------------------\n");
            pspDebugScreenPrintf(" [X]: Play | [QUADRATO]: CUE | [STICK UP/DN]: Pitch\n");
            pspDebugScreenPrintf(" [STICK L/R]: Pitch Bend (Mano sul piatto) | [TRIANGOLO]: Browser\n");
        }

        sceDisplayWaitVblankStart();
    }

    StopAndCloseAudio();
    if (audio_channel >= 0) sceAudioChRelease(audio_channel);
    pspAudioEnd();

    return 0;
}
