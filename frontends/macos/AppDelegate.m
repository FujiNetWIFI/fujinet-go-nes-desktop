/*
 * NESAppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import "DisplayView.h"
#import "KeyForward.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>
#include <string.h>

#define APP_TITLE @"FujiNet Go NES"

/* How long a gamepad connect/disconnect message stays over the picture. */
#define TOAST_SECONDS 3.0

@class NESAppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels. */
@interface NESContentView : NSView
@property (nonatomic) nessession *session;
@property (nonatomic, weak) NESAppDelegate *owner;
@end

@interface NESAppDelegate ()
- (void)runSysaction:(int)sa;
- (void)loadMedia:(NSString *)path;
@end

@implementation NESContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = NESKeysymFromEvent(e);
    if (!ks) return;
    /* While the emulated keyboard captures, every key is its key (Escape,
     * Tab, F9/F11/F12 included); Cmd shortcuts stay the menus'. */
    if (nessession_keyboard_captures(self.session)) {
        if (!([e modifierFlags] & NSEventModifierFlagCommand)) nessession_key(self.session, ks, 1);
        return;
    }
    if (ks == NES_KEYSYM_F9) { [NESControllersWindow toggleWithSession:self.session]; return; }
    if (ks == NES_KEYSYM_F11) { [[self window] toggleFullScreen:nil]; return; }
    if (ks == NES_KEYSYM_F12) { [NESDebuggerWindow toggleForSession:self.session]; return; }

    const int sa = nessession_key_sysaction(self.session, ks);
    if (sa >= 0) { [self.owner runSysaction:sa]; return; }
    nessession_key(self.session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = NESKeysymFromEvent(e);
    if (ks) nessession_key(self.session, ks, 0);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; Select is on
 * Right Shift by default and would otherwise never press. */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = NESKeysymFromFlagsChange(e, &down);
    if (ks) nessession_key(self.session, ks, down);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [self.owner loadMedia:[(NSURL *)urls[0] path]];
    return YES;
}
@end

@implementation NESAppDelegate {
    nessession *_session;
    const char *_cartPath;
    NSWindow *_window;
    NESDisplayView *_display;
    NESContentView *_content;
    NSTimer *_statusTimer;
    NSTimer *_sysactTimer;

    /* the transient gamepad hot-plug message */
    NSTextField *_toast;
    unsigned _toastGeneration;
    NSUInteger _toastSerial;

    NSMenuItem *_aspectItem;

    NSWindow *_settingsWindow;
    BOOL _sessionDirty;
    NSPopUpButton *_padList, *_padPort, *_aspectPopup;
    NSTimer *_settingsTimer;
    unsigned _padGeneration;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(nessession *)session cartPath:(const char *)cartPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _cartPath = cartPath;
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* 256x240 at 8:7 pixels is 292x240; three times that is a comfortable
     * first window on any Mac. */
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 876, 720)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window center];

    _content = [[NESContentView alloc] initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    _display = [[NESDisplayView alloc] initWithSession:_session];
    [_display setFrame:[_content bounds]];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_display setTvAspect:nessession_get_int(_session, "aspect", 0) == 0];
    [_display setSmooth:nessession_get_int(_session, "smooth", 0) != 0];
    [_content addSubview:_display];

    /* The hot-plug toast: a label over the top of the picture that fades. */
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    [_toast setFrame:NSMakeRect(0, [_content bounds].size.height - 34, [_content bounds].size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [_content addSubview:_toast];

    [_window setContentView:_content];
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];

    nessession_start_opts opts;
    nessession_default_opts(_session, &opts);
    if (_cartPath) opts.cart_path = _cartPath;
    if (nessession_start(_session, &opts) != 0) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Could not start"];
        [a setInformativeText:[NSString stringWithUTF8String:nessession_last_error(_session)]];
        [a runModal];
    }
    _toastGeneration = nessession_gamepad_generation(_session);

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("NES_OPEN_CONTROLLERS")) [NESControllersWindow toggleWithSession:_session];
    if (getenv("NES_OPEN_DEBUGGER")) [NESDebuggerWindow showForSession:_session];
    if (getenv("NES_OPEN_SETTINGS")) [self showSettings:nil];
    if (getenv("NES_OPEN_KEYBOARD")) [NESControllersWindow showKeyboardWithSession:_session];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
        block:^(NSTimer *t) { (void)t; [self updateTitle]; }];
    /* System actions the gamepad thread resolved (it cannot touch AppKit),
     * and gamepads coming and going. */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
        block:^(NSTimer *t) {
            (void)t;
            int sa;
            while (nessession_sysaction_take(self->_session, &sa)) [self runSysaction:sa];
            const unsigned gen = nessession_gamepad_generation(self->_session);
            if (gen != self->_toastGeneration) {
                self->_toastGeneration = gen;
                char msg[160];
                if (nessession_gamepad_last_event(self->_session, msg, sizeof msg) > 0)
                    [self showToast:[NSString stringWithUTF8String:msg]];
            }
        }];
    [self updateTitle];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:nil];
    });
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case NES_SYSACT_RESET_CONFIG: nessession_sysaction(_session, sa); break;
    case NES_SYSACT_PAUSE:
        /* the debugger window attaches, which stops the machine */
        [NESDebuggerWindow showForSession:_session];
        break;
    default: break;
    }
    [self updateTitle];
}

- (void)updateTitle
{
    NSString *state;
    char st[160];
    if (!nessession_is_running(_session)) {
        state = @"stopped";
    } else {
        nessession_cart_status(_session, st, sizeof st);
        state = [NSString stringWithFormat:@"FujiNet: %s", st];
        const char *cart = nessession_cart_path(_session);
        if (cart && cart[0])
            state = [NSString stringWithFormat:@"%@ — %@",
                     [[NSString stringWithUTF8String:cart] lastPathComponent], state];
    }
    /* the expansion-port keyboard and its Data Recorder */
    if (nessession_keyboard_captures(_session))
        state = [state stringByAppendingString:@" — KBD"];
    switch (nessession_tape_state(_session)) {
    case NES_TAPE_PLAYING:   state = [state stringByAppendingString:@" — Tape: PLAY"]; break;
    case NES_TAPE_RECORDING: state = [state stringByAppendingString:@" — Tape: REC"]; break;
    default: break;
    }
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    [_window setTitle:[NSString stringWithFormat:@"%@ — %@", APP_TITLE, state]];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object == _window) nessession_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_statusTimer invalidate];
    [_sysactTimer invalidate];
    [_display stop];
    nessession_stop(_session);
    nessession_free(_session);
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    [self loadMedia:filename];
    return YES;
}

/* ---- menu ------------------------------------------------------------------ */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About " APP_TITLE action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [self item:machine title:@"Open Cartridge…" action:@selector(openCart:) key:@"o"];
    [self item:machine title:@"Eject Cartridge" action:@selector(ejectCart:) key:@""];
    [self item:machine title:@"Import Cartridge to SD…" action:@selector(importToSd:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    /* Backspace is a binding (the console's RESET), not a key equivalent: a
     * menu equivalent would take the key before the bindings table sees it
     * and could not be remapped. */
    [self item:machine title:@"Reset Game (⌫)" action:@selector(resetGame:) key:@""];
    [self item:machine title:@"Reset to CONFIG" action:@selector(resetConfig:) key:@"r"];
    [machine addItem:[NSMenuItem separatorItem]];
    /* A Mac has no Scroll Lock: Cmd+K toggles keyboard mode instead. */
    [self item:machine title:@"Keyboard Mode" action:@selector(toggleKeyboardMode:) key:@"k"];
    NSMenuItem *tapeItem = [machine addItemWithTitle:@"Data Recorder" action:NULL keyEquivalent:@""];
    NSMenu *tape = [[NSMenu alloc] initWithTitle:@"Data Recorder"];
    [self item:tape title:@"Play Tape…" action:@selector(playTape:) key:@""];
    [self item:tape title:@"Record Tape…" action:@selector(recordTape:) key:@""];
    [self item:tape title:@"Stop" action:@selector(stopTape:) key:@""];
    [tapeItem setSubmenu:tape];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"j"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _aspectItem = [self item:view title:@"TV Aspect (8:7 pixels)" action:@selector(toggleAspect:) key:@""];
    [_aspectItem setState:(nessession_get_int(_session, "aspect", 0) == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *sm = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [sm setState:(nessession_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [self item:fuji title:@"Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    [NSApp setMainMenu:bar];
}

/* ---- actions ---------------------------------------------------------------- */

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

- (void)openCartAtPath:(NSString *)path
{
    if (nessession_load_cart(_session, [path fileSystemRepresentation]) != 0)
        [self alert:@"Could not open" text:[NSString stringWithUTF8String:nessession_last_error(_session)]];
    [self updateTitle];
}

- (void)loadMedia:(NSString *)path
{
    if (nessession_media_is_cartridge([path fileSystemRepresentation])) {
        [self openCartAtPath:path];
        return;
    }
    char dest[1024];
    if (nessession_import_media(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[NSString stringWithUTF8String:nessession_last_error(_session)]];
        return;
    }
    [self alert:@"Imported" text:@"Copied to FujiNet's SD folder. Mount it from CONFIG."];
}

- (NSString *)pickCartridge:(NSString *)title
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:title];
    [p setAllowedFileTypes:@[@"nes", @"fuji"]];
    if ([p runModal] != NSModalResponseOK) return nil;
    return [[p URL] path];
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Open Cartridge"];
    if (path) [self openCartAtPath:path];
}

- (void)ejectCart:(id)sender
{
    (void)sender;
    if (nessession_eject(_session) != 0)
        [self alert:@"Could not eject" text:[NSString stringWithUTF8String:nessession_last_error(_session)]];
    [self updateTitle];
}

- (void)importToSd:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Import Cartridge to SD"];
    if (!path) return;
    char dest[1024];
    if (nessession_import_cart_to_sd(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[NSString stringWithUTF8String:nessession_last_error(_session)]];
        return;
    }
    [self alert:@"Imported"
           text:[NSString stringWithFormat:@"%@ is on the SD host. Boot it from CONFIG.",
                 [[NSString stringWithUTF8String:dest] lastPathComponent]]];
}

- (void)resetGame:(id)sender { (void)sender; nessession_reset_game(_session); }
- (void)resetConfig:(id)sender { (void)sender; [self runSysaction:NES_SYSACT_RESET_CONFIG]; }

/* ---- the expansion-port keyboard and the Data Recorder ---------------------- */

- (void)toggleKeyboardMode:(id)sender
{
    (void)sender;
    if (nessession_keyboard(_session) == NES_KBD_NONE) return;
    nessession_set_keyboard_mode(_session, !nessession_keyboard_mode(_session));
    [self updateTitle];
}

- (void)tapeResult:(int)rc title:(NSString *)title
{
    if (rc != 0)
        [self alert:title text:[NSString stringWithUTF8String:nessession_last_error(_session)]];
    [self updateTitle];
}

- (NSURL *)tapesURL
{
    const char *dir = nessession_tapes_path(_session);
    return (dir && dir[0]) ? [NSURL fileURLWithPath:[NSString stringWithUTF8String:dir] isDirectory:YES] : nil;
}

- (void)playTape:(id)sender
{
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:@"Play Tape"];
    NSURL *dir = [self tapesURL];
    if (dir) [p setDirectoryURL:dir];
    if ([p runModal] != NSModalResponseOK) return;
    [self tapeResult:nessession_tape_play(_session, [[[p URL] path] fileSystemRepresentation])
               title:@"Could not play the tape"];
}

- (void)recordTape:(id)sender
{
    (void)sender;
    NSSavePanel *p = [NSSavePanel savePanel];
    [p setTitle:@"Record Tape"];
    NSURL *dir = [self tapesURL];
    if (dir) [p setDirectoryURL:dir];
    [p setNameFieldStringValue:@"untitled.fbt"];
    if ([p runModal] != NSModalResponseOK) return;
    [self tapeResult:nessession_tape_record(_session, [[[p URL] path] fileSystemRepresentation])
               title:@"Could not record a tape"];
}

- (void)stopTape:(id)sender
{
    (void)sender;
    [self tapeResult:nessession_tape_stop(_session) title:@"Could not stop the tape"];
}

/* Keyboard Mode needs a keyboard; the Data Recorder comes with the Family
 * BASIC keyboard only. The check mark follows the session (Scroll Lock on
 * an external keyboard toggles it too). */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    const int kbd = nessession_keyboard(_session);
    if (a == @selector(toggleKeyboardMode:)) {
        [item setState:(kbd != NES_KBD_NONE && nessession_keyboard_mode(_session))
                       ? NSControlStateValueOn : NSControlStateValueOff];
        return kbd != NES_KBD_NONE;
    }
    if (a == @selector(playTape:) || a == @selector(recordTape:))
        return kbd == NES_KBD_FAMILY_BASIC;
    if (a == @selector(stopTape:))
        return kbd == NES_KBD_FAMILY_BASIC && nessession_tape_state(_session) != NES_TAPE_IDLE;
    return YES;
}

- (void)toggleControllers:(id)sender { (void)sender; [NESControllersWindow toggleWithSession:_session]; }
- (void)toggleDebugger:(id)sender { (void)sender; [NESDebuggerWindow toggleForSession:_session]; }

- (void)setAspect:(int)aspect
{
    nessession_set_int(_session, "aspect", aspect);
    [_display setTvAspect:aspect == 0];
    [_aspectItem setState:(aspect == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    if (_aspectPopup) [_aspectPopup selectItemAtIndex:aspect];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    [self setAspect:nessession_get_int(_session, "aspect", 0) == 0 ? 1 : 0];
}

- (void)toggleSmooth:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setSmooth:on];
    nessession_set_int(_session, "smooth", on ? 1 : 0);
}

/* ---- settings ----------------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. Controllers, the analog
 * stick, the region (at the next power cycle), the picture and the volume
 * apply live; the host options restart the session when the window closes.
 */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSTextField *)note:(NSString *)text
{
    NSTextField *n = [NSTextField labelWithString:text];
    n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    n.textColor = NSColor.secondaryLabelColor;
    return n;
}

- (NSPopUpButton *)popUpForKey:(const char *)key fallback:(int)def names:(const char *(*)(int))names
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    for (int i = 0; names(i); i++) [popup addItemWithTitle:[NSString stringWithUTF8String:names(i)]];
    NSInteger current = nessession_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)popup.numberOfItems) current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(settingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key title:(NSString *)title fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(settingChanged:)];
    box.state = nessession_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

- (void)refreshPadList
{
    const NSInteger sel = _padList.indexOfSelectedItem;
    [_padList removeAllItems];
    const int n = nessession_gamepad_count(_session);
    if (n == 0) [_padList addItemWithTitle:@"(no gamepads connected)"];
    for (int i = 0; i < n; i++) {
        char name[128];
        const int eff = nessession_gamepad_effective_port(_session, i);
        nessession_gamepad_name(_session, i, name, sizeof name);
        /* Menu items with equal titles collapse; the index keeps them apart. */
        [_padList addItemWithTitle:(eff >= 0
            ? [NSString stringWithFormat:@"%d: %s  [player %d]", i + 1, name, eff + 1]
            : [NSString stringWithFormat:@"%d: %s  [unused]", i + 1, name])];
    }
    if (sel >= 0 && sel < n) [_padList selectItemAtIndex:sel];
    [self padSelected:nil];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < nessession_gamepad_count(_session))
        [_padPort selectItemAtIndex:nessession_gamepad_assignment(_session, (int)sel) + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < nessession_gamepad_count(_session))
        nessession_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

- (void)volumeChanged:(NSSlider *)sender
{
    nessession_set_volume(_session, (int)sender.integerValue);
}

- (void)aspectChanged:(NSPopUpButton *)sender
{
    [self setAspect:(int)sender.indexOfSelectedItem];
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    const char *key = [control.identifier UTF8String];
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "port0_type") || !strcmp(key, "port1_type")) {
        nessession_set_port_type(_session, key[4] == '1', value);
        return;
    }
    if (!strcmp(key, "region")) {
        nessession_set_region(_session, value);
        return;
    }
    if (!strcmp(key, "keyboard")) {
        nessession_set_keyboard(_session, value);
        [self updateTitle];
        return;
    }
    if (!strcmp(key, "analog_joystick")) {
        nessession_set_analog(_session, value);
        return;
    }
    nessession_set_int(_session, key, value);
    _sessionDirty = YES;
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    _padList = [[NSPopUpButton alloc] init];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    _padPort = [[NSPopUpButton alloc] init];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Player 1", @"Player 2"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);

    _aspectPopup = [[NSPopUpButton alloc] init];
    [_aspectPopup addItemsWithTitles:@[@"4:3 TV (8:7 pixels)", @"Square pixels"]];
    [_aspectPopup selectItemAtIndex:(nessession_get_int(_session, "aspect", 0) ? 1 : 0)];
    _aspectPopup.target = self;
    _aspectPopup.action = @selector(aspectChanged:);

    NSSlider *volume = [NSSlider sliderWithValue:nessession_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;

    NSStackView *padRow = [NSStackView stackViewWithViews:@[_padList, _padPort]];
    padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [self note:@"region takes effect at the next power cycle"] ],
        @[ [NSTextField labelWithString:@"Region"],
           [self popUpForKey:"region" fallback:NES_REGION_AUTO names:nes_region_name] ],
        @[ [NSTextField labelWithString:@"Picture"], _aspectPopup ],
        @[ [self sectionLabel:@"Controllers"], [self note:@"applied immediately"] ],
        @[ [NSTextField labelWithString:@"Player 1"],
           [self popUpForKey:"port0_type" fallback:NES_CTRL_STANDARD names:nes_ctrl_type_name] ],
        @[ [NSTextField labelWithString:@"Player 2"],
           [self popUpForKey:"port1_type" fallback:NES_CTRL_STANDARD names:nes_ctrl_type_name] ],
        @[ [NSTextField labelWithString:@""],
           [self checkBoxForKey:"analog_joystick" title:@"Analog sticks drive the D-pad" fallback:1] ],
        @[ [NSTextField labelWithString:@"Gamepads"], padRow ],
        @[ [NSTextField labelWithString:@"Expansion port"],
           [self popUpForKey:"keyboard" fallback:NES_KBD_NONE names:nes_keyboard_name] ],
        @[ [NSTextField labelWithString:@"Volume"], volume ],
        @[ [self sectionLabel:@"Host"], [self note:@"applied by restarting the session"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_gamepad" title:@"Gamepads" fallback:1] ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSStackView *root = [NSStackView stackViewWithViews:@[grid]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 560, 520)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];

    _padGeneration = nessession_gamepad_generation(_session);
    [self refreshPadList];
    _settingsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
        (void)t;
        const unsigned gen = nessession_gamepad_generation(self->_session);
        if (gen != self->_padGeneration) { self->_padGeneration = gen; [self refreshPadList]; }
    }];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    if (note.object != _settingsWindow) return;
    [_settingsTimer invalidate];
    _settingsTimer = nil;
    if (!_sessionDirty) return;
    _sessionDirty = NO;

    nessession_start_opts o;
    nessession_settings_flush(_session);
    nessession_default_opts(_session, &o);
    nessession_stop(_session);
    if (nessession_start(_session, &o) != 0)
        [self alert:@"Could not start" text:[NSString stringWithUTF8String:nessession_last_error(_session)]];
    [self updateTitle];
}

/* ---- FujiNet console log ------------------------------------------------------- */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = nessession_fujinet_copy_log(_session, buf, sizeof buf);
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf] : nil;
    [_logView setString:(text ?: @"(no FujiNet output yet)")];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        if (!_logTimer)
            _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                       selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;

    _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _logView.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = _logView;

    _logWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;
    _logWindow.delegate = self;
    _logWindow.contentView = scroll;
    [_logWindow center];

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                               selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (!nessession_fujinet_running(_session)) {
        [self alert:@"FujiNet is not running" text:nil];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:nessession_fujinet_webui_url(_session)]]];
}
@end
