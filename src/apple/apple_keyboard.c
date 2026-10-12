/*
 * apple_keyboard.c -- the Mac keyboard as an Xbox controller.
 *
 * Like the iPhone's touch controls (src/ios/ios_touch.m), the keyboard drives
 * a virtual SDL game controller (SDL_JoystickAttachVirtualEx), so the title
 * reads it through xinput_device.c like any pad and no shared code changes.
 * It is attached while no physical controller is connected and detached when
 * one is, so a real pad always gets port 0. NFSU2_KEYBOARD=0 turns it off.
 *
 *   arrow keys    d-pad and left stick (menus, steering)
 *   A / D         left stick left / right (steering, for WASD hands)
 *   W / S         right / left trigger (gas / brake, reverse)
 *   Enter         A and Start together ("press START", OK, select, pause)
 *   Esc, Bksp     B (back)
 *   Space         X               Left Shift   Y
 *   Q / E         White / Black   Tab          Back
 *   P             Start           I J K L      right stick
 *
 * Key events arrive on the main thread (apple_main.c's event loop); the
 * title reads the pad state from its own thread, as with the touch pad.
 */
#include <TargetConditionals.h>
#if defined(__APPLE__) && TARGET_OS_OSX

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Joystick *s_pad;
static int s_mode = -1;                  /* 0 off, 1 without a controller, 2 always */
static Uint8 s_held[SDL_NUM_SCANCODES];

enum { BTN, AXIS_NEG, AXIS_POS };

static const struct {
    SDL_Scancode key;
    int kind, target;
} k_map[] = {
    { SDL_SCANCODE_UP,        BTN,      SDL_CONTROLLER_BUTTON_DPAD_UP },
    { SDL_SCANCODE_DOWN,      BTN,      SDL_CONTROLLER_BUTTON_DPAD_DOWN },
    { SDL_SCANCODE_LEFT,      BTN,      SDL_CONTROLLER_BUTTON_DPAD_LEFT },
    { SDL_SCANCODE_RIGHT,     BTN,      SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
    { SDL_SCANCODE_UP,        AXIS_NEG, SDL_CONTROLLER_AXIS_LEFTY },
    { SDL_SCANCODE_DOWN,      AXIS_POS, SDL_CONTROLLER_AXIS_LEFTY },
    { SDL_SCANCODE_LEFT,      AXIS_NEG, SDL_CONTROLLER_AXIS_LEFTX },
    { SDL_SCANCODE_RIGHT,     AXIS_POS, SDL_CONTROLLER_AXIS_LEFTX },
    { SDL_SCANCODE_A,         AXIS_NEG, SDL_CONTROLLER_AXIS_LEFTX },
    { SDL_SCANCODE_D,         AXIS_POS, SDL_CONTROLLER_AXIS_LEFTX },
    { SDL_SCANCODE_W,         AXIS_POS, SDL_CONTROLLER_AXIS_TRIGGERRIGHT },
    { SDL_SCANCODE_S,         AXIS_POS, SDL_CONTROLLER_AXIS_TRIGGERLEFT },
    { SDL_SCANCODE_RETURN,    BTN,      SDL_CONTROLLER_BUTTON_A },
    { SDL_SCANCODE_RETURN,    BTN,      SDL_CONTROLLER_BUTTON_START },
    { SDL_SCANCODE_KP_ENTER,  BTN,      SDL_CONTROLLER_BUTTON_A },
    { SDL_SCANCODE_KP_ENTER,  BTN,      SDL_CONTROLLER_BUTTON_START },
    { SDL_SCANCODE_ESCAPE,    BTN,      SDL_CONTROLLER_BUTTON_B },
    { SDL_SCANCODE_BACKSPACE, BTN,      SDL_CONTROLLER_BUTTON_B },
    { SDL_SCANCODE_SPACE,     BTN,      SDL_CONTROLLER_BUTTON_X },
    { SDL_SCANCODE_LSHIFT,    BTN,      SDL_CONTROLLER_BUTTON_Y },
    /* xinput_device.c: right shoulder = White, left shoulder = Black */
    { SDL_SCANCODE_Q,         BTN,      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
    { SDL_SCANCODE_E,         BTN,      SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
    { SDL_SCANCODE_TAB,       BTN,      SDL_CONTROLLER_BUTTON_BACK },
    { SDL_SCANCODE_P,         BTN,      SDL_CONTROLLER_BUTTON_START },
    { SDL_SCANCODE_I,         AXIS_NEG, SDL_CONTROLLER_AXIS_RIGHTY },
    { SDL_SCANCODE_K,         AXIS_POS, SDL_CONTROLLER_AXIS_RIGHTY },
    { SDL_SCANCODE_J,         AXIS_NEG, SDL_CONTROLLER_AXIS_RIGHTX },
    { SDL_SCANCODE_L,         AXIS_POS, SDL_CONTROLLER_AXIS_RIGHTX },
};

static int mode(void)
{
    if (s_mode < 0) {
        const char *e = getenv("NFSU2_KEYBOARD");
        s_mode = e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1;
    }
    return s_mode;
}

/* Every button and axis from the keys held now: a button is down while any
 * key mapped to it is; an axis is the sum of its keys' directions. */
static void update(void)
{
    int down[SDL_CONTROLLER_BUTTON_MAX] = { 0 };
    int axis[SDL_CONTROLLER_AXIS_MAX] = { 0 };
    size_t i;

    if (!s_pad)
        return;
    for (i = 0; i < sizeof k_map / sizeof k_map[0]; i++) {
        if (!s_held[k_map[i].key])
            continue;
        if (k_map[i].kind == BTN)
            down[k_map[i].target] = 1;
        else
            axis[k_map[i].target] += k_map[i].kind == AXIS_POS ? 1 : -1;
    }
    for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
        SDL_JoystickSetVirtualButton(s_pad, (int)i, down[i] ? SDL_PRESSED : SDL_RELEASED);
    for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
        int v = axis[i] > 0 ? 1 : axis[i] < 0 ? -1 : 0;
        Sint16 s;
        if (i == SDL_CONTROLLER_AXIS_TRIGGERLEFT || i == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            s = v > 0 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN;   /* at rest: MIN */
        else
            s = v > 0 ? SDL_JOYSTICK_AXIS_MAX : v < 0 ? SDL_JOYSTICK_AXIS_MIN + 1 : 0;
        SDL_JoystickSetVirtualAxis(s_pad, (int)i, s);
    }
}

static void attach(void)
{
    SDL_VirtualJoystickDesc d;
    int index;

    if (s_pad)
        return;
    SDL_zero(d);
    d.version  = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type     = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes    = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    d.name     = "Keyboard";
    index = SDL_JoystickAttachVirtualEx(&d);
    s_pad = index >= 0 ? SDL_JoystickOpen(index) : NULL;
    if (!s_pad) {
        fprintf(stderr, "[keyboard] virtual controller: %s\n", SDL_GetError());
        return;
    }
    update();
    fprintf(stderr, "[keyboard] on: arrows/WASD drive (W gas, S brake), Enter = A + Start,"
                    " Esc = B, Space = X, Shift = Y, Q/E = White/Black, Tab = Back, P = Start\n");
}

static void detach(void)
{
    SDL_JoystickID id;
    int i;

    if (!s_pad)
        return;
    id = SDL_JoystickInstanceID(s_pad);
    SDL_JoystickClose(s_pad);
    s_pad = NULL;
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            SDL_JoystickDetachVirtual(i);
            break;
        }
    fprintf(stderr, "[keyboard] off: a controller is connected\n");
}

/* Main thread: at start and whenever a controller comes or goes. */
void apple_keyboard_controllers_changed(void)
{
    int physical = 0, i;

    if (!mode())
        return;
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (!SDL_JoystickIsVirtual(i) && SDL_IsGameController(i))
            physical++;
    if (mode() == 2 || physical == 0)
        attach();
    else
        detach();
}

/* Main thread: every SDL event; keys drive the pad, losing the focus lets
 * go of everything (the key-up would go to another application). */
void apple_keyboard_event(const SDL_Event *e)
{
    if (!s_pad)
        return;
    if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) {
        if (e->key.repeat)
            return;
        s_held[e->key.keysym.scancode] = e->type == SDL_KEYDOWN;
        update();
    } else if (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        SDL_memset(s_held, 0, sizeof s_held);
        update();
    }
}

#endif /* __APPLE__ && TARGET_OS_OSX */
