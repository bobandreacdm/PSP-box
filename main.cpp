#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

PSP_MODULE_INFO("PSPBox", 0, 1, 3);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define WAVEFORM_WIDTH 30

typedef struct {
    char title[64];
    float pitch;
    int is_playing;
    float progress;
    char waveform[WAVEFORM_WIDTH + 1]; // Buffer per la waveform rapida
} DeckState;

DeckState deckA = {"Nessun brano", 0.0f, 0, 0.0f, "------------------------------"};

int browser_open = 0;
char file_list[10][64];
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

// Generatore rapido di Waveform (analizza il nome o il file al volo)
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

void ScanMusicFolder() {
    file_count = 0;
    DIR *dir = opendir("ms0:/MUSIC");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && file_count < 10) {
            if (ent->d_name[0] != '.') {
                strncpy(file_list[file_count], ent->d_name, 63);
                file_count++;
            }
        }
        closedir(dir);
    }
    
    if (file_count == 0) {
        strcpy(file_list[0], "traccia_test.wav");
        file_count = 1;
    }
}

int main() {
    pspDebugScreenInit();
    SetupCallbacks();

    // Inizializzazione Hardware Audio PSP
    pspAudioInit();
    audio_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 2048, PSP_AUDIO_FORMAT_STEREO);

    ScanMusicFolder();

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
                strncpy(deckA.title, file_list[selected_index], 63);
                GenerateQuickWaveform(deckA.title); // Genera la waveform al volo!
                deckA.progress = 0.0f;
                browser_open = 0;
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
        pspDebugScreenPrintf("==================================================\n");
        pspDebugScreenPrintf("  PSPBox DJ Engine v1.3 - Quick Waveform & Audio  \n");
        pspDebugScreenPrintf("==================================================\n\n");

        if (browser_open) {
            pspDebugScreenPrintf(" === FILE BROWSER (ms0:/MUSIC/) ===\n\n");
            for (int i = 0; i < file_count; i++) {
                if (i == selected_index) pspDebugScreenPrintf(" > %s <\n", file_list[i]);
                else pspDebugScreenPrintf("   %s  \n", file_list[i]);
            }
        } else {
            pspDebugScreenPrintf(" [ DECK A ]\n");
            pspDebugScreenPrintf(" Traccia : %s\n", deckA.title);
            pspDebugScreenPrintf(" Status  : [%s]   |   Pitch: %+.1f%%\n\n", 
                                 deckA.is_playing ? "PLAYING" : "PAUSED ", deckA.pitch);

            pspDebugScreenPrintf(" --- RAPID WAVEFORM GENERATOR ---\n ");
            pspDebugScreenPrintf("%s\n", deckA.waveform);
            
            // Cursore di riproduzione sulla waveform
            int pos = (int)((deckA.progress / 100.0f) * WAVEFORM_WIDTH);
            pspDebugScreenPrintf(" ");
            for (int i = 0; i < WAVEFORM_WIDTH; i++) {
                if (i == pos) pspDebugScreenPrintf("^");
                else pspDebugScreenPrintf("-");
            }
            pspDebugScreenPrintf("  (%.0f%%)\n\n", deckA.progress);

            pspDebugScreenPrintf(" [Triangle]: Browser  |  [X]: Play/Pause  |  [L/R]: Pitch\n");
        }

        sceDisplayWaitVblankStart();
    }

    if (audio_channel >= 0) sceAudioChRelease(audio_channel);
    pspAudioEnd();

    return 0;
}
