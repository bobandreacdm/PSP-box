#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

PSP_MODULE_INFO("PSPBox", 0, 1, 7);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define AUDIO_BUFFER_SIZE 2048

typedef struct {
    char name[64];
    int is_dir;
} FileItem;

typedef struct {
    char title[64];
    char full_path[256];
    float base_bpm;
    float current_bpm;
    float pitch;
    int is_playing;
    float progress;
    int file_handle;
    int bpm_found;
} DeckState;

DeckState deckA = {"Nessuna Traccia", "", 120.0f, 120.0f, 0.0f, 0, 0.0f, -1, 0};

int browser_open = 0;
char current_path[256] = "ms0:/MUSIC";
FileItem file_list[30];
int file_count = 0;
int selected_index = 0;
int audio_channel = -1;

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

void ShowSplashScreen() {
    pspDebugScreenInit();
    for (int i = 0; i < 60; i++) {
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("\n\n\n");
        pspDebugScreenPrintf("  ==================================================\n");
        pspDebugScreenPrintf("  |               PSPBox DJ Engine                 |\n");
        pspDebugScreenPrintf("  |               v1.7 Rekordbox BPM Parser        |\n");
        pspDebugScreenPrintf("  ==================================================\n\n");
        pspDebugScreenPrintf("             Inizializzazione Reader ID3v2...\n");
        sceDisplayWaitVblankStart();
    }
}

// Lettore ID3v2 specifico per estrarre il BPM scritto da Rekordbox (Tag TBPM)
float ReadRekordboxBPM(const char* fullpath, char* title_out) {
    int fd = sceIoOpen(fullpath, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0.0f;

    unsigned char header[10];
    if (sceIoRead(fd, header, 10) < 10) {
        sceIoClose(fd);
        return 0.0f;
    }

    // Verifica la presenza dell'intestazione ID3v2
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        sceIoClose(fd);
        return 0.0f;
    }

    // Calcola dimensione del tag ID3v2
    int tag_size = ((header[6] & 0x7F) << 21) |
                   ((header[7] & 0x7F) << 14) |
                   ((header[8] & 0x7F) << 7)  |
                    (header[9] & 0x7F);

    unsigned char *buffer = (unsigned char*)malloc(tag_size);
    if (!buffer) {
        sceIoClose(fd);
        return 0.0f;
    }

    sceIoRead(fd, buffer, tag_size);
    sceIoClose(fd);

    float detected_bpm = 0.0f;
    int offset = 0;

    while (offset < tag_size - 10) {
        char frame_id[5];
        memcpy(frame_id, buffer + offset, 4);
        frame_id[4] = '\0';

        int frame_size = (buffer[offset + 4] << 24) | (buffer[offset + 5] << 16) |
                         (buffer[offset + 6] << 8)  |  buffer[offset + 7];

        if (frame_size <= 0 || offset + 10 + frame_size > tag_size) break;

        // Cerca frame TBPM (BPM Rekordbox/ID3)
        if (strcmp(frame_id, "TBPM") == 0) {
            char bpm_str[16] = {0};
            int read_len = frame_size < 15 ? frame_size : 15;
            memcpy(bpm_str, buffer + offset + 11, read_len - 1);
            detected_bpm = atof(bpm_str);
        }

        // Cerca frame TIT2 (Titolo brano ID3)
        if (strcmp(frame_id, "TIT2") == 0 && title_out) {
            int read_len = frame_size < 63 ? frame_size : 63;
            memcpy(title_out, buffer + offset + 11, read_len - 1);
            title_out[read_len - 1] = '\0';
        }

        offset += 10 + frame_size;
    }

    free(buffer);
    return detected_bpm;
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

void LoadTrack(const char* filename, const char* fullpath) {
    snprintf(deckA.title, 64, "%s", filename);
    snprintf(deckA.full_path, 256, "%s", fullpath);
    
    char id3_title[64] = {0};
    float rekordbox_bpm = ReadRekordboxBPM(fullpath, id3_title);

    if (rekordbox_bpm > 0.0f) {
        deckA.base_bpm = rekordbox_bpm;
        deckA.bpm_found = 1;
    } else {
        deckA.base_bpm = 120.0f; // Fallback se la traccia non e stata analizzata su Rekordbox
        deckA.bpm_found = 0;
    }

    if (strlen(id3_title) > 0) {
        snprintf(deckA.title, 64, "%s", id3_title);
    }

    deckA.pitch = 0.0f;
    deckA.current_bpm = deckA.base_bpm;
    deckA.progress = 0.0f;
    deckA.is_playing = 1;
}

int main() {
    SetupCallbacks();
    ShowSplashScreen();

    pspAudioInit();
    audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_BUFFER_SIZE, PSP_AUDIO_FORMAT_STEREO);

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

            if (pressed & PSP_CTRL_LTRIGGER) {
                deckA.pitch -= step;
                if (deckA.pitch < -16.0f) deckA.pitch = -16.0f;
            }
            if (pressed & PSP_CTRL_RTRIGGER) {
                deckA.pitch += step;
                if (deckA.pitch > 16.0f) deckA.pitch = 16.0f;
            }
            if (pressed & PSP_CTRL_CROSS) {
                deckA.is_playing = !deckA.is_playing;
            }

            // Calcolo preciso BPM dinamico basato sul Pitch %
            deckA.current_bpm = deckA.base_bpm * (1.0f + (deckA.pitch / 100.0f));
        }

        if (deckA.is_playing) {
            deckA.progress += 0.1f;
            if (deckA.progress > 100.0f) deckA.progress = 0.0f;
        }

        last_buttons = pad.Buttons;

        // --- RENDER INTERFACCIA ---
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
            pspDebugScreenPrintf("                PSPBox DJ - DECK A                \n");
            pspDebugScreenPrintf("==================================================\n\n\n");

            pspDebugScreenPrintf("   TRACCIA :  %s\n\n", deckA.title);
            pspDebugScreenPrintf("   BPM     :  %.1f  (Base: %.1f) [%s]\n\n", 
                                 deckA.current_bpm, 
                                 deckA.base_bpm, 
                                 deckA.bpm_found ? "REKORDBOX OK" : "DEFAULT 120");
            pspDebugScreenPrintf("   PITCH   :  %+.1f%%\n\n", deckA.pitch);
            pspDebugScreenPrintf("   STATO   :  [%s]\n\n\n", deckA.is_playing ? " PLAYING " : " PAUSED  ");

            pspDebugScreenPrintf("   [");
            int bar_pos = (int)((deckA.progress / 100.0f) * 30);
            for (int i = 0; i < 30; i++) {
                if (i == bar_pos) pspDebugScreenPrintf(">");
                else if (i < bar_pos) pspDebugScreenPrintf("=");
                else pspDebugScreenPrintf(" ");
            }
            pspDebugScreenPrintf("]  %.0f%%\n\n\n", deckA.progress);

            pspDebugScreenPrintf("--------------------------------------------------\n");
            pspDebugScreenPrintf(" [TRIANGOLO]: Browser | [X]: Play/Pause | [L/R]: Pitch\n");
        }

        sceDisplayWaitVblankStart();
    }

    if (deckA.file_handle >= 0) sceIoClose(deckA.file_handle);
    if (audio_channel >= 0) sceAudioChRelease(audio_channel);
    pspAudioEnd();

    return 0;
}
