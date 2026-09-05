#include <string.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <psprtc.h>

#include "input.h"

static unsigned int old_buttons = 0;

int input_init(void) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    old_buttons = 0;
    return 0;
}

void input_poll(InputState *state) {
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);
    
    state->buttons = pad.Buttons;
    state->analog_x = (int)pad.Lx - 128;
    state->analog_y = (int)pad.Ly - 128;
    state->changed = (pad.Buttons != old_buttons);
    
    old_buttons = pad.Buttons;
}

int input_any_pressed(const InputState *state) {
    return state->buttons != 0 || 
           state->analog_x > 20 || state->analog_x < -20 ||
           state->analog_y > 20 || state->analog_y < -20;
}

void input_print_state(const InputState *state) {
    pspDebugScreenSetXY(0, 20);
    pspDebugScreenPrintf("Buttons: 0x%08X  ", state->buttons);
    if (state->buttons & BTN_UP) pspDebugScreenPrintf("UP ");
    if (state->buttons & BTN_DOWN) pspDebugScreenPrintf("DOWN ");
    if (state->buttons & BTN_LEFT) pspDebugScreenPrintf("LEFT ");
    if (state->buttons & BTN_RIGHT) pspDebugScreenPrintf("RIGHT ");
    if (state->buttons & BTN_TRIANGLE) pspDebugScreenPrintf("TRI ");
    if (state->buttons & BTN_CIRCLE) pspDebugScreenPrintf("CIRC ");
    if (state->buttons & BTN_CROSS) pspDebugScreenPrintf("X ");
    if (state->buttons & BTN_SQUARE) pspDebugScreenPrintf("SQ ");
    if (state->buttons & BTN_L) pspDebugScreenPrintf("L ");
    if (state->buttons & BTN_R) pspDebugScreenPrintf("R ");
    if (state->buttons & BTN_START) pspDebugScreenPrintf("START ");
    if (state->buttons & BTN_SELECT) pspDebugScreenPrintf("SEL ");
    pspDebugScreenPrintf("\n");
    
    pspDebugScreenPrintf("Analog: X=%+4d Y=%+4d  ", state->analog_x, state->analog_y);
}
