/*
 * xinput_nx.c -- the Switch's own controllers as the Xbox gamepad.
 *
 * Read with libnx's pad API rather than SDL. The face buttons go by label
 * by default -- Switch A is Xbox A -- because that is what the title's
 * on-screen prompts name: a positional layout makes "(B) Back" the Switch's
 * A button, and a player pressing B in a modal box (NFSU2's Help, opened
 * with Y) sees the game ignore them. RECOMP_PAD_LAYOUT=position (settable in
 * nfsu2x_env.txt) swaps to the Xbox positions instead: A<->B, X<->Y. Kept in
 * its own file because <switch.h> and the Win32 vocabulary the rest of the
 * runtime uses define some of the same names.
 *
 *   Xbox            Switch (default)   Switch (position)
 *   A               A                  B
 *   B               B                  A
 *   X               X                  Y
 *   Y               Y                  X
 *   White / Black   L / R
 *   LT / RT         ZL / ZR   (digital on the Switch: 0 or 255)
 *   Start / Back    + / -
 *   sticks, clicks, D-pad as they are
 *
 * Handheld mode and the first player's controller (Joy-Con pair or Pro
 * Controller) both drive port 0; player 2's controller drives port 1 (the
 * second USB pad, for split screen). Each gets its own port's rumble
 * (xbox_nx_pad_rumble): the Xbox's left, heavy motor as the low band, its
 * right, light motor as the high band, on both sides of the pad.
 *
 * RECOMP_NX_JOYCON=single puts players 1 and 2 in single Joy-Con mode, so
 * the two detached Joy-Cons are one player each, held sideways:
 *
 *   Xbox            sideways Joy-Con (either side)
 *   A / B / X / Y   right / bottom / top / left face button (Nintendo's
 *                   A B X Y positions, so the labels still match)
 *   LT / RT         SL / SR
 *   Start           - (left Joy-Con) or + (right)
 *   left stick      the stick, turned with the Joy-Con
 *
 * RECOMP_NX_JOYCON_ROTATE=0 leaves the stick unturned, in case the system
 * already turns it.
 */
#ifdef __SWITCH__
#include <switch.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xinput_nx.h"

#define NX_PADS 2

static PadState s_pad[NX_PADS];     /* player 1 (handheld + No1), player 2 */
static int      s_ready;
static Mutex    s_lock;
static int      s_positional;
static int      s_rotate = 1;

static void nx_pad_init(void)
{
    const char *layout = getenv("RECOMP_PAD_LAYOUT");
    const char *joycon = getenv("RECOMP_NX_JOYCON");
    const char *rotate = getenv("RECOMP_NX_JOYCON_ROTATE");

    s_positional = layout && strcmp(layout, "position") == 0;
    s_rotate = !(rotate && strcmp(rotate, "0") == 0);
    padConfigureInput(NX_PADS, HidNpadStyleSet_NpadStandard);
    if (joycon && strcmp(joycon, "single") == 0) {
        hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
        hidSetNpadJoyAssignmentModeSingleByDefault(HidNpadIdType_No1);
        hidSetNpadJoyAssignmentModeSingleByDefault(HidNpadIdType_No2);
    }
    padInitializeDefault(&s_pad[0]);
    padInitialize(&s_pad[1], HidNpadIdType_No2);
    s_ready = 1;
}

/* One Joy-Con held sideways, rail up: the face buttons as Nintendo places
 * them (right = A, bottom = B, top = X, left = Y) and the stick turned a
 * quarter with the controller. */
static void nx_sideways(u32 style, u64 *b, HidAnalogStickState *stick,
                        HidAnalogStickState l, HidAnalogStickState r)
{
    u64 in = *b, out = 0;
    s32 x, y;

    if (style & HidNpadStyleTag_NpadJoyLeft) {
        /* turned anticlockwise: Down is on the right, Left at the bottom */
        if (in & HidNpadButton_Down)   out |= HidNpadButton_A;
        if (in & HidNpadButton_Left)   out |= HidNpadButton_B;
        if (in & HidNpadButton_Right)  out |= HidNpadButton_X;
        if (in & HidNpadButton_Up)     out |= HidNpadButton_Y;
        if (in & HidNpadButton_LeftSL) out |= HidNpadButton_ZL;
        if (in & HidNpadButton_LeftSR) out |= HidNpadButton_ZR;
        if (in & HidNpadButton_Minus)  out |= HidNpadButton_Plus;
        if (in & HidNpadButton_StickL) out |= HidNpadButton_StickL;
        x = s_rotate ? -l.y : l.x;
        y = s_rotate ?  l.x : l.y;
    } else {
        /* turned clockwise: X is on the right, A at the bottom */
        if (in & HidNpadButton_X)       out |= HidNpadButton_A;
        if (in & HidNpadButton_A)       out |= HidNpadButton_B;
        if (in & HidNpadButton_Y)       out |= HidNpadButton_X;
        if (in & HidNpadButton_B)       out |= HidNpadButton_Y;
        if (in & HidNpadButton_RightSL) out |= HidNpadButton_ZL;
        if (in & HidNpadButton_RightSR) out |= HidNpadButton_ZR;
        if (in & HidNpadButton_Plus)    out |= HidNpadButton_Plus;
        if (in & HidNpadButton_StickR)  out |= HidNpadButton_StickL;
        x = s_rotate ?  r.y : r.x;
        y = s_rotate ? -r.x : r.y;
    }
    if (x < -32767) x = -32767;
    if (y < -32767) y = -32767;
    stick->x = x;
    stick->y = y;
    *b = out;
}

int xbox_nx_pad_read(unsigned port, uint16_t *digital, uint8_t analog[8],
                     int16_t thumbs[4])
{
    u64 b;
    u32 style;
    HidAnalogStickState l, r;
    PadState *pad;

    if (port >= NX_PADS)
        return 0;
    pad = &s_pad[port];
    mutexLock(&s_lock);
    if (!s_ready)
        nx_pad_init();
    padUpdate(pad);
    b = padGetButtons(pad);
    l = padGetStickPos(pad, 0);
    r = padGetStickPos(pad, 1);
    style = padGetStyleSet(pad);
    mutexUnlock(&s_lock);

    if (!padIsConnected(pad))
        return 0;

    /* A single Joy-Con (single mode): sideways, one stick. */
    if (!(style & (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld
                   | HidNpadStyleTag_NpadJoyDual))
            && (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight))) {
        nx_sideways(style, &b, &l, l, r);
        r.x = r.y = 0;
    }

    *digital = 0;
    if (b & HidNpadButton_Up)     *digital |= 0x0001;
    if (b & HidNpadButton_Down)   *digital |= 0x0002;
    if (b & HidNpadButton_Left)   *digital |= 0x0004;
    if (b & HidNpadButton_Right)  *digital |= 0x0008;
    if (b & HidNpadButton_Plus)   *digital |= 0x0010;   /* Start */
    if (b & HidNpadButton_Minus)  *digital |= 0x0020;   /* Back */
    if (b & HidNpadButton_StickL) *digital |= 0x0040;
    if (b & HidNpadButton_StickR) *digital |= 0x0080;

    memset(analog, 0, 8);
    if (s_positional) {
        analog[NX_XBOX_A] = (b & HidNpadButton_B)  ? 255 : 0;
        analog[NX_XBOX_B] = (b & HidNpadButton_A)  ? 255 : 0;
        analog[NX_XBOX_X] = (b & HidNpadButton_Y)  ? 255 : 0;
        analog[NX_XBOX_Y] = (b & HidNpadButton_X)  ? 255 : 0;
    } else {
        analog[NX_XBOX_A] = (b & HidNpadButton_A)  ? 255 : 0;
        analog[NX_XBOX_B] = (b & HidNpadButton_B)  ? 255 : 0;
        analog[NX_XBOX_X] = (b & HidNpadButton_X)  ? 255 : 0;
        analog[NX_XBOX_Y] = (b & HidNpadButton_Y)  ? 255 : 0;
    }
    analog[NX_XBOX_BLACK] = (b & HidNpadButton_R)  ? 255 : 0;
    analog[NX_XBOX_WHITE] = (b & HidNpadButton_L)  ? 255 : 0;
    analog[NX_XBOX_LT]    = (b & HidNpadButton_ZL) ? 255 : 0;
    analog[NX_XBOX_RT]    = (b & HidNpadButton_ZR) ? 255 : 0;

    /* libnx sticks are +-32767 with +y up, the Xbox's convention too. */
    thumbs[0] = (int16_t)l.x;
    thumbs[1] = (int16_t)l.y;
    thumbs[2] = (int16_t)r.x;
    thumbs[3] = (int16_t)r.y;
    return 1;
}

/* Vibration handles for one input source, re-fetched when its style changes
 * (Joy-Con pair swapped for a Pro Controller, say). */
typedef struct {
    HidNpadIdType            id;
    u32                      style;     /* HidNpadStyleTag, 0 = none */
    s32                      count;
    HidVibrationDeviceHandle handles[2];
} NxRumbleTarget;

static int nx_rumble_refresh(NxRumbleTarget *t, int active)
{
    u32 styles = active ? hidGetNpadStyleSet(t->id) : 0;
    u32 style = 0;
    s32 count = 2;

    if (t->id == HidNpadIdType_Handheld) {
        if (styles & HidNpadStyleTag_NpadHandheld) style = HidNpadStyleTag_NpadHandheld;
    } else if (styles & HidNpadStyleTag_NpadFullKey) {
        style = HidNpadStyleTag_NpadFullKey;
    } else if (styles & HidNpadStyleTag_NpadJoyDual) {
        style = HidNpadStyleTag_NpadJoyDual;
    } else if (styles & HidNpadStyleTag_NpadJoyLeft) {
        style = HidNpadStyleTag_NpadJoyLeft;  count = 1;
    } else if (styles & HidNpadStyleTag_NpadJoyRight) {
        style = HidNpadStyleTag_NpadJoyRight; count = 1;
    }
    if (style != t->style) {
        t->style = style;
        t->count = 0;
        if (style && R_SUCCEEDED(hidInitializeVibrationDevices(t->handles,
                                        count, t->id, (HidNpadStyleTag)style)))
            t->count = count;
    }
    return t->count;
}

void xbox_nx_pad_rumble(unsigned port, uint16_t left, uint16_t right)
{
    /* Player 1: handheld and No1; player 2: No2. */
    static NxRumbleTarget targets[NX_PADS][2] = {
        { { HidNpadIdType_Handheld, 0, 0, {{0}} },
          { HidNpadIdType_No1,      0, 0, {{0}} } },
        { { HidNpadIdType_No2,      0, 0, {{0}} },
          { HidNpadIdType_No2,      0, 0, {{0}} } },
    };
    static int disabled = -1;
    HidVibrationDeviceHandle handles[4];
    HidVibrationValue values[4];
    HidVibrationValue v;
    int active[2];
    s32 n = 0;
    int i, k;

    if (port >= NX_PADS)
        return;
    if (disabled < 0) {
        const char *e = getenv("RECOMP_RUMBLE");
        disabled = e && strcmp(e, "0") == 0;
    }
    if (disabled)
        return;

    mutexLock(&s_lock);
    if (!s_ready) {                       /* no pad read yet: nothing to shake */
        mutexUnlock(&s_lock);
        return;
    }
    if (port == 0) {
        active[0] = padIsHandheld(&s_pad[0]);
        active[1] = padIsNpadActive(&s_pad[0], HidNpadIdType_No1);
    } else {
        active[0] = padIsConnected(&s_pad[1]);
        active[1] = 0;
    }

    /* Standard HD rumble resonances: 160 Hz low band, 320 Hz high band. */
    v.amp_low   = (float)left  / 65535.0f;
    v.freq_low  = 160.0f;
    v.amp_high  = (float)right / 65535.0f;
    v.freq_high = 320.0f;
    for (i = 0; i < 2; i++) {
        int c = nx_rumble_refresh(&targets[port][i], active[i]);
        for (k = 0; k < c; k++) {
            handles[n] = targets[port][i].handles[k];
            values[n] = v;
            n++;
        }
    }
    if (n)
        hidSendVibrationValues(handles, values, n);
    mutexUnlock(&s_lock);
}
#endif
