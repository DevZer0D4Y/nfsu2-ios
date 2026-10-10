/*
 * ios_touch.m -- on-screen controls for playing without a game controller.
 *
 * A transparent UIView over SDL's Metal view draws the controls and feeds a
 * virtual SDL game controller (SDL_JoystickAttachVirtualEx). The title reads
 * it through the ordinary SDL controller path (xinput_device.c), exactly as
 * it reads a DualSense, so nothing below the host knows about touch.
 *
 * Only one of the two is attached at a time: when a physical controller
 * connects, the overlay hides and the virtual controller is detached, so the
 * real one takes port 0; when the last one goes away, both come back.
 *
 *   left half        floating stick (left thumbstick): it centres where the
 *                    finger lands
 *   top left         D-pad
 *   top centre       BACK, START
 *   right            A B X Y, WHITE / BLACK (the shoulder buttons in SDL
 *                    terms), BRAKE (left trigger), GAS (right trigger)
 *
 * NFSU2_TOUCH=0 (Documents/nfsu2x_env.txt) turns the overlay off, =2 keeps
 * it on even with a controller (the virtual one then takes the next port).
 * The iOS Simulator always reports a controller of its own ("Gamepad").
 * Main thread only, like everything UIKit; SDL's joystick calls take SDL's
 * joystick lock, so the title's thread may read the controller meanwhile.
 */
#import <UIKit/UIKit.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum CtlKind { CTL_BUTTON, CTL_TRIGGER };

typedef struct {
    enum CtlKind kind;
    int          target;        /* SDL_GameControllerButton or _Axis */
    const char  *label;
    int          round;         /* circle (face buttons) or rounded rect */
} CtlDef;

static const CtlDef k_ctls[] = {
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_A,             "A",     1 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_B,             "B",     1 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_X,             "X",     1 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_Y,             "Y",     1 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  "WHITE", 0 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "BLACK", 0 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_BACK,          "BACK",  0 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_START,         "START", 0 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_DPAD_UP,       "\u25B2\uFE0E", 0 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_DPAD_DOWN,     "\u25BC\uFE0E", 0 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_DPAD_LEFT,     "\u25C0\uFE0E", 0 },
    { CTL_BUTTON,  SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    "\u25B6\uFE0E", 0 },
    { CTL_TRIGGER, SDL_CONTROLLER_AXIS_TRIGGERLEFT,     "BRAKE", 0 },
    { CTL_TRIGGER, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,    "GAS",   0 },
};
enum { NCTL = sizeof k_ctls / sizeof k_ctls[0], CTL_STICK = -1 };

/* The virtual controller: every button up to the D-pad and all six axes,
 * so SDL's automatic mapping numbers them exactly as the enums do. */
static SDL_Joystick *s_vpad;

static void vpad_attach(void)
{
    SDL_VirtualJoystickDesc d;
    int index;

    if (s_vpad)
        return;
    SDL_zero(d);
    d.version  = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type     = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes    = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    d.name     = "Touch Controls";
    index = SDL_JoystickAttachVirtualEx(&d);
    s_vpad = index >= 0 ? SDL_JoystickOpen(index) : NULL;
    if (!s_vpad)
        fprintf(stderr, "[ios] touch controller: %s\n", SDL_GetError());
}

static void vpad_detach(void)
{
    SDL_JoystickID id;

    if (!s_vpad)
        return;
    id = SDL_JoystickInstanceID(s_vpad);
    SDL_JoystickClose(s_vpad);
    s_vpad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            SDL_JoystickDetachVirtual(i);
            break;
        }
}

static void vpad_axis(int axis, float v)        /* -1..1 (triggers 0..1) */
{
    Sint16 s;

    if (!s_vpad)
        return;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        s = (Sint16)(SDL_JOYSTICK_AXIS_MIN + v * (SDL_JOYSTICK_AXIS_MAX - SDL_JOYSTICK_AXIS_MIN));
    else
        s = (Sint16)(v * SDL_JOYSTICK_AXIS_MAX);
    SDL_JoystickSetVirtualAxis(s_vpad, axis, s);
}

static void vpad_button(int button, int down)
{
    if (s_vpad)
        SDL_JoystickSetVirtualButton(s_vpad, button, down ? SDL_PRESSED : SDL_RELEASED);
}

@interface NFSTouchView : UIView
- (void)releaseAll;
@end

@implementation NFSTouchView {
    CAShapeLayer *_shape[NCTL];
    CATextLayer  *_text[NCTL];
    CGRect        _frame[NCTL];
    int           _held[NCTL];              /* touches holding each control */
    CAShapeLayer *_stickBase, *_stickKnob;
    CGPoint       _stickHome, _stickCentre;
    CGFloat       _stickRadius;
    UITouch      *_stickTouch;
    NSMapTable<UITouch *, NSNumber *> *_bound;   /* touch -> control index */
    UIImpactFeedbackGenerator *_haptic;
}

static CAShapeLayer *make_shape(CALayer *parent)
{
    CAShapeLayer *l = [CAShapeLayer layer];
    l.strokeColor = [UIColor colorWithWhite:1.0 alpha:0.55].CGColor;
    l.fillColor = [UIColor colorWithWhite:1.0 alpha:0.12].CGColor;
    l.lineWidth = 2.0;
    [parent addSublayer:l];
    return l;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (!self)
        return nil;
    self.backgroundColor = UIColor.clearColor;
    self.multipleTouchEnabled = YES;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _bound = [NSMapTable weakToStrongObjectsMapTable];
    _haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    for (int i = 0; i < NCTL; i++) {
        _shape[i] = make_shape(self.layer);
        _text[i] = [CATextLayer layer];
        _text[i].string = [NSString stringWithUTF8String:k_ctls[i].label];
        _text[i].fontSize = 13;
        _text[i].font = (__bridge CFTypeRef)[UIFont boldSystemFontOfSize:13];   /* a CTFont */
        _text[i].foregroundColor = [UIColor colorWithWhite:1.0 alpha:0.8].CGColor;
        _text[i].alignmentMode = kCAAlignmentCenter;
        _text[i].contentsScale = UIScreen.mainScreen.scale;
        [self.layer addSublayer:_text[i]];
    }
    _stickBase = make_shape(self.layer);
    _stickKnob = make_shape(self.layer);
    _stickKnob.fillColor = [UIColor colorWithWhite:1.0 alpha:0.35].CGColor;
    return self;
}

- (void)placeControl:(int)i rect:(CGRect)r
{
    CGFloat lh = 16;

    _frame[i] = r;
    _shape[i].path = (k_ctls[i].round
                      ? [UIBezierPath bezierPathWithOvalInRect:r]
                      : [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:10]).CGPath;
    _text[i].frame = CGRectMake(r.origin.x, CGRectGetMidY(r) - lh / 2, r.size.width, lh);
}

- (void)drawStick
{
    CGFloat r = _stickRadius, k = r * 0.45;
    _stickBase.path = [UIBezierPath bezierPathWithOvalInRect:
                       CGRectMake(_stickCentre.x - r, _stickCentre.y - r, 2 * r, 2 * r)].CGPath;
    if (!_stickTouch) {
        _stickKnob.path = [UIBezierPath bezierPathWithOvalInRect:
                           CGRectMake(_stickCentre.x - k, _stickCentre.y - k, 2 * k, 2 * k)].CGPath;
    }
}

/* Laid out in points inside the safe area, sized for a landscape phone. */
- (void)layoutSubviews
{
    UIEdgeInsets in = self.safeAreaInsets;
    CGFloat L = in.left + 16, R = self.bounds.size.width - in.right - 16;
    CGFloat T = in.top + 12, B = self.bounds.size.height - MAX(in.bottom, 12);
    CGFloat mid = self.bounds.size.width / 2, d = 44, f = 54, s = 40;
    CGFloat dx = L + d * 1.5, dy = T + d * 1.5;         /* D-pad centre */
    CGFloat fx = R - 150, fy = T + 92;                   /* face buttons centre */

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [super layoutSubviews];
    for (int i = 0; i < NCTL; i++) {
        CGRect r = CGRectZero;
        switch (k_ctls[i].target + (k_ctls[i].kind == CTL_TRIGGER ? 100 : 0)) {
        case SDL_CONTROLLER_BUTTON_Y:             r = CGRectMake(fx - f / 2, fy - f * 1.35, f, f); break;
        case SDL_CONTROLLER_BUTTON_A:             r = CGRectMake(fx - f / 2, fy + f * 0.35, f, f); break;
        case SDL_CONTROLLER_BUTTON_X:             r = CGRectMake(fx - f * 1.35, fy - f / 2, f, f); break;
        case SDL_CONTROLLER_BUTTON_B:             r = CGRectMake(fx + f * 0.35, fy - f / 2, f, f); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  r = CGRectMake(fx - f * 1.35 - 84, fy - s - 5, 60, s); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: r = CGRectMake(fx - f * 1.35 - 84, fy + 5, 60, s); break;
        case SDL_CONTROLLER_BUTTON_BACK:          r = CGRectMake(mid - 84, T, 72, 32); break;
        case SDL_CONTROLLER_BUTTON_START:         r = CGRectMake(mid + 12, T, 72, 32); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       r = CGRectMake(dx - d / 2, dy - d * 1.5, d, d); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     r = CGRectMake(dx - d / 2, dy + d * 0.5, d, d); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     r = CGRectMake(dx - d * 1.5, dy - d / 2, d, d); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    r = CGRectMake(dx + d * 0.5, dy - d / 2, d, d); break;
        case 100 + SDL_CONTROLLER_AXIS_TRIGGERRIGHT: r = CGRectMake(R - 100, B - 140, 100, 140); break;
        case 100 + SDL_CONTROLLER_AXIS_TRIGGERLEFT:  r = CGRectMake(R - 216, B - 100, 100, 100); break;
        }
        [self placeControl:i rect:r];
    }
    _stickRadius = 64;
    _stickHome = CGPointMake(L + _stickRadius + 40, B - _stickRadius - 16);
    if (!_stickTouch)
        _stickCentre = _stickHome;
    [self drawStick];
    [CATransaction commit];
}

- (int)controlAt:(CGPoint)p
{
    for (int i = 0; i < NCTL; i++)
        if (CGRectContainsPoint(CGRectInset(_frame[i], -6, -6), p))
            return i;
    return p.x < self.bounds.size.width * 0.45 ? CTL_STICK : -2;
}

- (void)setHeld:(int)i delta:(int)delta
{
    int was = _held[i] > 0;

    _held[i] += delta;
    if (_held[i] < 0)
        _held[i] = 0;
    if ((_held[i] > 0) == was)
        return;
    if (!was)
        [_haptic impactOccurred];
    if (k_ctls[i].kind == CTL_TRIGGER)
        vpad_axis(k_ctls[i].target, was ? 0.0f : 1.0f);
    else
        vpad_button(k_ctls[i].target, !was);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _shape[i].fillColor = [UIColor colorWithWhite:1.0 alpha:was ? 0.12 : 0.45].CGColor;
    [CATransaction commit];
}

- (void)moveStick:(CGPoint)p
{
    CGFloat dx = p.x - _stickCentre.x, dy = p.y - _stickCentre.y;
    CGFloat len = sqrt(dx * dx + dy * dy), r = _stickRadius, k = r * 0.45;

    if (len > r) {
        dx *= r / len;
        dy *= r / len;
    }
    vpad_axis(SDL_CONTROLLER_AXIS_LEFTX, (float)(dx / r));
    vpad_axis(SDL_CONTROLLER_AXIS_LEFTY, (float)(dy / r));
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _stickKnob.path = [UIBezierPath bezierPathWithOvalInRect:
                       CGRectMake(_stickCentre.x + dx - k, _stickCentre.y + dy - k, 2 * k, 2 * k)].CGPath;
    [CATransaction commit];
}

- (void)endStick
{
    _stickTouch = nil;
    _stickCentre = _stickHome;
    vpad_axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
    vpad_axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [self drawStick];
    [CATransaction commit];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        int c = [self controlAt:p];
        if (c >= 0) {
            [_bound setObject:@(c) forKey:t];
            [self setHeld:c delta:+1];
        } else if (c == CTL_STICK && !_stickTouch) {
            CGFloat r = _stickRadius;
            _stickTouch = t;
            _stickCentre = CGPointMake(MAX(p.x, r), MIN(MAX(p.y, r), self.bounds.size.height - r));
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [self drawStick];
            [CATransaction commit];
            [self moveStick:p];
        }
    }
}

/* A finger that slides from one control onto another (BRAKE to GAS, say)
 * moves the press with it; sliding off onto nothing keeps it held. */
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        NSNumber *b;
        int c;

        if (t == _stickTouch) {
            [self moveStick:p];
            continue;
        }
        b = [_bound objectForKey:t];
        if (!b)
            continue;
        c = [self controlAt:p];
        if (c >= 0 && c != b.intValue) {
            [self setHeld:b.intValue delta:-1];
            [_bound setObject:@(c) forKey:t];
            [self setHeld:c delta:+1];
        }
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    for (UITouch *t in touches) {
        NSNumber *b = [_bound objectForKey:t];
        if (t == _stickTouch)
            [self endStick];
        if (b) {
            [self setHeld:b.intValue delta:-1];
            [_bound removeObjectForKey:t];
        }
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
    [self touchesEnded:touches withEvent:event];
}

- (void)releaseAll
{
    for (int i = 0; i < NCTL; i++)
        if (_held[i])
            [self setHeld:i delta:-_held[i]];
    [_bound removeAllObjects];
    if (_stickTouch)
        [self endStick];
}
@end

static NFSTouchView *s_view;
static int           s_always;          /* NFSU2_TOUCH=2 */

/* A game controller that is not ours. */
static int physical_controllers(void)
{
    int n = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (!SDL_JoystickIsVirtual(i) && SDL_IsGameController(i))
            n++;
    return n;
}

void ios_touch_controllers_changed(void)
{
    int show;

    if (!s_view)
        return;
    show = s_always || physical_controllers() == 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        fprintf(stderr, "[ios] joystick %d: %s%s%s\n", i,
                SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "?",
                SDL_JoystickIsVirtual(i) ? " (virtual)" : "",
                SDL_IsGameController(i) ? "" : " (not a game controller)");
    if (show == !s_view.hidden)
        return;
    if (show) {
        vpad_attach();
    } else {
        [s_view releaseAll];
        vpad_detach();
    }
    s_view.hidden = !show;
    fprintf(stderr, "[ios] touch controls %s\n", show ? "on" : "off (controller connected)");
}

/* Called once SDL's Metal view exists (the Vulkan surface is made on the
 * main thread), so the overlay goes on top of it. */
void ios_touch_attach(SDL_Window *window)
{
    SDL_SysWMinfo info;
    const char *e = getenv("NFSU2_TOUCH");
    UIView *root;

    if (e && strcmp(e, "0") == 0)
        return;
    s_always = e && strcmp(e, "2") == 0;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT)
        return;
    root = info.info.uikit.window.rootViewController.view;
    if (!root)
        return;
    if (!s_view) {
        s_view = [[NFSTouchView alloc] initWithFrame:root.bounds];
        s_view.hidden = YES;
        [root addSubview:s_view];
    }
    [root bringSubviewToFront:s_view];
    ios_touch_controllers_changed();
}
