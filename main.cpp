#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("PSPBox", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

// Struttura stato Deck
typedef struct {
    float pitch;        // Pitch % (-10.0 a +10.0)
    int is_playing;     // 1 = Play, 0 = Pause
    int cue_set;        // 1 = Cue impostato
    int current_beat;   // Beat attuale per Beat Jump
    int bpm;            // BPM del brano
} DeckState;

DeckState deckA = {0.0f, 0, 1, 1, 128};

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

int main() {
    pspDebugScreenInit();
    SetupCallbacks();

    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        
        // Verifica tasto SHIFT (Cerchio)
        int shift = (pad.Buttons & PSP_CTRL_CIRCLE) ? 1 : 0;

        // Gestione Pitch Slider (L / R Bumpers)
        float step = shift ? 1.0f : 0.1f;
        if (pad.Buttons & PSP_CTRL_LTRIGGER) {
            deckA.pitch -= step;
            if (deckA.pitch < -10.0f) deckA.pitch = -10.0f;
        }
        if (pad.Buttons & PSP_CTRL_RTRIGGER) {
            deckA.pitch += step;
            if (deckA.pitch > 10.0f) deckA.pitch = 10.0f;
        }

        // Render UI Stile Rekordbox
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("==================================================\n");
        pspDebugScreenPrintf("  PSPBox DJ Engine v1.0 - Rekordbox/CDJ Nexus UI \n");
        pspDebugScreenPrintf("==================================================\n\n");

        pspDebugScreenPrintf(" [ DECK A ]  - Track: Synthwave_128BPM.wav\n");
        pspDebugScreenPrintf(" Status: [%s]   |   BPM: %d   |   Beat: %d/4\n", 
                             deckA.is_playing ? "PLAYING" : "PAUSED ", deckA.bpm, deckA.current_beat);
        
        pspDebugScreenPrintf(" Pitch: %+.1f%%  (L/R: fine, Shift+L/R: coarse)\n\n", deckA.pitch);

        pspDebugScreenPrintf(" --- WAVEFORM DISPLAY (480x272 @ 60FPS) ---\n");
        pspDebugScreenPrintf(" |||||||| | | ||||||||||||||||| | | ||||||| \n");
        pspDebugScreenPrintf(" ------------------- ^ --------------------\n\n");

        pspDebugScreenPrintf(" --- CONTROLLI ATTIVI (Ruckus v5) ---\n");
        pspDebugScreenPrintf(" [O Cerchio]: SHIFT Modifier (Stato: %s)\n", shift ? "ATTIVO " : "INATTIVO");
        pspDebugScreenPrintf(" [X Cross]  : Play/Pause | [Square] : Cue Point\n");
        pspDebugScreenPrintf(" [Triangle] : Browser    | [D-Pad]  : Beat Jump\n");

        sceDisplayWaitVblankStart();
    }
    return 0;
}
