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

PSP_MODULE_INFO("PSPBox", 0, 1, 4);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define WAVEFORM_WIDTH 40

typedef struct {
    char name[64];
    int is_dir;
} FileItem;

typedef struct {
    char title[64];
    float pitch;
    int is_playing;
    float progress;
    char waveform[WAVEFORM_WIDTH + 1];
} DeckState;

DeckState deckA = {"Nessun brano", 0.0f, 0, 0.0f, "----------------------------------------"};

int browser_open = 0;
char current_path[256] = "ms0:/MUSIC";
FileItem file_list[20];
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

void GenerateQuickWaveform(const char* filename) {
    int seed = 0;
    for (int i = 0; filename[i] != '\0'; i++) seed += filename[i];
    
    const char bars[] = " |_|i|I|#|";
    for (int i = 0; i < WAVEFORM_WIDTH; i++) {
        int val = (seed + i * 7) % 5;
        deckA.waveform[i] = bars[val];
    }
    deckA.waveform[WAVEFORM_WIDTH] = '\0';
}

void ScanPath(const char* path) {
    file_count = 0;
    
    // Aggiungiamo la voce ".." per tornare indietro se non siamo alla radice
    if (strcmp(path, "ms0:") != 0 && strcmp(path, "ms0:/") != 0 && strcmp(path, "ms0:/MUSIC") != 0) {
        snprintf(file_list[0].name, 64, "..");
        file_list[0].is_dir = 1;
        file_count = 1;
    }

    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && file_count < 20) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            
            snprintf(file_list[file_count].name, 64, "%s", ent->d_name);
            
            // Verifica tipo file/cartella usando sceIoGetstat
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
        snprintf(file_list[0].name, 64, "Nessun file trovato");
        file_list[0].is_dir = 0;
        file_count = 1;
    }
    selected_index = 0;
}

int main() {
    pspDebugScreenInit();
    SetupCallbacks();

    pspAudioInit();
    audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 2048, PSP_AUDIO_FORMAT_STEREO);

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
                    snprintf(deckA.title, 64, "%s", file_list[selected_index].name);
                    GenerateQuickWaveform(deckA.title);
                    deckA.progress = 0.0f;
                    browser_open = 0;
                }
            }
        } else {
            int shift = (pad.Buttons & PSP_CTRL_CIRCLE) ? 1 : 0;
            float step = shift ? 1.0f : 0.1f;

            if (pressed & PSP_CTRL_LTRIGGER) {
                deckA.pitch -= step;
                if (deckA.pitch < -10.0f) deckA.pitch = -10.0f;
            }
            if (pressed & PSP_CTRL_RTRIGGER) {
                deckA.pitch += step;
                if (deckA.pitch > 10.0f) deckA.pitch = 10.0f;
            }
            if (pressed & PSP_CTRL_CROSS) {
                deckA.is_playing = !deckA.is_playing;
            }
        }

        if (deckA.is_playing) {
            deckA.progress += 0.2f;
            if (deckA.progress > 100.0f) deckA.progress = 0.0f;
        }

        last_buttons = pad.Buttons;

        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("+------------------------------------------------+\n");
        pspDebugScreenPrintf("|            PSPBox DJ Engine v1.4               |\n");
        pspDebugScreenPrintf("+------------------------------------------------+\n\n");

        if (browser_open) {
            pspDebugScreenPrintf(" BROWSER: %s\n", current_path);
            pspDebugScreenPrintf("--------------------------------------------------\n");
            for (int i = 0; i < file_count; i++) {
                if (i == selected_index) {
                    pspDebugScreenPrintf(" > %s %s <\n", file_list[i].is_dir ? "[DIR]" : "[TRK]", file_list[i].name);
                } else {
                    pspDebugScreenPrintf("   %s %s  \n", file_list[i].is_dir ? "[DIR]" : "[TRK]", file_list[i].name);
                }
            }
            pspDebugScreenPrintf("--------------------------------------------------\n");
            pspDebugScreenPrintf(" [X] Entra / Seleziona  |  [Triangolo] Chiudi\n");
        } else {
            pspDebugScreenPrintf(" DECK A: %s\n", deckA.title);
            pspDebugScreenPrintf(" STATUS: %s  |  PITCH: %+.1f%%\n\n", 
                                 deckA.is_playing ? ">> PLAY" : "|| PAUSE", deckA.pitch);

            pspDebugScreenPrintf(" --- WAVEFORM DISPLAY ---\n ");
            pspDebugScreenPrintf("%s\n ", deckA.waveform);
            
            int pos = (int)((deckA.progress / 100.0f) * WAVEFORM_WIDTH);
            for (int i = 0; i < WAVEFORM_WIDTH; i++) {
                if (i == pos) pspDebugScreenPrintf("^");
                else pspDebugScreenPrintf("-");
            }
            pspDebugScreenPrintf("  [%.0f%%]\n\n", deckA.progress);

            pspDebugScreenPrintf("--------------------------------------------------\n");
            pspDebugScreenPrintf(" [Triangolo] Browser | [X] Play/Pause | [L/R] Pitch\n");
        }

        sceDisplayWaitVblankStart();
    }

    if (audio_channel >= 0) sceAudioChRelease(audio_channel);
    pspAudioEnd();

    return 0;
}
