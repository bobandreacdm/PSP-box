#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

PSP_MODULE_INFO("PSPBox", 0, 1, 2);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

typedef struct {
    char title[64];
    float pitch;
    int is_playing;
    int is_analyzed;  // 1 = Con Waveform, 0 = Grezzo (No Waveform)
    float progress;   // Progresso brano (0.0 a 100.0%)
} DeckState;

DeckState deckA = {"Nessun brano", 0.0f, 0, 0, 0.0f};

int browser_open = 0;
char file_list[10][64];
int file_count = 0;
int selected_index = 0;

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
        strcpy(file_list[0], "01_Track_Unanalyzed.wav");
        strcpy(file_list[1], "02_Synthwave_Analyzed.wav");
        file_count = 2;
    }
}

int main() {
    pspDebugScreenInit();
    SetupCallbacks();
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
                
                // Controllo simulato: se nel nome c'e' "Analyzed" attiva la waveform, altrimenti no
                if (strstr(deckA.title, "Analyzed") != NULL) {
                    deckA.is_analyzed = 1;
                } else {
                    deckA.is_analyzed = 0;
                }
                
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

        // Avanzamento progresso se in PLAY
        if (deckA.is_playing) {
            deckA.progress += 0.2f;
            if (deckA.progress > 100.0f) deckA.progress = 0.0f;
        }

        last_buttons = pad.Buttons;

        // Render dell'Interfaccia
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("==================================================\n");
        pspDebugScreenPrintf("  PSPBox DJ Engine v1.2 - Unanalyzed Track Mode   \n");
        pspDebugScreenPrintf("==================================================\n\n");

        if (browser_open) {
            pspDebugScreenPrintf(" === FILE BROWSER ===\n\n");
            for (int i = 0; i < file_count; i++) {
                if (i == selected_index) {
                    pspDebugScreenPrintf(" > %s <\n", file_list[i]);
                } else {
                    pspDebugScreenPrintf("   %s  \n", file_list[i]);
                }
            }
        } else {
            pspDebugScreenPrintf(" [ DECK A ]\n");
            pspDebugScreenPrintf(" Traccia : %s\n", deckA.title);
            pspDebugScreenPrintf(" Analisi : [%s]\n", deckA.is_analyzed ? "ANALIZZATO (WAVEFORM)" : "NON ANALIZZATO (GREZZO)");
            pspDebugScreenPrintf(" Status  : [%s]   |   Pitch: %+.1f%%\n\n", 
                                 deckA.is_playing ? "PLAYING" : "PAUSED ", deckA.pitch);

            pspDebugScreenPrintf(" --- VISUALIZZAZIONE SCHERMO ---\n");
            if (deckA.is_analyzed) {
                // Waveform dinamica
                pspDebugScreenPrintf(" |||||||| | | ||||||||||||||||| | | ||||||| \n");
                pspDebugScreenPrintf(" ------------------- ^ --------------------\n\n");
            } else {
                // Barra di progresso semplice per file grezzi
                pspDebugScreenPrintf(" Barra Traccia: [");
                int bars = (int)(deckA.progress / 5.0f);
                for (int b = 0; b < 20; b++) {
                    if (b < bars) pspDebugScreenPrintf("=");
                    else if (b == bars) pspDebugScreenPrintf(">");
                    else pspDebugScreenPrintf(" ");
                }
                pspDebugScreenPrintf("] %.0f%%\n", deckA.progress);
                pspDebugScreenPrintf(" (Nessuna Waveform - Riproduzione Diretta)\n\n");
            }

            pspDebugScreenPrintf(" [Triangle]: Browser  |  [X]: Play/Pause  |  [L/R]: Pitch\n");
        }

        sceDisplayWaitVblankStart();
    }
    return 0;
}
