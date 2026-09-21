#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("PSPBox", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

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

    while (1) {
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("===================================\n");
        pspDebugScreenPrintf("           PSPBOX v1.0             \n");
        pspDebugScreenPrintf("===================================\n\n");
        pspDebugScreenPrintf("Sistema pronto per il testing!\n");
        sceDisplayWaitVblankStart();
    }
    return 0;
}
