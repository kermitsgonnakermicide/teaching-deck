#ifndef INPUT_H
#define INPUT_H

#include <pspctrl.h>

#define INPUT_POLL_HZ 60

typedef struct {
    unsigned int buttons;      // bitmask of pressed buttons
    int analog_x;              // -128 to 127
    int analog_y;              // -128 to 127
    int changed;               // nonzero if state changed since last poll
} InputState;

// Button bitmasks matching PSP SDK
#define BTN_SELECT   PSP_CTRL_SELECT
#define BTN_START    PSP_CTRL_START
#define BTN_UP       PSP_CTRL_UP
#define BTN_RIGHT    PSP_CTRL_RIGHT
#define BTN_DOWN     PSP_CTRL_DOWN
#define BTN_LEFT     PSP_CTRL_LEFT
#define BTN_L        PSP_CTRL_LTRIGGER
#define BTN_R        PSP_CTRL_RTRIGGER
#define BTN_TRIANGLE PSP_CTRL_TRIANGLE
#define BTN_CIRCLE   PSP_CTRL_CIRCLE
#define BTN_CROSS    PSP_CTRL_CROSS
#define BTN_SQUARE   PSP_CTRL_SQUARE
#define BTN_HOME     PSP_CTRL_HOME
#define BTN_HOLD     PSP_CTRL_HOLD
#define BTN_NOTE     PSP_CTRL_NOTE

int input_init(void);
void input_poll(InputState *state);
int input_any_pressed(const InputState *state);
void input_print_state(const InputState *state);

#endif
