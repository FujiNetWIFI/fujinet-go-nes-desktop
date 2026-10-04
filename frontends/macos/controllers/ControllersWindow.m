/*
 * NESControllersWindow -- the AppKit controllers panel: a NES controller
 * per player (the cross, Select, Start, B, A and the two turbo buttons),
 * the console's RESET and the session's actions, the gamepads that are
 * connected and which player each drives, and the Map row.
 *
 * Buttons are a PadButton subclass that reports mouseDown and mouseUp
 * rather than an action on click: a controller button is HELD, and a game
 * reads the controller once a frame, so a value present only for the
 * instant of a click falls between frames. mouseUp arrives even when the
 * pointer has left the button (AppKit tracks the drag for the view that got
 * mouseDown), so dragging off cannot strand the machine with a button held.
 *
 * Whatever holds a button -- the keyboard, a gamepad or a click here -- the
 * button lights up in the accent colour, from nessession_buttons_held.
 *
 * A utility panel of fixed size (no resize mask). Singleton, ordered out
 * rather than closed, so its position survives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"

/* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
 * pad button. */
static int g_mapState = -2;
static NESControllersWindow *g_singleton;
static nessession *g_session;

#define MAX_PAD_ROWS 4

/* NOT named `target`: NSControl already has a `target` property. */
@interface PadButton : NSButton
@property (nonatomic) int padTarget;
@property (nonatomic, copy) NSString *face;
@property (nonatomic) BOOL down;
@property (nonatomic) BOOL lit;
@end

@implementation PadButton
- (void)setLitState:(BOOL)lit
{
    if (lit == self.lit) return;
    self.lit = lit;
    self.bezelColor = lit ? NESAccentColor() : nil;
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (g_mapState == -1) {
        g_mapState = self.padTarget;
        [[NSNotificationCenter defaultCenter] postNotificationName:@"NESPadMapTarget" object:nil];
        return;
    }
    if (g_mapState >= 0) return;
    self.down = YES;
    [self setLitState:YES];
    if (self.padTarget >= NES_TARGET_SWITCH(0)) return;   /* fires on release */
    nessession_press(g_session, self.padTarget, 1);
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.down) return;
    self.down = NO;
    [self setLitState:NO];
    if (g_mapState != -2) return;
    if (self.padTarget >= NES_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not power-cycle the machine. Posted, so the
         * application runs them exactly as it runs a gamepad's. */
        nessession_sysaction_post(g_session, self.padTarget - NES_TARGET_SYSACT(0));
        return;
    }
    if (self.padTarget >= NES_TARGET_SWITCH(0)) {
        nessession_press(g_session, self.padTarget, 1);    /* RESET acts on press */
        nessession_press(g_session, self.padTarget, 0);
        return;
    }
    nessession_press(g_session, self.padTarget, 0);
}
@end

@implementation NESControllersWindow {
    NSMutableArray<PadButton *> *_buttons;
    NSButton *_mapButton;
    NSTextField *_hint;
    NSPopUpButton *_typePopup[2];
    NSTextField *_padName[MAX_PAD_ROWS];
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    NSTextField *_noPads;
    unsigned _padGeneration;
    NSTimer *_captureTimer;
    NSTimer *_liveTimer;
}

#define KEY_W   60.0
#define KEY_H   34.0
#define GAP      6.0
#define MID_W   72.0
#define GROUP  24.0
#define DPAD_W (3 * KEY_W + 2 * GAP)
#define MID_GW (2 * MID_W + GAP)
#define BA_W   (2 * KEY_W + GAP)
#define PAD_W  (DPAD_W + GROUP + MID_GW + GROUP + BA_W)
#define PAD_H  (3 * KEY_H + 2 * GAP)
#define MARGIN  12.0
#define ROW_H   26.0

- (PadButton *)buttonWithFace:(NSString *)face target:(int)target frame:(NSRect)frame
{
    PadButton *b = [[PadButton alloc] initWithFrame:frame];
    [b setTitle:face];
    [b setBezelStyle:NSBezelStyleRounded];
    b.padTarget = target;
    b.face = face;
    /* Not focusable: clicking a pad button must not steal the key window's
     * first responder. */
    [b setRefusesFirstResponder:YES];
    [_buttons addObject:b];
    return b;
}

/* The pad's cell (col, row) inside a block whose top-left is (x, top);
 * AppKit's y grows upwards, so rows are placed by their bottom edge. */
static NSRect cell(CGFloat x, CGFloat top, int col, int row, CGFloat w)
{
    return NSMakeRect(x + col * (w + GAP), top - (row + 1) * KEY_H - row * GAP, w, KEY_H);
}

/* One controller, laid out downwards from topY. Returns the y below it. */
- (CGFloat)buildController:(int)port intoView:(NSView *)parent topY:(CGFloat)topY
{
    CGFloat y = topY;
    const CGFloat x0 = MARGIN;

    NSTextField *title = [NSTextField labelWithString:(port ? @"Player 2" : @"Player 1")];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(x0, y - 20, 80, 16)];
    [parent addSubview:title];

    _typePopup[port] = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(x0 + 90, y - 24, 180, 24)];
    for (int i = 0; nes_ctrl_type_name(i); i++)
        [_typePopup[port] addItemWithTitle:[NSString stringWithUTF8String:nes_ctrl_type_name(i)]];
    _typePopup[port].tag = port;
    _typePopup[port].target = self;
    _typePopup[port].action = @selector(typeChanged:);
    [_typePopup[port] setRefusesFirstResponder:YES];
    [parent addSubview:_typePopup[port]];
    y -= ROW_H + GAP;

    /* the cross */
    [parent addSubview:[self buttonWithFace:@"▲" target:NES_TARGET_PORT(port, NES_ACT_UP)
                                      frame:cell(x0, y, 1, 0, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"◀" target:NES_TARGET_PORT(port, NES_ACT_LEFT)
                                      frame:cell(x0, y, 0, 1, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"▶" target:NES_TARGET_PORT(port, NES_ACT_RIGHT)
                                      frame:cell(x0, y, 2, 1, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"▼" target:NES_TARGET_PORT(port, NES_ACT_DOWN)
                                      frame:cell(x0, y, 1, 2, KEY_W)]];

    /* Select and Start in the middle row */
    const CGFloat mx = x0 + DPAD_W + GROUP;
    [parent addSubview:[self buttonWithFace:@"Select" target:NES_TARGET_PORT(port, NES_ACT_SELECT)
                                      frame:cell(mx, y, 0, 1, MID_W)]];
    [parent addSubview:[self buttonWithFace:@"Start" target:NES_TARGET_PORT(port, NES_ACT_START)
                                      frame:cell(mx, y, 1, 1, MID_W)]];

    /* B and A in the controller's order, the turbos above them */
    const CGFloat bx = mx + MID_GW + GROUP;
    [parent addSubview:[self buttonWithFace:@"Turbo B" target:NES_TARGET_PORT(port, NES_ACT_TURBO_B)
                                      frame:cell(bx, y, 0, 0, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"Turbo A" target:NES_TARGET_PORT(port, NES_ACT_TURBO_A)
                                      frame:cell(bx, y, 1, 0, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"B" target:NES_TARGET_PORT(port, NES_ACT_B)
                                      frame:cell(bx, y, 0, 1, KEY_W)]];
    [parent addSubview:[self buttonWithFace:@"A" target:NES_TARGET_PORT(port, NES_ACT_A)
                                      frame:cell(bx, y, 1, 1, KEY_W)]];
    return y - PAD_H;
}

- (instancetype)init
{
    const CGFloat width = MARGIN + PAD_W + MARGIN;
    const CGFloat height = MARGIN
        + 2 * (ROW_H + GAP + PAD_H + MARGIN)      /* the two controllers */
        + KEY_H + MARGIN                          /* console row */
        + 18 + MAX_PAD_ROWS * ROW_H + MARGIN      /* gamepads */
        + 30 + MARGIN;                            /* map row */

    NSPanel *win = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Controllers"];
    /* Floats above the machine's window and stays out of the way. */
    [win setLevel:NSFloatingWindowLevel];
    [win setReleasedWhenClosed:NO];
    [win setBecomesKeyOnlyIfNeeded:NO];

    self = [super initWithWindow:win];
    if (!self) return nil;
    [win setDelegate:self];

    _buttons = [NSMutableArray array];
    NSView *content = [win contentView];
    CGFloat y = height - MARGIN;
    y = [self buildController:0 intoView:content topY:y] - MARGIN;
    y = [self buildController:1 intoView:content topY:y] - MARGIN;

    /* The console's RESET and the session's actions. */
    static const struct { NSString *__unsafe_unretained face; int target; } console[3] = {
        { @"Reset Game",      NES_TARGET_SWITCH(NES_SW_RESET) },
        { @"Reset to CONFIG", NES_TARGET_SYSACT(NES_SYSACT_RESET_CONFIG) },
        { @"Pause",           NES_TARGET_SYSACT(NES_SYSACT_PAUSE) },
    };
    const CGFloat cw = (PAD_W - 2 * GAP) / 3;
    for (int i = 0; i < 3; i++)
        [content addSubview:[self buttonWithFace:console[i].face target:console[i].target
                                           frame:NSMakeRect(MARGIN + i * (cw + GAP), y - KEY_H, cw, KEY_H)]];
    y -= KEY_H + MARGIN;

    /* The gamepads: which player each one drives. */
    NSTextField *pads = [NSTextField labelWithString:@"Gamepads"];
    [pads setFont:[NSFont boldSystemFontOfSize:12]];
    [pads setFrame:NSMakeRect(MARGIN, y - 16, 120, 16)];
    [content addSubview:pads];
    y -= 18;
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_noPads setFrame:NSMakeRect(MARGIN, y - 20, PAD_W, 18)];
    [content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat ry = y - (i + 1) * ROW_H;
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setFrame:NSMakeRect(MARGIN, ry + 4, PAD_W - 170, 18)];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [content addSubview:_padName[i]];
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(MARGIN + PAD_W - 160, ry, 160, 24)];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [content addSubview:_padAssign[i]];
    }
    y -= MAX_PAD_ROWS * ROW_H + MARGIN;

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setFrame:NSMakeRect(MARGIN, y - 30, 70, 30)];
    [_mapButton setRefusesFirstResponder:YES];
    [content addSubview:_mapButton];

    NSButton *defaults = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [defaults setFrame:NSMakeRect(MARGIN + 76, y - 30, 90, 30)];
    [defaults setRefusesFirstResponder:YES];
    [content addSubview:defaults];

    _hint = [NSTextField labelWithString:@""];
    [_hint setFrame:NSMakeRect(MARGIN + 176, y - 24, width - MARGIN - 190, 18)];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setLineBreakMode:NSLineBreakByTruncatingTail];
    [content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refresh)
                                                 name:@"NESPadMapTarget" object:nil];
    _padGeneration = (unsigned)-1;
    [self refresh];
    [self refreshPads];
    return self;
}

/* ---- the controls ------------------------------------------------------------ */

- (void)typeChanged:(NSPopUpButton *)sender
{
    nessession_set_port_type(g_session, (int)sender.tag, (int)sender.indexOfSelectedItem);
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    nessession_gamepad_assign(g_session, (int)sender.tag, (int)sender.indexOfSelectedItem - 1);
    [self refreshPads];
}

- (void)refreshPads
{
    _padGeneration = nessession_gamepad_generation(g_session);
    const int n = nessession_gamepad_count(g_session);
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < n;
        _padName[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        nessession_gamepad_name(g_session, i, name, sizeof name);
        const int eff = nessession_gamepad_effective_port(g_session, i);
        _padName[i].stringValue = eff >= 0
            ? [NSString stringWithFormat:@"%s — player %d", name, eff + 1]
            : [NSString stringWithFormat:@"%s — unused", name];
        [_padAssign[i] selectItemAtIndex:nessession_gamepad_assignment(g_session, i) + 1];
    }
    for (int port = 0; port < 2; port++)
        [_typePopup[port] selectItemAtIndex:nessession_port_type(g_session, port)];
}

/* The live part, twenty times a second while the panel is up: what is held,
 * and whether a gamepad came or went. */
- (void)tick
{
    if (nessession_gamepad_generation(g_session) != _padGeneration) [self refreshPads];
    if (g_mapState != -2) return;
    const unsigned held[2] = { nessession_buttons_held(g_session, 0), nessession_buttons_held(g_session, 1) };
    for (PadButton *b in _buttons) {
        if (b.down || b.padTarget >= NES_TARGET_SWITCH(0)) continue;
        const int port = b.padTarget / NES_ACT_PER_PORT;
        const int act = b.padTarget % NES_ACT_PER_PORT;
        [b setLitState:(held[port] & (1u << act)) != 0];
    }
}

/* ---- Map mode ---------------------------------------------------------------- */

- (void)toggleMap:(id)sender
{
    (void)sender;
    _hint.stringValue = @"";
    [self setMapState:(g_mapState == -2) ? -1 : -2];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    nessession_bindings_reset(g_session);
    _hint.stringValue = @"Default bindings restored";
    [self refresh];
}

- (void)setMapState:(int)state
{
    g_mapState = state;
    [_captureTimer invalidate];
    _captureTimer = nil;
    if (state >= 0) {
        nessession_gamepad_capture_begin(g_session);
        _captureTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [self pollCapture];
        }];
    } else {
        nessession_gamepad_capture_cancel(g_session);
        if (state == -2) _hint.stringValue = @"";
    }
    [self refresh];
}

- (void)pollCapture
{
    int button;
    if (g_mapState < 0) return;
    if (nessession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        const int target = g_mapState;
        nessession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
        [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
        _hint.stringValue = stolen[0]
            ? [NSString stringWithFormat:@"%s: %s (taken from %s)", nes_target_name(target),
                                         nes_pad_button_name(button), stolen]
            : [NSString stringWithFormat:@"%s: %s", nes_target_name(target), nes_pad_button_name(button)];
    }
}

/* In Map mode every button shows its binding; otherwise its face. */
- (void)refresh
{
    [_mapButton setTitle:(g_mapState == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = g_mapState == -2 ? nil : NESAccentColor();
    if (g_mapState == -1 && _hint.stringValue.length == 0)
        [_hint setStringValue:@"Click a control to remap"];
    else if (g_mapState >= 0)
        [_hint setStringValue:[NSString stringWithFormat:@"Press a key or gamepad button for %s",
                               nes_target_name(g_mapState)]];

    for (PadButton *b in _buttons) {
        if (g_mapState != -2) {
            const nes_binding bind = nessession_binding_get(g_session, b.padTarget);
            char key[32];
            nessession_keysym_name(bind.keysym, key, sizeof key);
            NSString *text = key[0] ? [NSString stringWithUTF8String:key] : @"—";
            if (bind.button != NES_PAD_BTN_NONE)
                text = [text stringByAppendingFormat:@" / %s", nes_pad_button_name(bind.button)];
            [b setTitle:text];
            [b setToolTip:[NSString stringWithUTF8String:nes_target_name(b.padTarget)]];
            [b setLitState:(b.padTarget == g_mapState)];
        } else {
            [b setTitle:b.face];
            [b setToolTip:nil];
            if (!b.down) [b setLitState:NO];
        }
    }
}

/* ---- the keyboard ------------------------------------------------------------- */

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key -- except in Map mode, where the next key
 * pressed is the binding. */
- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = NESKeysymFromEvent(e);
    if (g_mapState >= 0) {
        if (ks) {
            char stolen[128], name[32];
            const int target = g_mapState;
            nessession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
            nessession_keysym_name(ks, name, sizeof name);
            [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
            _hint.stringValue = stolen[0]
                ? [NSString stringWithFormat:@"%s: %s (taken from %s)", nes_target_name(target), name, stolen]
                : [NSString stringWithFormat:@"%s: %s", nes_target_name(target), name];
        }
        return;
    }
    if (g_mapState == -1 || !ks) return;
    if (ks == NES_KEYSYM_F9) { [NESControllersWindow toggleWithSession:g_session]; return; }

    const int sa = nessession_key_sysaction(g_session, ks);
    if (sa >= 0) { nessession_sysaction_post(g_session, sa); return; }
    nessession_key(g_session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    if (g_mapState != -2) return;
    const uint32_t ks = NESKeysymFromEvent(e);
    if (ks) nessession_key(g_session, ks, 0);
}

/* Modifiers bind too (Select is on Right Shift by default). */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = NESKeysymFromFlagsChange(e, &down);
    if (!ks) return;
    if (g_mapState >= 0) {
        if (!down) return;
        char stolen[128], name[32];
        const int target = g_mapState;
        nessession_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
        nessession_keysym_name(ks, name, sizeof name);
        [self setMapState:-1];
        _hint.stringValue = [NSString stringWithFormat:@"%s: %s", nes_target_name(target), name];
        return;
    }
    if (g_mapState == -2) nessession_key(g_session, ks, down);
}

/* ---- lifetime ------------------------------------------------------------------ */

- (void)hidePanel
{
    [self setMapState:-2];
    [_liveTimer invalidate];
    _liveTimer = nil;
    nessession_release_all(g_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self hidePanel];
}

+ (void)toggleWithSession:(nessession *)session
{
    g_session = session;
    if (!g_singleton) g_singleton = [[NESControllersWindow alloc] init];

    if ([[g_singleton window] isVisible]) {
        [g_singleton hidePanel];
        [[g_singleton window] orderOut:nil];
    } else {
        [g_singleton refreshPads];
        [g_singleton refresh];
        g_singleton->_liveTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [g_singleton tick];
        }];
        [[g_singleton window] makeKeyAndOrderFront:nil];
    }
}

+ (BOOL)isVisible
{
    return g_singleton && [[g_singleton window] isVisible];
}
@end
