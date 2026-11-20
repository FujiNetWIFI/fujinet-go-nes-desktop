/*
 * NESControllersWindow -- the AppKit Controllers panel: the two ports'
 * controllers, each an NES-001 pad (or a dashed empty socket when the port is
 * set to None), over the console strip with its RESET key, all painted from the
 * session's layout (nessession_controls_layout) so every frontend shows the
 * same thing. Then the controller each port has, the gamepads with the player
 * each drives, and Map mode for rebinding. The Keyboard page draws the
 * expansion keyboard (Family BASIC or Subor) from nessession_keyboard_skin_layout.
 * The art follows ~/Workspace/fujinet-go-all/controllers-port/DESIGN-nes.md
 * (colours are the NESSESSION_*_RGB defines, read from photos of the hardware).
 *
 * Gestures: press a button, pill or the RESET key and it is held until it is
 * released or dragged off; press on the cross and drag to steer it (eight
 * 45-degree sectors, nessession_pointer_set; it lets go on release). The
 * pointer is its own input source, so the panel lets go of everything the mouse
 * holds when it is hidden or loses key status (nessession_release_pointer);
 * what the console sees held from any source (the host keyboard, a gamepad,
 * the mouse) is painted in the held colour, so the panel doubles as an input
 * tester for a new gamepad.
 *
 * Keys typed with the panel key go to the console as in the main window
 * (keyboard mode included); no view here takes the keyboard, so the panel
 * itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * more pads than the two rows kept for them change its height (the top edge
 * stays put). Hidden at start, ordered out rather than closed, so its position
 * survives. The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture or
 * choose one from the list (all 23 targets); every control other than the pick
 * dims. Then press a key or a gamepad button; the next one is bound and
 * whatever held it is told so in the hint line. Clear unbinds the pick's key
 * and pad button. Esc backs out of the pick. F9, F11 and F12 are the app's and
 * never bound.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here). Every file-static name carries a NesCtl / NESC_
 * prefix: a generic one can clash with an SDK symbol.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../debugger/DebuggerWindow.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define NESC_UNIT         22.0     /* points per layout unit */
#define NESC_MARGIN       16.0
#define NESC_STAGE_PAD    10.0
#define NESC_TOP_H        40.0     /* the page selector strip */
#define NESC_TITLE_H      18.0
#define NESC_ROW_H        26.0
#define NESC_EFF_W       130.0
#define NESC_POPUP_W     130.0
#define NESC_TYPE_W      150.0
#define NESC_BTN_H        28.0
#define NESC_MIN_PAD_ROWS  2       /* rows kept for pads, more when more are connected */
#define NESC_MAX_PAD_ROWS  4
#define NESC_SLOW_TICKS    8       /* the 30 Hz tick's 250 ms part */
#define NESC_PILL_H       14.0
#define NESC_PILL_W       40.0
#define NESC_MAX_PILLS   128
#define NESC_MAX_CTL      96
#define NESC_MAX_KEYS    256
#define NESC_DIM_ALPHA    0.4      /* controls that are not the Map pick */
#define NESC_PAGE_CONTROLS 0
#define NESC_PAGE_KEYBOARD 1

/* The stage behind the controls, and the captions on it. */
#define NESC_COL_STAGE_TOP   0x6B6C72
#define NESC_COL_STAGE_BOT   0x55565B
#define NESC_COL_STAGE_EDGE  0x3A3B3F
#define NESC_COL_CAPTION     0xEEEEF2
#define NESC_COL_PILL        0x1A1A1C
#define NESC_COL_KEY_HELD    0xFFB04A

/* ---- helpers -------------------------------------------------------------------- */

static NSString *NesCtlStr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *NesCtlKeyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) nessession_keysym_name(keysym, name, (int)sizeof name);
    return NesCtlStr(name);
}

static NSColor *NesCtlRgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *NesCtlRgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

/* Toggled-on look for a push button: accent bezel, white title. */
static void NesCtlSetButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = NESAccentColor();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A linear gradient clipped to a path, from one point to another. */
static void NesCtlFillLinear(NSBezierPath *path, NSPoint from, NSPoint to, uint32_t rgbA, uint32_t rgbB)
{
    const CGFloat comps[8] = {
        ((rgbA >> 16) & 0xff) / 255.0, ((rgbA >> 8) & 0xff) / 255.0, (rgbA & 0xff) / 255.0, 1.0,
        ((rgbB >> 16) & 0xff) / 255.0, ((rgbB >> 8) & 0xff) / 255.0, (rgbB & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views here are flipped, so "top"
 * is the smaller y). */
static void NesCtlFillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    NesCtlFillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMinX(bnd), NSMaxY(bnd)),
                     topRgb, botRgb);
}

/* Text in rect, the font shrunk (to 5 pt) until it fits. */
static void NesCtlDrawText(NSString *text, NSRect rect, CGFloat size, BOOL bold,
                           NSTextAlignment align, NSColor *ink)
{
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = align;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        const NSSize extent = [text sizeWithAttributes:attrs];
        lineHeight = extent.height;
        if ((extent.width <= rect.size.width && lineHeight <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    const CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight) / 2);
    [text drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
}

static NSBezierPath *NesCtlRound(NSRect rect, CGFloat radius)
{
    const CGFloat rad = MIN(radius, MIN(rect.size.width, rect.size.height) / 2);
    return [NSBezierPath bezierPathWithRoundedRect:rect xRadius:rad yRadius:rad];
}

/* A rectangle rounded at its bottom corners only (the console's base band; the
 * view is flipped, so the bottom is the larger y). */
static NSBezierPath *NesCtlBottomRound(NSRect rect, CGFloat radius)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    const CGFloat left = NSMinX(rect), right = NSMaxX(rect), top = NSMinY(rect), bottom = NSMaxY(rect);
    [path moveToPoint:NSMakePoint(left, top)];
    [path lineToPoint:NSMakePoint(right, top)];
    [path lineToPoint:NSMakePoint(right, bottom - radius)];
    [path curveToPoint:NSMakePoint(right - radius, bottom)
         controlPoint1:NSMakePoint(right, bottom)
         controlPoint2:NSMakePoint(right, bottom)];
    [path lineToPoint:NSMakePoint(left + radius, bottom)];
    [path curveToPoint:NSMakePoint(left, bottom - radius)
         controlPoint1:NSMakePoint(left, bottom)
         controlPoint2:NSMakePoint(left, bottom)];
    [path closePath];
    return path;
}

/* A small triangle at ctr pointing along (dirx, diry), a unit step. */
static NSBezierPath *NesCtlTriangle(NSPoint ctr, double dirx, double diry, CGFloat size)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    [path moveToPoint:NSMakePoint(ctr.x + dirx * size, ctr.y + diry * size)];
    [path lineToPoint:NSMakePoint(ctr.x - dirx * size * 0.6 - diry * size, ctr.y - diry * size * 0.6 + dirx * size)];
    [path lineToPoint:NSMakePoint(ctr.x - dirx * size * 0.6 + diry * size, ctr.y - diry * size * 0.6 - dirx * size)];
    [path closePath];
    return path;
}

/* The binding target a Map click on a cross at (dx, dy) from its centre means:
 * the direction's, a diagonal going to its stronger axis; -1 inside the dead
 * centre. dx, dy are in layout units. */
static int NesCtlDirectionTarget(int port, float dx, float dy)
{
    const unsigned mask = nessession_dpad_from_vector(dx, dy);
    if (!mask) return -1;
    int act;
    if (mask & (mask - 1u)) act = fabsf(dx) >= fabsf(dy) ? (dx < 0 ? NES_ACT_LEFT : NES_ACT_RIGHT)
                                                         : (dy < 0 ? NES_ACT_UP : NES_ACT_DOWN);
    else if (mask & (1u << NES_ACT_UP)) act = NES_ACT_UP;
    else if (mask & (1u << NES_ACT_DOWN)) act = NES_ACT_DOWN;
    else if (mask & (1u << NES_ACT_LEFT)) act = NES_ACT_LEFT;
    else act = NES_ACT_RIGHT;
    return NES_TARGET_PORT(port, act);
}

/* A label on a dark pill, filled with the accent when picked. */
static void NesCtlDrawPill(NSRect rect, NSString *text, BOOL picked)
{
    NSBezierPath *pill = NesCtlRound(rect, rect.size.height / 2);
    [(picked ? NESAccentColor() : NesCtlRgba(NESC_COL_PILL, 0.88)) setFill];
    [pill fill];
    NesCtlDrawText(text, NSInsetRect(rect, 3, 0), 9, YES, NSTextAlignmentCenter, NSColor.whiteColor);
}

/* ---- views ---------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface NesCtlFlippedView : NSView
@end

@implementation NesCtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class NESControllersWindow;
@interface NesCtlPanel : NSPanel
@property (nonatomic, weak) NESControllersWindow *owner;
@end

/* Everything the views and the class methods call on the window. */
@interface NESControllersWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(nessession *)session;
- (void)show;
- (void)hide;
- (void)toggle;
- (BOOL)isVisible;
- (BOOL)isActive;
- (void)refresh;
- (void)showPage:(int)page;
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
@end

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on. */
typedef struct {
    NSRect rect;
    int target;
    char text[64];
} NesCtlLabel;

/* The stage: both ports and the console on a grey backdrop. Everything is drawn
 * here and hit-tested by the core (the cross's drag maths are the core's too). */
@interface NesCtlStageView : NesCtlFlippedView <NSViewToolTipOwner>
@property (nonatomic) nessession *session;
@property (nonatomic, weak) NESControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Whether the layout is out of date: a port has another controller to draw. */
- (BOOL)layoutStale;
/* Reads what the console sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation NesCtlStageView {
    const nes_control *_ctls;   /* the core's static table */
    int _count;
    float _dimW, _dimH;
    int _types[2];              /* the port types the layout was read for */
    NSSize _size;
    BOOL _lit[NESC_MAX_CTL];    /* what the console sees held, per control */

    /* the mouse */
    int _downIdx;               /* the control pressed, or -1 */
    int _downTarget;            /* the target the pointer holds with a press, or -1 */
    BOOL _crossDrag;
    int _hover;                 /* the pressable part under the pointer (Map), or -1 */
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downTarget = -1;
    _mapTarget = -1;
    _hover = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

/* (Re)reads the layout for the controllers the ports show. */
- (void)configure
{
    for (int port = 0; port < 2; port++) _types[port] = nessession_port_type(_session, port);
    _ctls = nessession_controls_layout(_types[0], _types[1], &_count, &_dimW, &_dimH);
    if (!_ctls || _count < 0) _count = 0;
    if (_count > NESC_MAX_CTL) _count = NESC_MAX_CTL;
    _size = NSMakeSize(ceil(_dimW * NESC_UNIT) + 2 * NESC_STAGE_PAD, ceil(_dimH * NESC_UNIT) + 2 * NESC_STAGE_PAD);
    _hover = -1;
    memset(_lit, 0, sizeof _lit);
    [self setNeedsDisplay:YES];
}

- (BOOL)layoutStale
{
    for (int port = 0; port < 2; port++)
        if (nessession_port_type(_session, port) != _types[port]) return YES;
    return NO;
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

/* Where the layout's origin is, in view points. */
- (NSPoint)origin
{
    return NSMakePoint(floor((_size.width - _dimW * NESC_UNIT) / 2), NESC_STAGE_PAD);
}

- (NSRect)rectOf:(const nes_control *)ctl
{
    const NSPoint org = [self origin];
    return NSMakeRect(org.x + ctl->x * NESC_UNIT, org.y + ctl->y * NESC_UNIT, ctl->w * NESC_UNIT, ctl->h * NESC_UNIT);
}

- (const nes_control *)ctlAt:(int)idx
{
    return idx >= 0 && idx < _count ? &_ctls[idx] : NULL;
}

/* The pressable part under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return nessession_controls_hit(_types[0], _types[1], (float)((pnt.x - org.x) / NESC_UNIT),
                                   (float)((pnt.y - org.y) / NESC_UNIT));
}

/* Whether a Map pick is one of the port's four directions. */
static BOOL NesCtlIsDirectionOf(int target, int port)
{
    return target >= NES_TARGET_PORT(port, NES_ACT_UP) && target <= NES_TARGET_PORT(port, NES_ACT_RIGHT);
}

- (BOOL)picked:(int)target
{
    return self.mapping && target >= 0 && target == self.mapTarget;
}

/* Whether a part is drawn faint: in Map mode every pressable part but the pick
 * (or, while nothing is picked yet, the one under the pointer). */
- (BOOL)dimmedIdx:(int)idx
{
    if (!self.mapping) return NO;
    const nes_control *ctl = &_ctls[idx];
    switch (ctl->kind) {
    case NES_CTL_DPAD_ARM:
        if (self.mapTarget >= 0 && NesCtlIsDirectionOf(self.mapTarget, ctl->port)) return NO;
        break;
    case NES_CTL_ROUND_BUTTON:
    case NES_CTL_PILL:
    case NES_CTL_BUTTON:
        if ([self picked:ctl->target]) return NO;
        break;
    default:
        return NO;
    }
    if (self.mapTarget >= 0) return YES;
    if (_hover < 0) return YES;
    const nes_control *over = &_ctls[_hover];
    if (ctl->kind == NES_CTL_DPAD_ARM) return !(over->kind == NES_CTL_DPAD && over->port == ctl->port);
    return idx != _hover;
}

- (void)beginDim:(BOOL)dim alpha:(CGFloat)alpha
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, alpha);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* ---- the labels shown over the controls ---------------------------------------- */

- (int)labels:(NesCtlLabel *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
#define NESC_ADD_LABEL(cx, cy, wid, tgt, prefix) do { \
        if (num < max) { \
            NesCtlLabel *lab = &out[num++]; \
            lab->rect = NSMakeRect((cx) - (wid) / 2, (cy), (wid), NESC_PILL_H); \
            lab->target = (tgt); \
            NSString *name = [self.owner legendForTarget:(tgt)]; \
            NSString *text = [NSString stringWithFormat:@"%@%@", prefix, name.length ? name : @"-"]; \
            strncpy(lab->text, text.UTF8String, sizeof lab->text - 1); \
            lab->text[sizeof lab->text - 1] = '\0'; \
        } } while (0)
    for (int idx = 0; idx < _count; idx++) {
        const nes_control *ctl = &_ctls[idx];
        const NSRect rect = [self rectOf:ctl];
        const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
        switch (ctl->kind) {
        case NES_CTL_DPAD: {
            const int port = ctl->port;
            const CGFloat half = NESC_PILL_H / 2;
            NESC_ADD_LABEL(ctr.x - 1.5 * NESC_UNIT, ctr.y - half, NESC_PILL_W,
                           NES_TARGET_PORT(port, NES_ACT_LEFT), @"◀ ");
            NESC_ADD_LABEL(ctr.x + 1.5 * NESC_UNIT, ctr.y - half, NESC_PILL_W,
                           NES_TARGET_PORT(port, NES_ACT_RIGHT), @"▶ ");
            NESC_ADD_LABEL(ctr.x, ctr.y - 1.4 * NESC_UNIT - half, NESC_PILL_W,
                           NES_TARGET_PORT(port, NES_ACT_UP), @"▲ ");
            NESC_ADD_LABEL(ctr.x, ctr.y + 1.4 * NESC_UNIT - half, NESC_PILL_W,
                           NES_TARGET_PORT(port, NES_ACT_DOWN), @"▼ ");
            break;
        }
        case NES_CTL_ROUND_BUTTON:
            NESC_ADD_LABEL(ctr.x, NSMaxY(rect) + 0.2 * NESC_UNIT, NESC_PILL_W, ctl->target, @"");
            break;
        case NES_CTL_PILL:
            NESC_ADD_LABEL(ctr.x, NSMaxY(rect) - 1.0, NESC_PILL_W, ctl->target, @"");
            break;
        case NES_CTL_BUTTON:
            NESC_ADD_LABEL(ctr.x, NSMaxY(rect), NESC_PILL_W, ctl->target, @"");
            break;
        default:
            break;
        }
    }
#undef NESC_ADD_LABEL
    return num;
}

/* The target Map would pick at a point (view points), or -1. */
- (int)mapTargetAt:(NSPoint)pnt
{
    NesCtlLabel labels[NESC_MAX_PILLS];
    const int num = [self labels:labels max:NESC_MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        if (NSPointInRect(pnt, labels[idx].rect)) return labels[idx].target;
    const nes_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return -1;
    switch (ctl->kind) {
    case NES_CTL_ROUND_BUTTON:
    case NES_CTL_PILL:
    case NES_CTL_BUTTON:
        return ctl->target;
    case NES_CTL_DPAD: {
        const NSRect rect = [self rectOf:ctl];
        return NesCtlDirectionTarget(ctl->port, (float)((pnt.x - NSMidX(rect)) / NESC_UNIT),
                                     (float)((pnt.y - NSMidY(rect)) / NESC_UNIT));
    }
    default:
        return -1;
    }
}

/* ---- polling ------------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    unsigned held[2];
    for (int port = 0; port < 2; port++)
        held[port] = self.mapping ? 0u : nessession_buttons_held(_session, port);
    for (int idx = 0; idx < _count; idx++) {
        const nes_control *ctl = &_ctls[idx];
        BOOL lit = NO;
        if (!self.mapping && ctl->target >= 0) {
            if (ctl->kind == NES_CTL_DPAD_ARM && ctl->port >= 0 && ctl->port < 2)
                lit = ((held[ctl->port] >> (ctl->target % NES_ACT_PER_PORT)) & 1u) != 0;
            else if (ctl->kind == NES_CTL_ROUND_BUTTON || ctl->kind == NES_CTL_PILL || ctl->kind == NES_CTL_BUTTON)
                lit = nessession_target_held(_session, ctl->target) != 0;
        }
        if (lit != _lit[idx]) { _lit[idx] = lit; dirty = YES; }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------- */

/* A held round part: the ring and a darker fill. */
- (void)markHeld:(NSBezierPath *)path
{
    [NesCtlRgba(NESSESSION_HELD_RGB, 0.35) setFill];
    [path fill];
    [NesCtlRgb(NESSESSION_HELD_RGB) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* The picked part in Map mode: the ring. */
- (void)ringPicked:(NSBezierPath *)path
{
    [NesCtlRgb(NESSESSION_HELD_RGB) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* The art of a BODY entry, by its label. */
- (void)drawBody:(const nes_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const char *label = ctl->label ? ctl->label : "";
    if (!strcmp(label, "NES-001")) {
        NSBezierPath *shell = NesCtlRound(rect, 0.6 * NESC_UNIT);
        NesCtlFillGradient(shell, NESSESSION_SHELL_RGB, NESSESSION_SHELL_SHADOW_RGB);
        [NesCtlRgb(NESSESSION_SHELL_SHADOW_RGB) setStroke];
        shell.lineWidth = 1.5;
        [shell stroke];
    } else if (!strcmp(label, "Face")) {
        [NesCtlRgb(NESSESSION_FACE_RGB) setFill];
        [NesCtlRound(rect, 0.3 * NESC_UNIT) fill];
    } else if (!strcmp(label, "Stripe")) {
        [NesCtlRgb(NESSESSION_STRIPE_RGB) setFill];
        [NSBezierPath fillRect:rect];
    } else if (!strcmp(label, "DpadWell") || !strcmp(label, "SelectWell") || !strcmp(label, "ButtonWell")) {
        NSBezierPath *well = NesCtlRound(rect, 0.3 * NESC_UNIT);
        [NesCtlRgb(NESSESSION_SHELL_RGB) setFill];
        [well fill];
        [NesCtlRgb(NESSESSION_SHELL_SHADOW_RGB) setStroke];
        well.lineWidth = 1;
        [well stroke];
    } else if (!strcmp(label, "Socket")) {
        NSBezierPath *socketPath = NesCtlRound(NSInsetRect(rect, 1, 1), 0.6 * NESC_UNIT);
        const CGFloat dash[2] = { 6, 4 };
        [socketPath setLineDash:dash count:2 phase:0];
        [NesCtlRgba(0x000000, 0.12) setFill];
        [socketPath fill];
        [NesCtlRgb(NESSESSION_SOCKET_RGB) setStroke];
        socketPath.lineWidth = 2;
        [socketPath stroke];
    } else if (!strcmp(label, "Console")) {
        NSBezierPath *slab = NesCtlRound(rect, 0.4 * NESC_UNIT);
        [NesCtlRgb(NESSESSION_CONSOLE_RGB) setFill];
        [slab fill];
    } else if (!strcmp(label, "Base")) {
        [NesCtlRgb(NESSESSION_CONSOLE_BASE_RGB) setFill];
        [NesCtlBottomRound(rect, 0.4 * NESC_UNIT) fill];
    } else if (!strcmp(label, "POWER")) {
        /* not interactive: the emulator is on */
        NSBezierPath *key = NesCtlRound(rect, 0.2 * NESC_UNIT);
        [NesCtlRgba(NESSESSION_KEY_RGB, 0.55) setFill];
        [key fill];
        NesCtlDrawText(@"POWER", NSInsetRect(rect, 3, 0), 10, YES, NSTextAlignmentCenter,
                       NesCtlRgba(NESSESSION_KEY_INK_RGB, 0.6));
    }
}

- (void)drawCaption:(const nes_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSString *text = NesCtlStr(ctl->label);
    const char *label = ctl->label ? ctl->label : "";
    const CGFloat size = MAX(7.0, rect.size.height * 0.9);
    if (!strncmp(label, "Player ", 7)) {
        NesCtlDrawText(text, rect, 13, YES, NSTextAlignmentCenter, NesCtlRgb(NESC_COL_CAPTION));
    } else if (!strcmp(label, "No controller")) {
        NesCtlDrawText(text, rect, 16, NO, NSTextAlignmentCenter, NesCtlRgb(NESSESSION_SOCKET_INK_RGB));
    } else if (!strcmp(label, "Nintendo ENTERTAINMENT SYSTEM")) {
        NesCtlDrawText(text, rect, size, YES, NSTextAlignmentLeft, NesCtlRgb(NESSESSION_LEGEND_RGB));
    } else {
        NesCtlDrawText(text, rect, size, YES, NSTextAlignmentCenter, NesCtlRgb(NESSESSION_LEGEND_RGB));
    }
}

/* One arm of the cross, with its embossed triangle; the dimple goes over the
 * last arm. */
- (void)drawArm:(const nes_control *)ctl lit:(BOOL)lit
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *arm = NesCtlRound(rect, 0.25 * NESC_UNIT);
    [NesCtlRgb(NESSESSION_DPAD_RGB) setFill];
    [arm fill];
    if (lit) {
        [NesCtlRgba(NESSESSION_DPAD_HELD_RGB, 0.55) setFill];
        [arm fill];
    }
    double dirx = 0, diry = 0;
    const char *label = ctl->label ? ctl->label : "";
    if (!strcmp(label, "Up")) diry = -1;
    else if (!strcmp(label, "Down")) diry = 1;
    else if (!strcmp(label, "Left")) dirx = -1;
    else dirx = 1;
    const NSPoint mid = NSMakePoint(NSMidX(rect) + dirx * 0.3 * NESC_UNIT, NSMidY(rect) + diry * 0.3 * NESC_UNIT);
    [(lit ? NesCtlRgba(0xFFFFFF, 0.8) : NesCtlRgb(NESSESSION_DPAD_MARK_RGB)) setFill];
    [NesCtlTriangle(mid, dirx, diry, 0.28 * NESC_UNIT) fill];
    if (self.mapping && [self picked:ctl->target]) [self ringPicked:NesCtlRound(rect, 0.25 * NESC_UNIT)];
    if (dirx > 0) {
        /* the dimple, at the cross's centre: the right arm's left edge */
        const NSPoint centre = NSMakePoint(NSMinX(rect), NSMidY(rect));
        [NesCtlRgb(NESSESSION_DPAD_DIMPLE_RGB) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(centre.x - 0.4 * NESC_UNIT, centre.y - 0.4 * NESC_UNIT,
                                                           0.8 * NESC_UNIT, 0.8 * NESC_UNIT)] fill];
    }
}

- (void)drawRound:(const nes_control *)ctl lit:(BOOL)lit
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:rect];
    NesCtlFillGradient(disc, NESSESSION_BUTTON_LIGHT_RGB, NESSESSION_BUTTON_RGB);
    if (lit) {
        [NesCtlRgb(NESSESSION_HELD_FILL_RGB) setFill];
        [disc fill];
        [self markHeld:[NSBezierPath bezierPathWithOvalInRect:rect]];
    }
    if ([self picked:ctl->target]) [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:rect]];
}

- (void)drawPill:(const nes_control *)ctl lit:(BOOL)lit
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *pill = NesCtlRound(rect, rect.size.height / 2);
    const BOOL turbo = ctl->label && !strncmp(ctl->label, "TURBO", 5);
    if (turbo) {
        [NesCtlRgb(NESSESSION_SHELL_RGB) setFill];
        [pill fill];
        [NesCtlRgb(NESSESSION_SHELL_SHADOW_RGB) setStroke];
        pill.lineWidth = 1;
        [pill stroke];
    } else {
        [NesCtlRgb(NESSESSION_RUBBER_RGB) setFill];
        [pill fill];
    }
    if (lit) [self markHeld:NesCtlRound(rect, rect.size.height / 2)];
    if ([self picked:ctl->target]) [self ringPicked:NesCtlRound(rect, rect.size.height / 2)];
}

- (void)drawResetKey:(const nes_control *)ctl lit:(BOOL)lit
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *key = NesCtlRound(rect, 0.2 * NESC_UNIT);
    [NesCtlRgb(lit ? NESSESSION_HELD_FILL_RGB : NESSESSION_KEY_RGB) setFill];
    [key fill];
    NesCtlDrawText(@"RESET", NSInsetRect(rect, 3, 0), 11, YES, NSTextAlignmentCenter,
                   lit ? NSColor.whiteColor : NesCtlRgb(NESSESSION_KEY_INK_RGB));
    if (lit) [self markHeld:NesCtlRound(rect, 0.2 * NESC_UNIT)];
    if ([self picked:ctl->target]) [self ringPicked:NesCtlRound(rect, 0.2 * NESC_UNIT)];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *stage = NesCtlRound(NSMakeRect(0, 0, _size.width, _size.height), 10);
    NesCtlFillGradient(stage, NESC_COL_STAGE_TOP, NESC_COL_STAGE_BOT);
    [NesCtlRgb(NESC_COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    for (int idx = 0; idx < _count; idx++) {
        const nes_control *ctl = &_ctls[idx];
        const BOOL dim = [self dimmedIdx:idx];
        const BOOL lit = _lit[idx];
        [self beginDim:dim alpha:NESC_DIM_ALPHA];
        switch (ctl->kind) {
        case NES_CTL_CAPTION:      [self drawCaption:ctl]; break;
        case NES_CTL_BODY:         [self drawBody:ctl]; break;
        case NES_CTL_DPAD_ARM:     [self drawArm:ctl lit:lit]; break;
        case NES_CTL_ROUND_BUTTON: [self drawRound:ctl lit:lit]; break;
        case NES_CTL_PILL:         [self drawPill:ctl lit:lit]; break;
        case NES_CTL_BUTTON:       [self drawResetKey:ctl lit:lit]; break;
        default: break;   /* the cross's grab area is not drawn */
        }
        [self endDim:dim];
    }

    /* the Map / Show bindings labels, over everything */
    NesCtlLabel labels[NESC_MAX_PILLS];
    const int num = [self labels:labels max:NESC_MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        NesCtlDrawPill(labels[idx].rect, NesCtlStr(labels[idx].text), [self picked:labels[idx].target]);
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)letGo
{
    if (_downTarget >= 0) nessession_press(_session, _downTarget, 0);
    if (_downIdx >= 0 && _downIdx < _count && _crossDrag) nessession_pointer_set(_session, _ctls[_downIdx].port, 0);
    _downIdx = -1;
    _downTarget = -1;
    _crossDrag = NO;
    _hover = -1;
    [self setNeedsDisplay:YES];
}

/* The cross follows the pointer: the core turns the offset from its centre
 * into one of eight 45-degree sectors (nothing in the dead centre). */
- (void)dragCrossAt:(NSPoint)pnt
{
    const nes_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    const unsigned mask = nessession_dpad_from_vector((float)((pnt.x - NSMidX(rect)) / NESC_UNIT),
                                                      (float)((pnt.y - NSMidY(rect)) / NESC_UNIT));
    nessession_pointer_set(_session, ctl->port, mask);
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a label, a direction, a button or the RESET key is picked */
        const int target = [self mapTargetAt:pnt];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    const int idx = [self hit:pnt];
    const nes_control *ctl = [self ctlAt:idx];
    if (!ctl) return;
    switch (ctl->kind) {
    case NES_CTL_DPAD:
        _downIdx = idx;
        _crossDrag = YES;
        [self dragCrossAt:pnt];
        break;
    case NES_CTL_ROUND_BUTTON:
    case NES_CTL_PILL:
    case NES_CTL_BUTTON:
        if (ctl->target < 0) break;
        _downIdx = idx;
        _downTarget = ctl->target;
        nessession_press(_session, ctl->target, 1);
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)mouseDragged:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    if (_crossDrag) {
        [self dragCrossAt:pnt];
        [self poll];
        return;
    }
    /* a button or the RESET key: dragging off lets go */
    if ([self hit:pnt] != _downIdx) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    if (_downIdx < 0) return;
    [self letGo];
    [self poll];
}

/* In Map mode the part under the pointer stays bright until one is picked. */
- (void)mouseMoved:(NSEvent *)evt
{
    if (!self.mapping || self.mapTarget >= 0) return;
    const int idx = [self hit:[self convertPoint:[evt locationInWindow] fromView:nil]];
    if (idx != _hover) {
        _hover = idx;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover >= 0) {
        _hover = -1;
        [self setNeedsDisplay:YES];
    }
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const nes_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return nil;
    int target = ctl->target;
    if (ctl->kind == NES_CTL_DPAD) {
        const NSRect rect = [self rectOf:ctl];
        target = NesCtlDirectionTarget(ctl->port, (float)((pnt.x - NSMidX(rect)) / NESC_UNIT),
                                       (float)((pnt.y - NSMidY(rect)) / NESC_UNIT));
        if (target < 0) return [NSString stringWithFormat:@"Player %d D-pad: drag to steer", ctl->port + 1];
    }
    if (target < 0) return nil;
    const nes_binding bind = nessession_binding_get(_session, target);
    NSString *key = bind.keysym ? NesCtlKeyName(bind.keysym) : @"none";
    NSString *padText = bind.button != NES_PAD_BTN_NONE ? NesCtlStr(nes_pad_button_name(bind.button)) : @"none";
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", NesCtlStr(nes_target_name(target)), key, padText];
}

/* One tooltip rect over the whole stage; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

/* A mix of two 0xRRGGBB colours, frac of the way from the first to the second. */
static uint32_t NesCtlMixColour(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (double)((from >> shift) & 0xff), cb = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(ca + (cb - ca) * frac) << shift;
    }
    return out;
}

/* ---- the keyboard page ---------------------------------------------------------- */

/* The expansion keyboard, drawn from the core's skin: click a key to hold it,
 * every held key (typed, clicked or from a tape program) is lit. */
@interface NesCtlKeyboardView : NesCtlFlippedView
@property (nonatomic) nessession *session;
- (void)poll;
- (void)letGo;
@end

@implementation NesCtlKeyboardView {
    int _type;
    const nes_skin_item *_items;
    int _count;
    float _unitsW, _unitsH;
    BOOL _held[NESC_MAX_KEYS];
    int _pressed;               /* the key index the mouse holds, or -1 */
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _type = -1;
    _pressed = -1;
    return self;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

/* Reads the skin again when the attached keyboard changed. */
- (void)reload
{
    const int type = nessession_keyboard(_session);
    if (type == _type) return;
    [self letGo];
    _type = type;
    _items = nessession_keyboard_skin_layout(type, &_count, &_unitsW, &_unitsH);
    if (!_items || _count < 0) _count = 0;
    memset(_held, 0, sizeof _held);
    [self setNeedsDisplay:YES];
}

- (void)poll
{
    [self reload];
    BOOL changed = NO;
    for (int idx = 0; idx < _count; idx++) {
        const nes_skin_item *item = &_items[idx];
        if (item->kind != NES_SKIN_KEYCAP || item->key < 0 || item->key >= NESC_MAX_KEYS) continue;
        const BOOL held = nessession_keyboard_held(_session, item->key) != 0;
        if (held != _held[item->key]) { _held[item->key] = held; changed = YES; }
    }
    if (changed) [self setNeedsDisplay:YES];
}

/* Points per key unit, and the top-left that centres the keyboard. */
- (CGFloat)unitWithOrigin:(NSPoint *)origin
{
    const NSRect bnd = [self bounds];
    if (_unitsW <= 0 || _unitsH <= 0) { *origin = NSZeroPoint; return 0; }
    CGFloat unit = MIN((bnd.size.width - 2 * NESC_STAGE_PAD) / _unitsW, (bnd.size.height - 2 * NESC_STAGE_PAD) / _unitsH);
    if (unit > 40) unit = 40;
    *origin = NSMakePoint(floor((bnd.size.width - _unitsW * unit) / 2), floor((bnd.size.height - _unitsH * unit) / 2));
    return unit;
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [self reload];
    const NSRect bnd = [self bounds];
    NSBezierPath *stage = NesCtlRound(bnd, 10);
    NesCtlFillGradient(stage, NESC_COL_STAGE_TOP, NESC_COL_STAGE_BOT);
    [NesCtlRgb(NESC_COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];
    if (_count == 0) {
        NesCtlDrawText(@"No keyboard on the expansion port - choose one below", bnd, 14, NO,
                       NSTextAlignmentCenter, NesCtlRgb(NESC_COL_CAPTION));
        return;
    }
    NSPoint org;
    const CGFloat unit = [self unitWithOrigin:&org];
    for (int idx = 0; idx < _count; idx++) {
        const nes_skin_item *item = &_items[idx];
        NSRect rect = NSMakeRect(org.x + item->x * unit, org.y + item->y * unit, item->w * unit, item->h * unit);
        if (item->kind == NES_SKIN_KEYCAP) {
            [self drawCap:item rect:NSInsetRect(rect, 0.04 * unit, 0.04 * unit) unit:unit];
        } else {
            const char *label = item->label ? item->label : "";
            if (!strcmp(label, "Vent")) {
                /* a group of thin vertical slits in the rect */
                const int slits = 14;
                [NesCtlRgb(item->fill_rgb) setFill];
                for (int slit = 0; slit < slits; slit++) {
                    const CGFloat pitch = rect.size.width / slits;
                    [NSBezierPath fillRect:NSMakeRect(rect.origin.x + slit * pitch + pitch * 0.25, rect.origin.y,
                                                      MAX(1.0, pitch * 0.5), rect.size.height)];
                }
                continue;
            }
            if (!strcmp(label, "Well")) {
                /* a recess the keys sit in: filled, a 1 px outline */
                NSBezierPath *well = NesCtlRound(rect, 0.1 * unit);
                [NesCtlRgb(item->fill_rgb) setFill];
                [well fill];
                [NesCtlRgb(item->ink_rgb) setStroke];
                well.lineWidth = 1;
                [well stroke];
                continue;
            }
            const BOOL isCase = !strcmp(label, "Case");
            NSBezierPath *body = NesCtlRound(rect, isCase ? 0.5 * unit : 0.12 * unit);
            [NesCtlRgb(item->fill_rgb) setFill];
            [body fill];
            if (isCase) {
                [NesCtlRgba(0x000000, 0.4) setStroke];
                body.lineWidth = 1.5;
                [body stroke];
            }
            if (item->text)
                NesCtlDrawText(NesCtlStr(item->text), NSInsetRect(rect, 3, 0), MAX(7.0, rect.size.height * 0.6), YES,
                               NSTextAlignmentCenter, NesCtlRgb(item->ink_rgb));
        }
    }
}

/* One keycap: a bevelled cap with a lighter top face; the main legend large
 * (low and to the left when the key has other legends), the shifted symbol
 * small at the top right and the katakana small at the bottom right. */
- (void)drawCap:(const nes_skin_item *)item rect:(NSRect)rect unit:(CGFloat)unit
{
    const BOOL lit = item->key >= 0 && item->key < NESC_MAX_KEYS && (_held[item->key] || item->key == _pressed);
    const uint32_t fill = lit ? NESC_COL_KEY_HELD : item->fill_rgb;
    NSBezierPath *cap = NesCtlRound(rect, 0.15 * unit);
    [NesCtlRgb(NesCtlMixColour(fill, 0x000000, 0.25)) setFill];
    [cap fill];
    const NSRect face = NSMakeRect(rect.origin.x + 0.04 * unit, rect.origin.y + 0.03 * unit,
                                   rect.size.width - 0.08 * unit, rect.size.height - 0.12 * unit);
    NSBezierPath *top = NesCtlRound(face, 0.12 * unit);
    NesCtlFillGradient(top, NesCtlMixColour(fill, 0xFFFFFF, 0.18), fill);
    [NesCtlRgba(0x000000, 0.35) setStroke];
    cap.lineWidth = 1;
    [cap stroke];
    if (!item->label) return;
    NSColor *ink = lit ? NesCtlRgb(0x1E1E1E) : NesCtlRgb(item->ink_rgb);
    NSString *shifted = @"";
    NSString *kana = @"";
    if (item->text) {
        NSArray<NSString *> *parts = [NesCtlStr(item->text) componentsSeparatedByString:@"|"];
        if (parts.count > 0) shifted = parts[0];
        if (parts.count > 1) kana = parts[1];
    }
    const BOOL extra = shifted.length > 0 || kana.length > 0;
    const CGFloat small = MAX(5.0, unit * 0.24);
    NSString *main = NesCtlStr(item->label);
    if ([main isEqualToString:@"CLR HOME"]) {
        const CGFloat half = face.size.height / 2;
        NesCtlDrawText(@"CLR", NSMakeRect(face.origin.x, face.origin.y, face.size.width, half),
                       MAX(6.0, unit * 0.3), NO, NSTextAlignmentCenter, ink);
        NesCtlDrawText(@"HOME", NSMakeRect(face.origin.x, face.origin.y + half, face.size.width, half),
                       MAX(6.0, unit * 0.3), NO, NSTextAlignmentCenter, ink);
        return;
    }
    if (extra) {
        NesCtlDrawText(main, NSMakeRect(face.origin.x + 1, face.origin.y + face.size.height * 0.3,
                                        face.size.width * 0.65, face.size.height * 0.7),
                       MAX(7.0, unit * 0.42), NO, NSTextAlignmentCenter, ink);
        if (shifted.length)
            NesCtlDrawText(shifted, NSMakeRect(face.origin.x + face.size.width * 0.55, face.origin.y,
                                               face.size.width * 0.45, face.size.height * 0.45),
                           small, NO, NSTextAlignmentRight, ink);
        if (kana.length)
            NesCtlDrawText(kana, NSMakeRect(face.origin.x + face.size.width * 0.55,
                                            face.origin.y + face.size.height * 0.55,
                                            face.size.width * 0.45, face.size.height * 0.45),
                           small, NO, NSTextAlignmentRight, ink);
    } else {
        NesCtlDrawText(main, NSInsetRect(face, 1, 0), MAX(7.0, unit * 0.4), NO, NSTextAlignmentCenter, ink);
    }
}

- (void)mouseDown:(NSEvent *)evt
{
    [self reload];
    if (_count == 0 || _pressed >= 0) return;
    NSPoint org;
    const CGFloat unit = [self unitWithOrigin:&org];
    if (unit <= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    const int idx = nessession_keyboard_skin_hit(_type, (float)((pnt.x - org.x) / unit), (float)((pnt.y - org.y) / unit));
    if (idx < 0 || idx >= _count || _items[idx].key < 0) return;
    _pressed = _items[idx].key;
    nessession_keyboard_press(_session, _pressed, 1);
    [self setNeedsDisplay:YES];
}

/* mouseUp comes to the view that got mouseDown wherever the pointer went, so a
 * drag off the key still lets it go. */
- (void)mouseUp:(NSEvent *)evt
{
    (void)evt;
    [self letGo];
}

- (void)letGo
{
    if (_pressed < 0) return;
    nessession_keyboard_press(_session, _pressed, 0);
    _pressed = -1;
    [self setNeedsDisplay:YES];
}
@end

@implementation NesCtlPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* Escape is a key of the console's (or cancels a Map pick), not "close the
 * panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

static NESControllersWindow *NesCtlSingleton;

@implementation NESControllersWindow {
    nessession *_session;
    NesCtlPanel *_panel;
    NesCtlFlippedView *_content;
    NSSegmentedControl *_pageControl;
    NesCtlStageView *_stage;
    NesCtlKeyboardView *_keyboard;
    int _page;
    NSTextField *_typeLabel[2];
    NSPopUpButton *_typePopup[2];
    NSTextField *_kbdLabel;
    NSPopUpButton *_kbdPopup;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[NESC_MAX_PAD_ROWS];
    NSTextField *_padEff[NESC_MAX_PAD_ROWS];     /* each pad's "drives player N" */
    NSPopUpButton *_padAssign[NESC_MAX_PAD_ROWS];
    int _padRows;                                /* the rows showing a pad */
    int _reservedRows;                           /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSPopUpButton *_mapList;                     /* every target, the session's actions too */
    NSButton *_helpButton;
    NSButton *_configButton;
    NSButton *_stopButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    CGFloat _winW;
    CGFloat _pageH;
    uint32_t _swallowKey;                        /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

+ (NESControllersWindow *)instanceForSession:(nessession *)session
{
    if (!NesCtlSingleton) NesCtlSingleton = [[NESControllersWindow alloc] initWithSession:session];
    return NesCtlSingleton;
}

+ (void)toggleWithSession:(nessession *)session
{
    [[self instanceForSession:session] toggle];
}

+ (void)showWithSession:(nessession *)session
{
    [[self instanceForSession:session] show];
}

+ (void)showKeyboardWithSession:(nessession *)session
{
    NESControllersWindow *win = [self instanceForSession:session];
    [win show];
    [win showPage:NESC_PAGE_KEYBOARD];
}

+ (BOOL)isVisible
{
    return NesCtlSingleton && [NesCtlSingleton isVisible];
}

+ (BOOL)isActive
{
    return NesCtlSingleton && [NesCtlSingleton isActive];
}

+ (void)refresh
{
    if (NesCtlSingleton) [NesCtlSingleton refresh];
}

- (instancetype)initWithSession:(nessession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mapState = -2;
    _padGeneration = (unsigned)-1;
    _reservedRows = NESC_MIN_PAD_ROWS;

    _panel = [[NesCtlPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 746, 640)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers (F9)"];
    NESSetHelpContext(_panel, @"controllers");
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    _content = [[NesCtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 746, 640)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _pageControl = [NSSegmentedControl segmentedControlWithLabels:@[@"Controllers", @"Keyboard"]
                                                     trackingMode:NSSegmentSwitchTrackingSelectOne
                                                           target:self
                                                           action:@selector(pageChanged:)];
    [_pageControl setSelectedSegment:0];
    [_pageControl setRefusesFirstResponder:YES];
    [_content addSubview:_pageControl];

    _stage = [[NesCtlStageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.mapTarget = -1;
    _stage.showKeys = nessession_controls_show_bindings(_session) != 0;
    [_content addSubview:_stage];

    _keyboard = [[NesCtlKeyboardView alloc] initWithFrame:NSZeroRect];
    _keyboard.session = _session;
    _keyboard.hidden = YES;
    [_content addSubview:_keyboard];

    /* What is plugged into each port. */
    for (int port = 0; port < 2; port++) {
        _typeLabel[port] = [NSTextField labelWithString:(port ? @"Player 2" : @"Player 1")];
        [_typeLabel[port] setFont:[NSFont boldSystemFontOfSize:12]];
        [_content addSubview:_typeLabel[port]];
        _typePopup[port] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        for (int type = 0; nes_ctrl_type_name(type); type++)
            [_typePopup[port] addItemWithTitle:NesCtlStr(nes_ctrl_type_name(type))];
        _typePopup[port].tag = port;
        _typePopup[port].target = self;
        _typePopup[port].action = @selector(typeChanged:);
        [_typePopup[port] setRefusesFirstResponder:YES];
        [_content addSubview:_typePopup[port]];
    }

    /* The expansion keyboard (the Keyboard page's row). */
    _kbdLabel = [NSTextField labelWithString:@"Expansion port"];
    [_kbdLabel setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_kbdLabel];
    _kbdPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int type = 0; nes_keyboard_name(type); type++)
        [_kbdPopup addItemWithTitle:NesCtlStr(nes_keyboard_name(type))];
    _kbdPopup.target = self;
    _kbdPopup.action = @selector(keyboardChanged:);
    [_kbdPopup setRefusesFirstResponder:YES];
    [_content addSubview:_kbdPopup];

    /* Gamepads: one row per pad, its name, the player it drives and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < NESC_MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* Automatic (connection order), then one item per player */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    /* The Map list: every target by name, the session's own actions too (they
     * have no part on the picture). Items go in by the menu, as equal titles
     * would collapse. */
    _mapList = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    {
        NSMenuItem *none = [[_mapList menu] addItemWithTitle:@"Choose a control..." action:nil keyEquivalent:@""];
        none.tag = -1;
        for (int target = 0; target < NES_TARGET_COUNT; target++) {
            NSMenuItem *item = [[_mapList menu] addItemWithTitle:NesCtlStr(nes_target_name(target)) action:nil
                                                   keyEquivalent:@""];
            item.tag = target;
        }
    }
    _mapList.target = self;
    _mapList.action = @selector(mapListChanged:);
    [_mapList setRefusesFirstResponder:YES];
    [_content addSubview:_mapList];
    _helpButton = NESHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];

    _configButton = [NSButton buttonWithTitle:@"Reset to CONFIG" target:self action:@selector(resetToConfig:)];
    [_configButton setRefusesFirstResponder:YES];
    [_content addSubview:_configButton];
    _stopButton = [NSButton buttonWithTitle:@"Stop in debugger" target:self action:@selector(stopInDebugger:)];
    [_stopButton setRefusesFirstResponder:YES];
    [_content addSubview:_stopButton];

    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_stage configure];
    _winW = _stage.contentSize.width;
    _pageH = _stage.contentSize.height;
    [self refreshTypes];
    [self setMapState:-2];
    [self relayout];
    [self showPage:NESC_PAGE_CONTROLS];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout ------------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize ssize = _stage.contentSize;
    _winW = ssize.width;
    _pageH = ssize.height;
    const CGFloat inner = _winW - 2 * NESC_MARGIN;
    const CGFloat pageY = NESC_TOP_H;
    const CGFloat typeY = pageY + _pageH + 10;
    const CGFloat padsY = typeY + NESC_ROW_H + 8;
    const CGFloat rowsY = padsY + NESC_TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * NESC_ROW_H + 6;
    const CGFloat sysY = mapY + NESC_BTN_H + 8;
    const CGFloat hintY = sysY + NESC_BTN_H + 6;
    const CGFloat total = hintY + 34 + NESC_MARGIN;

    [self fitPanel:NSMakeSize(_winW, total)];
    [_content setFrameSize:NSMakeSize(_winW, total)];
    [_pageControl setFrame:NSMakeRect(NESC_MARGIN, 8, 230, 26)];
    [_stage setFrame:NSMakeRect(0, pageY, ssize.width, ssize.height)];
    [_keyboard setFrame:NSMakeRect(0, pageY, ssize.width, ssize.height)];
    /* a type popup under each port's half of the picture */
    for (int port = 0; port < 2; port++) {
        const CGFloat colX = NESC_MARGIN + port * (inner / 2);
        [_typeLabel[port] setFrame:NSMakeRect(colX, typeY + 4, 62, 18)];
        [_typePopup[port] setFrame:NSMakeRect(colX + 64, typeY - 1, NESC_TYPE_W, 25)];
    }
    [_kbdLabel setFrame:NSMakeRect(NESC_MARGIN, typeY + 4, 100, 18)];
    [_kbdPopup setFrame:NSMakeRect(NESC_MARGIN + 104, typeY - 1, 220, 25)];
    [_padsTitle setFrame:NSMakeRect(NESC_MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(NESC_MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < NESC_MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * NESC_ROW_H;
        [_padName[row] setFrame:NSMakeRect(NESC_MARGIN, rowY + 4, inner - NESC_POPUP_W - NESC_EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(NESC_MARGIN + inner - NESC_POPUP_W - NESC_EFF_W - 6, rowY + 4,
                                          NESC_EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(NESC_MARGIN + inner - NESC_POPUP_W, rowY - 1, NESC_POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(NESC_MARGIN, mapY, 56, NESC_BTN_H)];
    [_clearButton setFrame:NSMakeRect(NESC_MARGIN + 62, mapY, 56, NESC_BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(NESC_MARGIN + 124, mapY, 72, NESC_BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(NESC_MARGIN + 206, mapY + 3, 122, 22)];
    [_mapList setFrame:NSMakeRect(NESC_MARGIN + 336, mapY + 1, 200, 25)];
    [_helpButton setFrameOrigin:NSMakePoint(_winW - NESC_MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_configButton setFrame:NSMakeRect(NESC_MARGIN, sysY, 130, NESC_BTN_H)];
    [_stopButton setFrame:NSMakeRect(NESC_MARGIN + 136, sysY, 130, NESC_BTN_H)];
    [_hint setFrame:NSMakeRect(NESC_MARGIN, hintY, inner, 34)];
    [_stage setNeedsDisplay:YES];
    [_keyboard setNeedsDisplay:YES];
}

/* The Controllers or the Keyboard page. */
- (void)showPage:(int)page
{
    if (page != _page) {
        [self setMapState:-2];
        [self letGo];
    }
    _page = page;
    const BOOL pads = page == NESC_PAGE_CONTROLS;
    [_pageControl setSelectedSegment:page];
    _stage.hidden = !pads;
    _keyboard.hidden = pads;
    for (int port = 0; port < 2; port++) {
        _typeLabel[port].hidden = !pads;
        _typePopup[port].hidden = !pads;
    }
    _kbdLabel.hidden = pads;
    _kbdPopup.hidden = pads;
    _mapButton.enabled = pads;
    _showKeysBox.enabled = pads;
    [self refreshKeyboardPopup];
    [_keyboard poll];
}

- (void)pageChanged:(NSSegmentedControl *)sender
{
    [self showPage:(int)sender.selectedSegment];
}

/* Lets go of what the mouse holds in this panel (the pointer source only). */
- (void)letGo
{
    [_stage letGo];
    [_keyboard letGo];
    nessession_release_pointer(_session);
}

/* The ports' controllers changed (Settings did): the picture is read again;
 * the panel does not change size. */
- (void)syncLayout
{
    if (![_stage layoutStale]) return;
    [self letGo];
    [_stage configure];
    [self refreshTypes];
    [_stage poll];
}

/* ---- the controller each port has --------------------------------------------------- */

- (void)refreshTypes
{
    for (int port = 0; port < 2; port++) {
        const int type = nessession_port_type(_session, port);
        if (type >= 0 && type < (int)_typePopup[port].numberOfItems && _typePopup[port].indexOfSelectedItem != type)
            [_typePopup[port] selectItemAtIndex:type];
    }
}

- (void)typeChanged:(NSPopUpButton *)sender
{
    nessession_set_port_type(_session, (int)sender.tag, (int)sender.indexOfSelectedItem);
    [self refreshTypes];
    [self syncLayout];
}

/* The popup follows the session (Settings changes it too). */
- (void)refreshKeyboardPopup
{
    const int type = nessession_keyboard(_session);
    if (type >= 0 && type < (int)_kbdPopup.numberOfItems && _kbdPopup.indexOfSelectedItem != type)
        [_kbdPopup selectItemAtIndex:type];
}

- (void)keyboardChanged:(NSPopUpButton *)sender
{
    nessession_set_keyboard(_session, (int)sender.indexOfSelectedItem);
    [_keyboard poll];
}

/* ---- session actions ---------------------------------------------------------------- */

/* Posted, so the application runs them exactly as it runs a gamepad's. */
- (void)resetToConfig:(id)sender
{
    (void)sender;
    nessession_sysaction_post(_session, NES_SYSACT_RESET_CONFIG);
}

- (void)stopInDebugger:(id)sender
{
    (void)sender;
    nessession_sysaction_post(_session, NES_SYSACT_PAUSE);
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return NesCtlKeyName(nessession_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the hint line. */
- (NSString *)bindingText:(int)target
{
    const nes_binding bind = nessession_binding_get(_session, target);
    NSString *key = bind.keysym ? NesCtlKeyName(bind.keysym) : @"no key";
    if (bind.button != NES_PAD_BTN_NONE)
        return [NSString stringWithFormat:@"%@, pad %@", key, NesCtlStr(nes_pad_button_name(bind.button))];
    return key;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != -2;
    _stage.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_stage setNeedsDisplay:YES];
    [_stage poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        nessession_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc backs out.",
                             NesCtlStr(nes_target_name(state)), [self bindingText:state]];
    } else {
        nessession_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a direction, a button or RESET to remap it, or choose one from the list."
            : @"Hold a button or a key, drag the D-pad, or click RESET. Keys and gamepads work here too.";
    }
    if (state != -2) {
        [self letGo];
        nessession_release_all(_session);   /* Map's key-ups must not strand a held input */
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    NesCtlSetButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    _mapList.enabled = state != -2;
    if (![_mapList selectItemWithTag:(state >= 0 ? state : -1)]) [_mapList selectItemAtIndex:0];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2 && target >= 0 && target < NES_TARGET_COUNT) [self setMapState:target];
}

- (void)mapListChanged:(NSPopUpButton *)sender
{
    const NSInteger tag = sender.selectedItem.tag;
    if (tag >= 0) [self mapPick:(int)tag];
    else if (_mapState >= 0) [self setMapState:-1];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = NesCtlStr(nes_target_name(target));
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, NesCtlStr(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    nessession_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:NesCtlKeyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = NES_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !nessession_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    nessession_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:NesCtlStr(nes_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    nessession_binding_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", NesCtlStr(nes_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    nessession_bindings_reset(_session);
    for (int pad = 0, count = nessession_gamepad_count(_session); pad < count; pad++)
        nessession_gamepad_assign(_session, pad, -1);
    [self refreshPads];
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    nessession_set_controls_show_bindings(_session, show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int row = 0; row < _padRows; row++) {
        const int port = nessession_gamepad_effective_port(_session, row);
        NSString *text = port >= 0 ? [NSString stringWithFormat:@"drives player %d", port + 1] : @"drives nothing";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

/* Rebuilt whenever the pads' generation changes (one added or removed). */
- (void)refreshPads
{
    const int count = nessession_gamepad_count(_session);
    _padGeneration = nessession_gamepad_generation(_session);
    _padRows = count < NESC_MAX_PAD_ROWS ? (count > 0 ? count : 0) : NESC_MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more pads
     * are connected); the panel changes size only then */
    const int rooms = _padRows > NESC_MIN_PAD_ROWS ? _padRows : NESC_MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > NESC_MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", NESC_MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < NESC_MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        nessession_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = NesCtlStr(name);
        _padName[row].toolTip = NesCtlStr(name);
        /* the pad's own player, or Automatic when it has none */
        const int assign = nessession_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= 2 ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    nessession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads and what Settings changed. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= NESC_SLOW_TICKS) {
        _slow = 0;
        [self syncLayout];
        [self refreshTypes];
        [self refreshKeyboardPopup];
        if (nessession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
    }
    if (_page == NESC_PAGE_CONTROLS) [_stage poll];
    else [_keyboard poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak NESControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = NESKeysymFromEvent(evt);
    if (!keysym) return;
    if (_swallowKey && keysym == _swallowKey) return;
    if ([evt isARepeat]) return;
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick; a Ctrl chord
         * and the app's keys are not bindings */
        if ([evt modifierFlags] & NSEventModifierFlagControl) return;
        if (nessession_key_reserved(keysym)) return;
        if (keysym == NES_KEYSYM_ESCAPE) {
            [self setMapState:-1];
            _swallowKey = keysym;
        } else {
            [self bindKey:keysym swallow:YES];
        }
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* While the emulated keyboard captures, every key is its key (F9 and
     * Escape too); the Cmd shortcuts stay the menus'. */
    if (nessession_keyboard_captures(_session)) {
        nessession_key(_session, keysym, 1);
        return;
    }
    /* the keys the app owns: F9 toggles this panel, F11 is full screen, F12 the debugger */
    if (keysym == NES_KEYSYM_F9) { [self toggle]; return; }
    if (keysym == NES_KEYSYM_F11) { [[NSApp mainWindow] toggleFullScreen:nil]; return; }
    if (keysym == NES_KEYSYM_F12) { [NESDebuggerWindow toggleForSession:_session]; return; }
    /* a key bound to a system action goes to the app's queue, as a gamepad's does */
    const int sysact = nessession_key_sysaction(_session, keysym);
    if (sysact >= 0) {
        nessession_sysaction_post(_session, sysact);
        return;
    }
    nessession_key(_session, keysym, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    const uint32_t keysym = NESKeysymFromEvent(evt);
    if (_swallowKey && keysym == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    if (_mapState != -2 || !keysym) return;
    nessession_key(_session, keysym, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise they are
 * the console's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    int down = 0;
    const uint32_t keysym = NESKeysymFromFlagsChange(evt, &down);
    if (!keysym) return;
    if (_mapState >= 0) {
        /* a modifier pressed while a pick waits is the binding */
        if (down) [self bindKey:keysym swallow:NO];
        return;
    }
    if (_mapState == -1) return;
    nessession_key(_session, keysym, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    nessession_release_all(_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [_stage configure];
    [self refreshTypes];
    [self refreshPads];
    [self setMapState:-2];
    [self relayout];
    [self showPage:_page];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (BOOL)isActive
{
    return [_panel isKeyWindow];
}

/* The bindings or the gamepads changed elsewhere (Settings, a hot-plug): read
 * again. */
- (void)refresh
{
    [self syncLayout];
    [self refreshTypes];
    [self refreshKeyboardPopup];
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    nessession_release_pointer(_session);
    nessession_release_keyboard(_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
