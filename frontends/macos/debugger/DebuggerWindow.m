/*
 * Debugger window (AppKit) over MesenCE's own debugger engine, via
 * core/include/nesdebug.h. Twelve tabs, as on the GNOME, KDE and Windows
 * debuggers: Console, Disassembly, Memory, Breakpoints, Watches, Cart & I/O,
 * Trace, History, Video, Sound, Symbols, FujiNet, over an editable register
 * strip. The Video tab is VideoTab.m, a view over core/include/nesvidtab.h;
 * the old PPU panes are its Decoded sources.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it (F12, the close button, the menu) turns the Trace and History records
 * off, detaches, and the machine runs on. F12 in the machine's window
 * toggles it.
 *
 * The engine serialises its calls and blocks during a power cycle, so while
 * the machine runs the window reads almost nothing back: a 100 ms NSTimer
 * polls nesdebug_stop_snapshot() (the stop's serial) and nesdebug_generation()
 * and refreshes the tabs on a stop, a resume, a command or when a tab is
 * shown. A running machine is followed only by the Video tab (every 0.3 s,
 * it copies finished frames), the Trace tab (every 0.5 s, a few hundred rows)
 * and the FujiNet tab (every 0.5 s, its own trace). After a step the window
 * waits a moment (~40 ms) for the stop to settle before it reads anything
 * back; a longer run (Frame, Step Out, Run to Cursor) is picked up by the
 * poll when it stops.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 stop/run, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 toggle the breakpoint on the selected disassembly
 * line, F12 close.
 *
 * Environment hooks, for tests and captures: NES_DEBUGGER_TAB (an index, a
 * tab's name or id, or an older name: prompt, cpu, ppu, apu),
 * NES_DEBUGGER_WATCH, NES_DEBUGGER_WATCH_BREAK, NES_DEBUGGER_MEM_GOTO,
 * NES_DEBUGGER_HISTORY, NES_DEBUGGER_POPOUT (a tab), and
 * NES_DEBUGGER_VIDEO_* (read by the Video tab's controller). NES_OPEN_DEBUGGER
 * opens the window at launch (AppDelegate).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"
#import "VideoTab.h"
#import "../help/HelpWindow.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "nesdebug.h"
#include "nesmemtools.h"
#include "nessession.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 6
#define NUM_FLAGS 6
#define MEM_ROWS 32            /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512          /* FujiNet packets shown */
#define VIDEO_TICKS 3          /* the Video tab follows a running machine, every 0.3 s */
#define TRACE_TICKS 5          /* the Trace tab, every 0.5 s */
#define FUJI_TICKS 5           /* the FujiNet tab, every 0.5 s */
#define HIST_BUF 480           /* records read from the ring: a big read pauses the machine */
#define HIST_SHOWN 400         /* lines shown */
#define MEM_SPACES 7
#define BP_SPACES 4
#define MAP_MAX 24

static NESDebuggerWindow *g_debugger;

/* The tabs, in order. The identifiers are the help contexts' ends
 * (debugger/<id>, data/help/help.md). NES_DEBUGGER_TAB picks one by number,
 * by the start of its title or id (any case), or by an older name ("prompt",
 * "cpu", "ppu", "apu"), for tests and captures. */
static const char *const kTabNames[] = {"Console", "Disassembly", "Memory", "Breakpoints", "Watches", "Cart & I/O",
                                        "Trace", "History", "Video", "Sound", "Symbols", "FujiNet"};
static const char *const kTabIds[] = {"console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                      "trace", "history", "video", "sound", "symbols", "fujinet"};
#define TAB_COUNT ((int)(sizeof(kTabNames) / sizeof(kTabNames[0])))
enum { MT_CONSOLE, MT_DISASM, MT_MEMORY, MT_BREAKS, MT_WATCHES, MT_CART, MT_TRACE, MT_HISTORY, MT_VIDEO, MT_SOUND,
       MT_SYMBOLS, MT_FUJINET };

static int tabFromText(const char *tab)
{
    if (!tab || !*tab) return -1;
    char *end;
    const long count = strtol(tab, &end, 10);
    if (end != tab && *end == '\0') return count >= 0 && count < TAB_COUNT ? (int)count : -1;
    if (strcasecmp(tab, "prompt") == 0) return MT_CONSOLE;
    if (strcasecmp(tab, "cpu") == 0) return MT_MEMORY;
    if (strcasecmp(tab, "ppu") == 0) return MT_VIDEO;
    if (strcasecmp(tab, "apu") == 0) return MT_SOUND;
    for (int idx = 0; idx < TAB_COUNT; idx++)
        if (strncasecmp(tab, kTabNames[idx], strlen(tab)) == 0 || strncasecmp(tab, kTabIds[idx], strlen(tab)) == 0)
            return idx;
    return -1;
}

/* The address spaces the Memory tab offers, and the four breakpoints take. */
static const int kMemSpaces[MEM_SPACES] = {NESDEBUG_SPACE_CPU, NESDEBUG_SPACE_PPU, NESDEBUG_SPACE_OAM,
                                           NESDEBUG_SPACE_PALETTE, NESDEBUG_SPACE_PRG, NESDEBUG_SPACE_CHR,
                                           NESDEBUG_SPACE_WRAM};
static const int kBpSpaces[BP_SPACES] = {NESDEBUG_SPACE_CPU, NESDEBUG_SPACE_PPU, NESDEBUG_SPACE_PALETTE,
                                         NESDEBUG_SPACE_OAM};

/* The stopped Run button, the PC's line and a marked row wear the accent (the
 * logo's red) with its ink on top; changed values wear the light variant,
 * which reads on a dark theme. */
static NSColor *rgbColor(unsigned rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *accentColor(void)
{
    return rgbColor(NESSESSION_ACCENT_RGB);
}

static NSColor *accentInk(void)
{
    return rgbColor(NESSESSION_ACCENT_INK_RGB);
}

static NSColor *accentLight(void)
{
    return rgbColor(NESSESSION_ACCENT_LIGHT_RGB);
}

/* Disassembly text view: a click selects the clicked line (Run to Cursor);
 * a click on its gutter, or a double-click, toggles its breakpoint; the
 * wheel browses by instruction. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation DasmTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger charPos = 0; charPos < idx; charPos++)
        if ([text characterAtIndex:charPos] == '\n') { line++; lineStart = charPos + 1; }
    if (self.onClick) self.onClick(line, (int)(idx - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0) return;
    _wheel -= rows;
    if (self.onScroll) self.onScroll(rows);
}
@end

/* A list of lines the user picks one of by clicking (the Watches tab, and the
 * Symbols tab, where the pick is the address to go to). The picked line is
 * kept in `picked`, -1 for none, and drawn with a background by whoever owns
 * the text. */
@interface PickTextView : NSTextView
@property (nonatomic) int picked;
@property (nonatomic, copy) void (^onPick)(int line, NSInteger clicks);
@end

@implementation PickTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger charPos = 0; charPos < idx; charPos++)
        if ([text characterAtIndex:charPos] == '\n') line++;
    self.picked = line;
    if (self.onPick) self.onPick(line, event.clickCount);
}
@end

@interface NESDebuggerWindow () <NSTabViewDelegate>
- (instancetype)initWithSession:(nessession *)session;
- (void)refreshNow;
@end

@implementation NESDebuggerWindow {
    nessession *_session;
    nesdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    uint64_t _seenSerial;
    unsigned _seenGen;
    BOOL _wasStopped;
    BOOL _envApplied;
    int _videoTicks, _traceTicks, _fujiTicks;
    NSTabView *_tabs;

    /* Tabs peeled off into windows of their own */
    NSTabViewItem *_items[16];
    NSWindow *_popped[16];

    NSButton *_runBtn;
    NSTextField *_status;

    /* Console */
    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    /* The register strip under every tab */
    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_regInfo;

    /* Disassembly */
    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _selected;              /* the selected line's address, or -1 */

    /* Memory: the space, the dump, the highlighted range, the Poke bar and the search */
    NSPopUpButton *_memSpace;
    NSTextView *_mem;
    NSTextField *_memGoto, *_memWrite, *_memNote;
    int64_t _memBase;           /* the dump's first address */
    BOOL _memHl;
    uint32_t _memHlLo, _memHlHi;
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;

    /* Breakpoints */
    NSPopUpButton *_bpType, *_bpSpace;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr, *_bpStop;
    NSStackView *_bpList;
    NSString *_bpSig;

    /* Watches */
    NSTextField *_wtEntry;
    PickTextView *_wtList;

    /* Cart & I/O, Sound */
    NSTextView *_cartText, *_soundText;

    /* Trace (0, live) and History (1, when stopped) share one ring */
    NSButton *_hRec[2];
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];
    uint64_t _hSeen[2];

    /* Video */
    NESVideoTab *_video;

    /* Symbols */
    PickTextView *_symList;
    NSTextField *_symFilter, *_symCount;
    NSString *_symSig;
    NSMutableArray<NSNumber *> *_symAddrs;

    /* FujiNet */
    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}

#define NUM_FILE_KINDS 5
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "prg", "chr", "ram", "wram" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"PRG…", @"CHR…", @"RAM…", @"WRAM…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"prg.bin", @"chr.bin", @"ram.bin", @"wram.bin"
};

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *source)
{
    if (!source) return @"";
    const size_t length = strlen(source);
    char *copy = malloc(length + 1);
    if (!copy) return @"";
    size_t outPos = 0;
    for (size_t index = 0; index < length; index++) {
        const unsigned char chr = (unsigned char)source[index];
        if (chr >= 0x20 || chr == '\n' || chr == '\t') copy[outPos++] = (char)chr;
    }
    copy[outPos] = '\0';
    NSString *result = [NSString stringWithUTF8String:copy];
    if (!result) result = [[NSString alloc] initWithBytes:copy length:outPos encoding:NSISOLatin1StringEncoding];
    free(copy);
    return result ?: @"";
}

/* $hex, 0xhex, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *token = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([token hasPrefix:@"$"]) token = [token substringFromIndex:1];
    else if ([token.lowercaseString hasPrefix:@"0x"]) token = [token substringFromIndex:2];
    else if ([token hasPrefix:@"#"]) { token = [token substringFromIndex:1]; base = 10; }
    if (token.length == 0) return NO;
    char *end;
    long number = strtol(token.UTF8String, &end, base);
    if (*end) return NO;
    *out = number;
    return YES;
}

/* A label, a number, or an expression, in the CPU's 64K. -1 when none. */
- (int)resolveAddr:(NSString *)text
{
    long value;
    NSString *slot = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (slot.length == 0) return -1;
    int addr = nesdebug_label_address(_dbg, slot.UTF8String);
    if (addr < 0 && parseNum(slot, &value) && value >= 0 && value <= 0xffff) addr = (int)value;
    if (addr < 0) {
        char err[160];
        err[0] = '\0';
        if (nesdebug_eval(_dbg, slot.UTF8String, &value, err, (int)sizeof err) == 0 && value >= 0 && value <= 0xffff) addr = (int)value;
    }
    return addr;
}

/* An address in one of the machine's spaces: the CPU's takes labels and
 * expressions; the others a number (hex unless marked), then an expression.
 * -1 when none. */
- (int64_t)resolveIn:(int)space text:(NSString *)text
{
    if (space == NESDEBUG_SPACE_CPU) return [self resolveAddr:text];
    long value;
    NSString *slot = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (slot.length == 0) return -1;
    if (parseNum(slot, &value) && value >= 0) return value;
    char err[160];
    err[0] = '\0';
    if (nesdebug_eval(_dbg, slot.UTF8String, &value, err, (int)sizeof err) == 0 && value >= 0) return value;
    return -1;
}

/* A bare run of hex digits is hex (the fields show hex); anything else is an
 * expression for the core to evaluate ($1234, 0x1234, a label, x + 1). */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *trim = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    const char *chars = trim.UTF8String;
    char err[160];
    if (!*chars) {
        *why = @"nothing to apply";
        return NO;
    }
    char *end;
    const unsigned long hex = strtoul(chars, &end, 16);
    if (*end == '\0') {
        *out = (long)hex;
        return YES;
    }
    err[0] = '\0';
    if (nesdebug_eval(_dbg, chars, out, err, (int)sizeof err) == 0) return YES;
    *why = err[0] ? cstr(err) : @"does not parse";
    return NO;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with its ink on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: accentColor(), NSForegroundColorAttributeName: accentInk() };
}

/* A watched byte in the Memory tab: underlined in the light accent. */
static NSDictionary *watchAttrs(void)
{
    return @{ NSForegroundColorAttributeName: accentLight(), NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle) };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:origin];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:origin];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

/* Text for a pick list, the picked line drawn on the accent. */
static void setPickText(PickTextView *view, NSArray<NSString *> *lines)
{
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    int line = 0;
    for (NSString *source in lines) {
        NSMutableAttributedString *entry = [[NSMutableAttributedString alloc]
            initWithString:[source stringByAppendingString:@"\n"] attributes:monoAttrs()];
        if (line == view.picked) [entry addAttributes:accentAttrs() range:NSMakeRange(0, entry.length)];
        [text appendAttributedString:entry];
        line++;
    }
    setStyledText(view, text);
}

static PickTextView *makePickView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    PickTextView *view = [[PickTextView alloc] initWithFrame:NSMakeRect(0, 0, 900, 400)];
    view.editable = NO;
    view.richText = YES;
    view.selectable = NO;
    view.font = monoFont();
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    view.picked = -1;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *button = [NSButton buttonWithTitle:title target:self action:sel];
    [button setRefusesFirstResponder:YES];
    return button;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *label = [self label:text];
    label.textColor = NSColor.secondaryLabelColor;
    return label;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *field = [[NSTextField alloc] init];
    field.placeholderString = placeholder;
    field.font = monoFont();
    field.target = self;
    field.action = sel;
    if (width > 0) [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [popup addItemWithTitle:title];
    popup.lastItem.tag = -1;
    for (NSUInteger index = 0; index < items.count; index++) {
        [popup addItemWithTitle:items[index]];
        popup.lastItem.tag = (NSInteger)index;
    }
    popup.target = self;
    popup.action = sel;
    [popup setRefusesFirstResponder:YES];
    return popup;
}

/* A pop-up of the engine's address-space names. */
- (NSPopUpButton *)spacePopup:(const int *)spaces count:(int)count action:(SEL)sel
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int idx = 0; idx < count; idx++) [popup addItemWithTitle:@(nesdebug_space_name(spaces[idx]))];
    if (sel) {
        popup.target = self;
        popup.action = sel;
    }
    [popup setRefusesFirstResponder:YES];
    return popup;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing = 6;
    return stack;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 6;
    return stack;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* Why the machine stopped, worded by the core. Empty while running. */
static NSString *stopText(const nesdebug_snapshot *snap)
{
    return snap->paused ? cstr(snap->text) : @"";
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(nessession *)session
{
    if (!g_debugger) g_debugger = [[NESDebuggerWindow alloc] initWithSession:session];
    [g_debugger show];
}

+ (void)toggleForSession:(nessession *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: records off, detached, the machine runs on */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (void)show
{
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    for (int slot = 0; slot < TAB_COUNT; slot++) [_popped[slot] orderFront:nil];
    nesdebug_attach(_dbg);      /* stops the machine, as on every sibling */
    [self applyEnvironment];
    [self refreshNow];
    if ([self tabVisible:MT_CONSOLE]) [_window makeFirstResponder:_conIn];
}

/* The captures' hooks, once the engine is up (closing the window turns the
 * Trace and History records off, so they could not be set before):
 * NES_DEBUGGER_WATCH="x" adds a watch, NES_DEBUGGER_WATCH_BREAK="x" a breakpoint on a write
 * that changes it, NES_DEBUGGER_MEM_GOTO="x" shows it in Memory, NES_DEBUGGER_HISTORY records
 * the History ring, NES_DEBUGGER_POPOUT=<tab> pops that tab out into a window. */
- (void)applyEnvironment
{
    if (_envApplied) return;
    _envApplied = YES;
    const char *watchText = getenv("NES_DEBUGGER_WATCH"), *breakText = getenv("NES_DEBUGGER_WATCH_BREAK"),
               *gotoText = getenv("NES_DEBUGGER_MEM_GOTO");
    uint16_t first, last;
    if (watchText && *watchText) neswatch_add(watchText);
    if (breakText && *breakText && nessym_parse_range(_dbg, breakText, &first, &last))
        [self breakOnChangeFrom:first to:last];
    if (gotoText && *gotoText && nessym_parse_range(_dbg, gotoText, &first, &last))
        [self showInMemorySpace:NESDEBUG_SPACE_CPU from:first last:last];
    if (getenv("NES_DEBUGGER_HISTORY")) nesdebug_history_enable(_dbg, 1);
    const int pop = tabFromText(getenv("NES_DEBUGGER_POPOUT"));
    if (pop >= 0) [self popOutTab:pop];
}

- (instancetype)initWithSession:(nessession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = nessession_debugger(session);
    _fujiCount = -1;
    _selected = -1;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    _history = [NSMutableArray array];
    _symAddrs = [NSMutableArray array];
    [self buildWindow];

    __weak NESDebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/F9/F12 wherever the focus is in this window (or a tab peeled
     * off into its own), the console's field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        NESDebuggerWindow *owner = weakSelf;
        if (!owner || ![owner ownsWindow:event.window]) return event;
        return [owner handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window) return NO;
    if (window == _window) return _window.isVisible;
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (window == _popped[slot]) return YES;
    return NO;
}

/* The main window's close control and Window > Close: a peeled-off tab's
 * window docks it instead. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (sender == _popped[slot]) {
            [self dockTab:slot];
            return NO;
        }
    return YES;
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    for (int slot = 0; slot < TAB_COUNT; slot++) [_popped[slot] orderOut:nil];
    nesdebug_trace_enable(_dbg, 0);
    nesdebug_history_enable(_dbg, 0);
    nesdebug_detach(_dbg);      /* the machine runs on */
    _envApplied = YES;          /* the hooks are for the first opening only */
}

/* On screen: the tab view's selected page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || _tabs.selectedTabViewItem == _items[tab];
}

/* The poll: refresh on a stop (its serial), a resume or a command (the
 * generation). A running machine is followed by the Video tab (it copies
 * finished frames), the Trace tab and the FujiNet tab only: the engine is not
 * read otherwise. The FujiNet tab also follows the cartridge's packets while
 * stopped, as they arrive on their own. */
- (void)onTick
{
    if (![_window isVisible]) return;
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(_dbg, &snap);
    const unsigned gen = nesdebug_generation(_dbg);
    const BOOL stopped = snap.paused != 0;
    if (snap.serial != _seenSerial || gen != _seenGen || stopped != _wasStopped) {
        [self refreshNow];
        return;
    }
    if (!stopped && [self tabVisible:MT_VIDEO] && ++_videoTicks >= VIDEO_TICKS) {
        _videoTicks = 0;
        [_video refreshForce:NO];
    }
    if ([self tabVisible:MT_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;
        [self refreshHist:0 force:NO];
    }
    if ([self tabVisible:MT_FUJINET] && ++_fujiTicks >= FUJI_TICKS) {
        _fujiTicks = 0;
        [self refreshFujinet:NO];
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as stopped. */
- (void)settleFrom:(unsigned)gen
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (nesdebug_generation(_dbg) != gen && nesdebug_is_stopped(_dbg)) break;
        usleep(2000);
    }
}

/* Help's contexts (data/help/help.md): the tab identifiers are the contexts' ends */
- (void)updateHelpContext
{
    NSString *ident = _tabs.selectedTabViewItem.identifier;
    if (ident) NESSetHelpContext(_window, [@"debugger/" stringByAppendingString:ident]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self updateHelpContext];
    [self refreshVisibleForce:YES];
    if ([self tabVisible:MT_CONSOLE]) [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1) return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch ([chars characterAtIndex:0]) {
    case NSF5FunctionKey: [self toggleRun:nil]; return YES;
    case NSF7FunctionKey: if (shift) [self frame:nil]; else [self step:nil]; return YES;
    case NSF8FunctionKey: if (shift) [self stepOut:nil]; else [self stepOver:nil]; return YES;
    case NSF9FunctionKey: [self toggleSelected:nil]; return YES;
    case NSF12FunctionKey: [_window performClose:nil]; return YES;
    default: return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

/* A tab's content, padded and pinned on all four sides of its page. */
- (NSView *)pageHolding:(NSView *)content
{
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    return holder;
}

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; anything else is an error. "
                     @"Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, break $C000, bpw $0300, print a, mem $6000 32, video, ...)"
                   width:0 action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *column = vstack(@[outScroll, _conIn]);
    fill(column, outScroll);
    fill(column, _conIn);
    return column;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it (Run to Cursor); click its left edge, double-click it "
                                           @"or press F9 for a breakpoint (* on, o off); scroll to browse"]]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = monoFont();
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak NESDebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        NESDebuggerWindow *owner = weakSelf;
        if (!owner || line < 0 || line >= owner->_lineCount) return;
        const BOOL gutter = column < 2;
        owner->_selected = owner->_lineAddr[line];
        if (gutter ? clicks == 1 : clicks == 2) [owner toggleBreakpointAt:owner->_lineAddr[line]];
        else [owner refreshDisasm];
    };
    _disasm.onScroll = ^(int rows) {
        NESDebuggerWindow *owner = weakSelf;
        if (!owner || rows == 0) return;
        owner->_followPc.state = NSControlStateValueOff;
        owner->_disasmAddr = (uint16_t)nesdebug_row_address(owner->_dbg, owner->_disasmAddr, rows);
        [owner refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *column = vstack(@[row, scroll]);
    fill(column, row);
    fill(column, scroll);
    return column;
}

/* Any of the machine's address spaces: a go-to bar, a search, the dump and a Poke bar. */
- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);
    _memSpace = [self spacePopup:kMemSpaces count:MEM_SPACES action:@selector(memSpaceChanged:)];
    _memSpace.toolTip = @"CPU: the 64K bus as mapped now. PPU: pattern tables and nametables. OAM: sprite RAM. "
                        @"Palette: the 32 palette bytes. PRG, CHR, WRAM: the cartridge's own memory.";
    _memGoto = [self field:@"address or label" width:170 action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Space:"], _memSpace, [self label:@"Address:"], _memGoto,
                                    [self button:@"Go" action:@selector(gotoMem:)], prev, next,
                                    [self hint:@"CPU: as the CPU sees it, through the mapper"]]);

    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[@"Hex bytes", @"Text"]];
    _srchKind.toolTip = @"Hex bytes (A9 00 8D) or text, one character per byte";
    _srchEntry = [self field:@"A9 00 8D, or some text" width:0 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);
    searchRow.distribution = NSStackViewDistributionFill;

    _memWrite = [self field:@"address value ... (hex; several values go to consecutive addresses)" width:0
                     action:@selector(writeMem:)];
    _memNote = [self hint:@""];
    NSStackView *pokeRow = hstack(@[[self label:@"Poke"], _memWrite, [self button:@"Write" action:@selector(writeMem:)],
                                    _memNote]);
    pokeRow.distribution = NSStackViewDistributionFill;

    NSStackView *column = vstack(@[memHead, searchRow, memScroll, pokeRow]);
    fill(column, memHead);
    fill(column, searchRow);
    fill(column, memScroll);
    fill(column, pokeRow);
    [memScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

/* One breakpoint-table cell: a fixed-width monospace label, or (width 0) the
 * flexible one that takes what is left. */
static NSTextField *bpCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [label.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [label setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return label;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to
     * the top and to the clip view's width so rows keep the tab's width. */
    NSStackView *header = hstack(@[bpCell(@"On  #", 70, YES), bpCell(@"Type", 60, YES), bpCell(@"Space", 60, YES),
                                   bpCell(@"Range", 170, YES), bpCell(@"Condition", 0, YES), bpCell(@"Hits", 60, YES),
                                   bpCell(@"", 80, YES), bpCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Access"]];
    _bpType.toolTip = @"Execute stops before an instruction (CPU space only); Read, Write and Access stop at the "
                      @"access (a CPU access stops before the instruction that makes it)";
    _bpSpace = [self spacePopup:kBpSpaces count:BP_SPACES action:nil];
    _bpSpace.toolTip = @"CPU: the bus. PPU: pattern tables and nametables ($0000-$3EFF). Palette: $00-$1F. OAM: $00-$FF.";
    _bpStart = [self field:@"$2005, $C000 or a label" width:170 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): value != old, a == 5, [$C000] > 3" width:0 action:@selector(addBreakpoint:)];
    _bpCond.toolTip = @"A Mesen expression: a x y sp ps pc, [addr] and {addr}, labels, frame cycle scanline and, on an "
                      @"access, value, address and (CPU space) old, the byte a write replaced.";
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear All" action:@selector(clearBreaks:)];
    NSButton *hits = [self button:@"Clear Hits" action:@selector(clearHits:)];
    NSStackView *row = hstack(@[_bpType, _bpSpace, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, hits, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    _bpStop = [self hint:@""];
    NSTextField *hint = [self hint:@"Untick to disable. The end defaults to the start. A stop in PPU space reports a PC a few "
                                   @"cycles into the next instruction."];

    NSStackView *column = vstack(@[header, bpScroll, hint, row, _bpErr, _bpStop]);
    fill(column, header);
    fill(column, bpScroll);
    fill(column, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

- (NSView *)buildWatches
{
    _wtEntry = [self field:@"address, label, expression, or lo-hi" width:0 action:@selector(addWatch:)];
    NSStackView *form = hstack(@[[self label:@"Watch"], _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    form.distribution = NSStackViewDistributionFill;

    NSScrollView *scroll;
    _wtList = makePickView(&scroll);
    __weak NESDebuggerWindow *weakSelf = self;
    _wtList.onPick = ^(int line, NSInteger clicks) {
        (void)line;
        (void)clicks;
        [weakSelf refreshWatches];
    };

    NSStackView *foot = hstack(@[[self button:@"Memory" action:@selector(wtMemory:)],
                                 [self button:@"Break on Change" action:@selector(wtBreak:)],
                                 [self button:@"Remove" action:@selector(wtRemove:)],
                                 [self hint:@"Values show while the machine is stopped"]]);

    NSStackView *column = vstack(@[form, scroll, foot]);
    fill(column, form);
    fill(column, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

- (NSView *)buildTextTab:(NSTextView *__strong *)view
{
    NSScrollView *scroll;
    *view = monoView(&scroll, NO);
    return scroll;
}

- (NSView *)buildHist:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].tag = idx;
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs slower while it does)";
    _hFilter[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[idx] addItemsWithTitles:@[@"All instructions", @"Branches only", @"In range"]];
    _hFilter[idx].target = self;
    _hFilter[idx].action = @selector(histChanged:);
    _hRange[idx] = [self field:@"range: lo-hi or a label" width:200 action:@selector(histChanged:)];
    _hNote[idx] = [self hint:@""];
    NSButton *clear = [self button:@"Clear" action:@selector(histClear:)];
    NSStackView *bar = hstack(@[_hRec[idx], _hFilter[idx], _hRange[idx], clear, _hNote[idx]]);
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll, NO);
    NSStackView *column = vstack(@[bar, scroll]);
    fill(column, bar);
    fill(column, scroll);
    return column;
}

- (NSView *)buildVideo
{
    _video = [[NESVideoTab alloc] initWithSession:_session];
    NSView *view = _video.view;
    NSStackView *column = vstack(@[view]);
    [view.widthAnchor constraintEqualToAnchor:column.widthAnchor].active = YES;
    [view.heightAnchor constraintEqualToAnchor:column.heightAnchor].active = YES;
    return column;
}

- (NSView *)buildSymbols
{
    _symFilter = [self field:@"Filter (Enter to apply)" width:0 action:@selector(symFilterChanged:)];
    _symCount = [self hint:@""];
    NSStackView *filterRow = hstack(@[_symFilter, _symCount]);
    filterRow.distribution = NSStackViewDistributionFill;

    NSScrollView *scroll;
    _symList = makePickView(&scroll);
    __weak NESDebuggerWindow *weakSelf = self;
    _symList.onPick = ^(int line, NSInteger clicks) {
        NESDebuggerWindow *owner = weakSelf;
        if (!owner || line < 0 || line >= (int)owner->_symAddrs.count) return;
        owner->_symSig = nil;       /* redraw with the pick */
        [owner refreshSymbols];
        if (clicks >= 2) {      /* activating a row goes to that address */
            const int addr = owner->_symAddrs[(NSUInteger)line].intValue;
            [owner activateSymbol:(uint16_t)addr];
        }
    };
    NSTextField *hint = [self hint:@"Double-click a row to show its address in the Disassembly"];

    NSStackView *column = vstack(@[filterRow, scroll, hint]);
    fill(column, filterRow);
    fill(column, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

/* The packets left (a note and Clear over them), the cartridge's link and
 * counts right. */
- (NSView *)buildFujinet
{
    NSTextField *title = [self label:@"FujiBus packets (SLIP-decoded)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"The cartridge's link"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *both = hstack(@[left, right]);
    both.alignment = NSLayoutAttributeTop;
    both.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:both.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:both.heightAnchor].active = YES;
    return both;
}

/* The 6502's registers under every tab, each a field that takes a value (hex,
 * $hex, a label or an expression) while the machine is stopped; Enter applies
 * it (nesdebug_register_set). The flags are checkboxes. */
- (NSView *)buildRegStrip
{
    static NSString *const names[NUM_REGS] = { @"PC", @"SP", @"A", @"X", @"Y", @"P" };
    NSMutableArray *row = [NSMutableArray array];
    for (int idx = 0; idx < NUM_REGS; idx++) {
        [row addObject:[self label:names[idx]]];
        _reg[idx] = [self field:@"" width:idx == 0 ? 60 : 40 action:@selector(applyRegister:)];
        _reg[idx].tag = idx;
        _reg[idx].toolTip = @"Edit while the machine is stopped (hex, $hex, a label or an expression), then Enter";
        [row addObject:_reg[idx]];
    }
    static NSString *const flagNames[NUM_FLAGS] = { @"N", @"V", @"D", @"I", @"Z", @"C" };
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"Flags"]];
    for (int idx = 0; idx < NUM_FLAGS; idx++) {
        _flag[idx] = [NSButton checkboxWithTitle:flagNames[idx] target:self action:@selector(flagClicked:)];
        _flag[idx].tag = idx;
        [flags addObject:_flag[idx]];
    }
    _regInfo = [self hint:@""];
    _regInfo.font = monoFont();
    return vstack(@[hstack(row), hstack(flags), _regInfo]);
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 860)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *toCursor = [self button:@"Run to Cursor" action:@selector(runToCursor:)];
    NSButton *scan = [self button:@"Scanline+1" action:@selector(scanline:)];
    NSButton *frame = [self button:@"Frame+1 (⇧F7)" action:@selector(frame:)];
    NSPopUpButton *symbols = [self pullDown:@"Symbols" items:@[@"Load Symbols…", @"Clear Symbols"]
                                     action:@selector(symbolsMenu:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int entry = 0; entry < NUM_FILE_KINDS; entry++) [saveTitles addObject:kFileTitles[entry]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    NSButton *popOut = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popOut.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSButton *helpBtn = NESHelpButton(nil);
    [helpBtn setRefusesFirstResponder:YES];
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, toCursor, scan, frame, symbols, save, popOut, _status,
                                    helpBtn]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray<NSView *> *pages = @[
        [self buildConsole], [self buildDisasm], [self buildMemory], [self buildBreaks], [self buildWatches],
        [self buildTextTab:&_cartText], [self buildHist:0], [self buildHist:1], [self buildVideo],
        [self buildTextTab:&_soundText], [self buildSymbols], [self buildFujinet]
    ];
    for (int idx = 0; idx < TAB_COUNT; idx++) {   /* each keeps its place when moved around */
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@(kTabIds[idx])];
        item.label = @(kTabNames[idx]);
        item.view = [self pageHolding:pages[(NSUInteger)idx]];
        _items[idx] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("NES_DEBUGGER_TAB"));
    if (first >= 0) [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;
    [self updateHelpContext];

    /* The registers sit under every tab. */
    NSView *strip = [self buildRegStrip];

    NSStackView *root = vstack(@[toolbar, _tabs, strip]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.heightAnchor constraintGreaterThanOrEqualToConstant:620].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console) [self appendConsole:[text stringByAppendingString:@"\n"]];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = nesdebug_generation(_dbg);
    if (nesdebug_is_stopped(_dbg)) nesdebug_resume(_dbg); else nesdebug_stop(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)step:(id)sender { (void)sender; const unsigned genNow = nesdebug_generation(_dbg); nesdebug_step(_dbg); [self moved:genNow]; }
- (void)stepOver:(id)sender { (void)sender; const unsigned genNow = nesdebug_generation(_dbg); nesdebug_step_over(_dbg); [self moved:genNow]; }
- (void)stepOut:(id)sender { (void)sender; const unsigned genNow = nesdebug_generation(_dbg); nesdebug_step_out(_dbg); [self moved:genNow]; }
- (void)scanline:(id)sender { (void)sender; const unsigned genNow = nesdebug_generation(_dbg); nesdebug_scanline(_dbg, 1); [self moved:genNow]; }
- (void)frame:(id)sender { (void)sender; const unsigned genNow = nesdebug_generation(_dbg); nesdebug_frame(_dbg, 1); [self moved:genNow]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to" console:NO];
        return;
    }
    const unsigned genNow = nesdebug_generation(_dbg);
    nesdebug_run_to(_dbg, (uint16_t)_selected);
    [self moved:genNow];
}

- (void)toggleBreakpointAt:(uint16_t)addr
{
    nesdebug_breakpoint_toggle(_dbg, addr);
    [self refreshDisasm];
    if ([self tabVisible:MT_BREAKS]) [self refreshBps];
}

/* F9 */
- (void)toggleSelected:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to toggle its breakpoint" console:NO];
        return;
    }
    [self toggleBreakpointAt:(uint16_t)_selected];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    nesdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)selectTab:(int)tab
{
    if (_popped[tab]) [_popped[tab] makeKeyAndOrderFront:nil];
    else [_tabs selectTabViewItem:_items[tab]];
}

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_conIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", cmd]];
    out[0] = '\0';
    const unsigned gen = nesdebug_generation(_dbg);
    nesdebug_command(_dbg, cmd.UTF8String, out, sizeof out);
    NSString *text = cstr(out);
    if (text.length) [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger total = (NSInteger)_history.count;
        if (total == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > total) _historyPos = total;
        _conIn.stringValue = _historyPos < total ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    /* the engine completes the last word */
    NSString *text = _conIn.stringValue;
    const NSRange space = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = space.location == NSNotFound ? text : [text substringFromIndex:space.location + 1];
    char comps[4096];
    comps[0] = '\0';
    const int matches = nesdebug_completions(_dbg, word.UTF8String, comps, sizeof comps);
    NSString *list = cstr(comps);
    if (matches == 1) {
        NSString *first = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, first];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (matches > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

/* The Symbols pull-down: Load Symbols… or Clear Symbols. */
- (void)symbolsMenu:(NSPopUpButton *)sender
{
    if (sender.selectedItem.tag == 1) {
        nesdebug_clear_symbols(_dbg);
        [self message:@"Symbols cleared (the register names stay)" console:YES];
        [self refreshNow];
        return;
    }
    char msg[512];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load Symbols (ld65 -Ln / VICE, ca65 .dbg, Mesen .mlb)";
    if ([panel runModal] != NSModalResponseOK) return;
    msg[0] = '\0';
    nesdebug_load_symbols(_dbg, [[panel URL] fileSystemRepresentation], msg, sizeof msg);
    [self message:cstr(msg) console:YES];
    [self refreshNow];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [panel setNameFieldStringValue:kFileNames[kind]];
    if ([panel runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    nesdebug_save(_dbg, kFileKinds[kind], [[panel URL] fileSystemRepresentation], msg, sizeof msg);
    [self message:cstr(msg) console:YES];
}

/* A register field's text applied through nesdebug_register_set. */
- (void)applyRegister:(NSTextField *)sender
{
    long value = 0;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!nesdebug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to edit registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REGS) {
        /* not one of ours */
    } else if (![self evaluate:sender.stringValue into:&value why:&why]) {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    } else {
        /* the engine's own register names, in the strip's order */
        static const char *const regNames[NUM_REGS] = { "pc", "sp", "a", "x", "y", "ps" };
        char err[160];
        err[0] = '\0';
        if (!nesdebug_register_set(_dbg, regNames[sender.tag], value, err, (int)sizeof err))
            [self message:[NSString stringWithFormat:@"Register: %@", err[0] ? cstr(err) : @"refused"] console:NO];
    }
    [self refreshNow];
}

- (void)flagClicked:(NSButton *)sender
{
    static const char *const flagNames[NUM_FLAGS] = { "n", "v", "d", "i", "z", "c" };
    char err[160];
    err[0] = '\0';
    if (sender.tag < 0 || sender.tag >= NUM_FLAGS)
        return;
    if (nesdebug_is_stopped(_dbg))
        nesdebug_register_set(_dbg, flagNames[sender.tag], sender.state == NSControlStateValueOn, err, (int)sizeof err);
    else
        [self message:@"Stop the machine (F5) to edit flags" console:NO];
    [self refreshRegs];
}

/* The space the Memory tab shows now. */
- (int)memSpaceNow
{
    const NSInteger sel = _memSpace.indexOfSelectedItem;
    return kMemSpaces[sel >= 0 && sel < MEM_SPACES ? sel : 0];
}

/* Shows a range of a space in the Memory tab, highlighted, a couple of rows
 * down from the top. */
- (void)showInMemorySpace:(int)space from:(uint32_t)first last:(uint32_t)last
{
    for (int idx = 0; idx < MEM_SPACES; idx++)
        if (kMemSpaces[idx] == space) [_memSpace selectItemAtIndex:idx];
    _memBase = (int64_t)(first & ~(uint32_t)0xF) - 2 * 16;
    _memHl = YES;
    _memHlLo = first;
    _memHlHi = last;
    [self refreshMem];
}

- (void)memSpaceChanged:(id)sender
{
    (void)sender;
    _memBase = 0;
    _memHl = NO;
    _srchNote.stringValue = @"";
    _memNote.stringValue = @"";
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    const int space = [self memSpaceNow];
    uint16_t first16, last16;
    if (space == NESDEBUG_SPACE_CPU) {
        if (!nessym_parse_range(_dbg, _memGoto.stringValue.UTF8String, &first16, &last16)) {
            [self message:[NSString stringWithFormat:@"Not an address or label: %@", _memGoto.stringValue] console:NO];
            return;
        }
        [self showInMemorySpace:space from:first16 last:last16];
        return;
    }
    const int64_t addr = [self resolveIn:space text:_memGoto.stringValue];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Not an address: %@", _memGoto.stringValue] console:NO];
        return;
    }
    [self showInMemorySpace:space from:(uint32_t)addr last:(uint32_t)addr];
}

- (void)memPrev:(id)sender { (void)sender; _memBase -= MEM_BYTES; [self refreshMem]; }
- (void)memNext:(id)sender { (void)sender; _memBase += MEM_BYTES; [self refreshMem]; }

/* "address value [value...]": the values land at consecutive addresses. */
- (void)writeMem:(id)sender
{
    (void)sender;
    const int space = [self memSpaceNow];
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@" \t,"]];
    NSMutableArray<NSString *> *tok = [NSMutableArray array];
    for (NSString *piece in words) if (piece.length) [tok addObject:piece];
    const int64_t address = tok.count > 0 ? [self resolveIn:space text:tok[0]] : -1;
    if (tok.count < 2 || address < 0) {
        _memNote.stringValue = @"Poke: address value [value...]";
        return;
    }
    if (!nesdebug_is_stopped(_dbg)) {
        _memNote.stringValue = @"Stop the machine (F5) to poke it";
        return;
    }
    const int size = nesdebug_space_size(_dbg, space);
    if (address + (int64_t)tok.count - 2 >= size) {
        _memNote.stringValue = [NSString stringWithFormat:@"Past the end of the %s space ($%X bytes)",
                                nesdebug_space_name(space), size];
        return;
    }
    for (NSUInteger idx = 1; idx < tok.count; idx++) {
        long value;
        if (!parseNum(tok[idx], &value) || value < 0 || value > 0xFF) {
            _memNote.stringValue = [NSString stringWithFormat:@"Not a byte: %@", tok[idx]];
            return;
        }
        const uint8_t byte = (uint8_t)value;
        nesdebug_write_space(_dbg, space, (uint32_t)(address + (int64_t)idx - 1), &byte, 1);
    }
    _memWrite.stringValue = @"";
    _memNote.stringValue = [NSString stringWithFormat:@"Wrote %d byte%s at $%llX (RAM takes it; ROM ignores it)",
                            (int)tok.count - 1, tok.count == 2 ? "" : "s", (unsigned long long)address];
    if (address < _memBase || address >= _memBase + MEM_BYTES)
        [self showInMemorySpace:space from:(uint32_t)address last:(uint32_t)address];
    else
        [self refreshMem];
}

- (void)searchBackward:(BOOL)backward
{
    const int space = [self memSpaceNow];
    uint8_t pat[NESSEARCH_MAX], mask = 0xFF;
    int count = 0, flags = 0;
    uint32_t found;
    if (!nessearch_parse((int)_srchKind.indexOfSelectedItem, _srchEntry.stringValue.UTF8String, pat, &count, &mask,
                         &flags) || count < 1) {
        _srchNote.stringValue = @"Type hex bytes (A9 00 8D) or some text.";
        return;
    }
    if (!nesdebug_is_stopped(_dbg)) {
        _srchNote.stringValue = @"Stop the machine (F5) to search its memory.";
        return;
    }
    const uint32_t from = _memHl ? (backward ? _memHlLo : _memHlLo + 1) : (uint32_t)(_memBase < 0 ? 0 : _memBase);
    const int hit = nesdebug_search_next_in(_dbg, space, pat, count, mask, flags, from, backward ? 1 : 0, &found);
    if (hit > 0) {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at $%X", found];
        [self showInMemorySpace:space from:found last:found + (uint32_t)count - 1];
    } else {
        _srchNote.stringValue = hit == 0 ? @"Not found." : @"Stop the machine (F5) to search its memory.";
    }
}

- (void)searchNext:(id)sender { (void)sender; [self searchBackward:NO]; }
- (void)searchPrev:(id)sender { (void)sender; [self searchBackward:YES]; }

- (void)followToggled:(id)sender
{
    (void)sender;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Not an address or label: %@", _jump.stringValue] console:NO];
        return;
    }
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    _selected = addr;
    [self refreshDisasm];
}

/* A Symbols row activated: the Disassembly shows that address. */
- (void)activateSymbol:(uint16_t)addr
{
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = addr;
    _selected = addr;
    [self selectTab:MT_DISASM];
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[4] = { NESDEBUG_BP_EXEC, NESDEBUG_BP_READ, NESDEBUG_BP_WRITE, NESDEBUG_BP_ACCESS };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 4 ? sel : 0];
    const NSInteger spaceSel = _bpSpace.indexOfSelectedItem;
    const int space = kBpSpaces[spaceSel >= 0 && spaceSel < BP_SPACES ? spaceSel : 0];
    NSString *startText = [_bpStart.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *endText = [_bpEnd.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (startText.length == 0) {
        _bpErr.stringValue = @"Give an address";
        return;
    }
    const int64_t start = [self resolveIn:space text:startText];
    const int64_t end = endText.length ? [self resolveIn:space text:endText] : start;
    if (start < 0 || end < 0 || end < start) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad address range: %@%@%@", startText,
                              endText.length ? @" - " : @"", endText];
        return;
    }
    char err[256];
    err[0] = '\0';
    const int bpId = nesdebug_breakpoint_add_in(_dbg, space, type, (uint32_t)start, (uint32_t)end,
                                                _bpCond.stringValue.UTF8String, err, sizeof err);
    if (bpId < 0) {
        NSString *reason = cstr(err);
        _bpErr.stringValue = reason.length ? reason : @"The breakpoint was refused";
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    nesdebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshNow];
}

/* Replaces the entry's condition with the Condition field's text. */
- (void)bpSetCondition:(NSButton *)sender
{
    char err[256];
    err[0] = '\0';
    if (!nesdebug_breakpoint_set_condition(_dbg, (int)sender.tag, _bpCond.stringValue.UTF8String, err, (int)sizeof err)) {
        NSString *reason = cstr(err);
        _bpErr.stringValue = reason.length ? reason : @"The condition was refused";
        return;
    }
    _bpErr.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    nesdebug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshNow];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    nesdebug_breakpoint_clear(_dbg);
    [self refreshNow];
}

- (void)clearHits:(id)sender
{
    (void)sender;
    nesdebug_breakpoint_clear_hits(_dbg);
    [self refreshNow];
}

- (void)addWatch:(id)sender
{
    (void)sender;
    if (neswatch_add(_wtEntry.stringValue.UTF8String) < 0) {
        [self message:@"Type an address, label, expression or lo-hi range to watch" console:NO];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
}

- (void)wtMemory:(id)sender
{
    (void)sender;
    uint16_t first, last;
    if (_wtList.picked >= 0 && neswatch_range(_dbg, _wtList.picked, &first, &last)) {
        [self selectTab:MT_MEMORY];
        [self showInMemorySpace:NESDEBUG_SPACE_CPU from:first last:last];
    }
}

- (void)wtBreak:(id)sender
{
    (void)sender;
    uint16_t first, last;
    if (_wtList.picked < 0 || !neswatch_range(_dbg, _wtList.picked, &first, &last)) return;
    [self breakOnChangeFrom:first to:last];
}

/* A write breakpoint on first..last that fires when the value written
 * differs from the old one (the core's `value != old`). */
- (void)breakOnChangeFrom:(uint16_t)first to:(uint16_t)last
{
    char err[160];
    err[0] = '\0';
    if (nesdebug_breakpoint_add_in(_dbg, NESDEBUG_SPACE_CPU, NESDEBUG_BP_WRITE, first, last, "value != old", err,
                                   (int)sizeof err) >= 0)
        [self message:@"Breaking when it changes" console:NO];
    else
        [self message:err[0] ? cstr(err) : @"The breakpoint was refused" console:NO];
    [self refreshNow];
}

- (void)wtRemove:(id)sender
{
    (void)sender;
    if (_wtList.picked >= 0) {
        neswatch_remove(_wtList.picked);
        _wtList.picked = -1;
        [self refreshWatches];
    }
}

- (void)histRec:(NSButton *)sender
{
    const int enabled = sender.state == NSControlStateValueOn;
    if (sender.tag == 0) nesdebug_trace_enable(_dbg, enabled); else nesdebug_history_enable(_dbg, enabled);
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self refreshNow];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self refreshHist:0 force:YES];
    [self refreshHist:1 force:YES];
}

- (void)histClear:(id)sender
{
    (void)sender;
    nesdebug_trace_clear(_dbg);
    nesdebug_history_clear(_dbg);
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self refreshNow];
}

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    _symList.picked = -1;
    [self refreshSymbols];
}

/* ---- tabs in windows of their own --------------------------------------------------- */

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (_tabs.selectedTabViewItem == _items[slot]) {
            [self popOutTab:slot];
            return;
        }
}

- (void)popOutTab:(int)tab
{
    if (_popped[tab] || _tabs.numberOfTabViewItems <= 1) return;     /* one tab always stays */
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [NSString stringWithFormat:@"Debugger – %s", kTabNames[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSStackView *root = vstack(@[dockBtn, view]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [view.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [view.heightAnchor constraintGreaterThanOrEqualToConstant:300].active = YES;
    win.contentView = root;
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self refreshNow];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

- (void)dockTab:(int)tab
{
    NSWindow *win = _popped[tab];
    if (!win) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int pos = 0;
    _popped[tab] = nil;
    [view removeFromSuperview];
    item.view = view;
    for (int slot = 0; slot < tab; slot++)   /* back where it was in the order */
        if (!_popped[slot]) pos++;
    [_tabs insertTabViewItem:item atIndex:pos];
    [_tabs selectTabViewItem:item];
    win.delegate = nil;
    [win close];
    [self refreshNow];
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status, the register strip
 * and the visible tabs. The poll compares against what was seen here. */
- (void)refreshNow
{
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(_dbg, &snap);
    _seenSerial = snap.serial;
    _seenGen = nesdebug_generation(_dbg);
    _wasStopped = snap.paused != 0;
    [self refreshStatus];
    [self refreshVisibleForce:YES];
}

/* Only the tabs on show: the others refresh when they are selected. A running
 * machine is not read (see onTick) unless the user asked for it. */
- (void)refreshVisibleForce:(BOOL)force
{
    [self refreshRegs];
    const BOOL liveRead = !force && !nesdebug_is_stopped(_dbg);
    for (int slot = 0; slot < TAB_COUNT; slot++) {
        if (![self tabVisible:slot]) continue;
        if (liveRead && slot != MT_VIDEO && slot != MT_TRACE && slot != MT_FUJINET) continue;
        switch (slot) {
        case MT_DISASM: [self refreshDisasm]; break;
        case MT_MEMORY: [self refreshMem]; break;
        case MT_BREAKS: [self refreshBps]; break;
        case MT_WATCHES: [self refreshWatches]; break;
        case MT_CART: [self refreshCart]; break;
        case MT_TRACE: [self refreshHist:0 force:force]; break;
        case MT_HISTORY: [self refreshHist:1 force:force]; break;
        case MT_VIDEO: [_video refreshForce:force]; break;
        case MT_SOUND: [self refreshSound]; break;
        case MT_SYMBOLS: [self refreshSymbols]; break;
        case MT_FUJINET: [self refreshFujinet:NO]; break;
        default: break;
        }
    }
}

- (void)refreshStatus
{
    nesdebug_snapshot snap;
    nesdebug_stop_snapshot(_dbg, &snap);
    const BOOL stopped = snap.paused != 0;
    NSString *why = stopText(&snap);
    if (why.length)
        why = [[[why substringToIndex:1] uppercaseString] stringByAppendingString:[why substringFromIndex:1]];
    _status.stringValue = stopped ? (why.length ? why : @"Stopped") : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? accentColor() : nil;
    _runBtn.contentTintColor = stopped ? accentInk() : nil;
}

- (void)refreshRegs
{
    static int previous[NUM_REGS];
    static BOOL havePrevious;
    if (!nesdebug_is_stopped(_dbg)) {
        _regInfo.stringValue = @"  running";
        return;
    }
    nesdebug_cpu cpu;
    nesdebug_cpu_get(_dbg, &cpu);
    const int vals[NUM_REGS] = { cpu.pc, cpu.sp, cpu.a, cpu.x, cpu.y, cpu.ps };
    for (int idx = 0; idx < NUM_REGS; idx++) {
        const BOOL changed = havePrevious && previous[idx] != vals[idx];
        previous[idx] = vals[idx];
        _reg[idx].textColor = changed ? accentLight() : NSColor.textColor;     /* moved since the last look */
        if (_reg[idx].currentEditor) continue;        /* never over a field being edited */
        _reg[idx].stringValue = [NSString stringWithFormat:(idx == 0 ? @"%04X" : @"%02X"), vals[idx]];
    }
    havePrevious = YES;
    const int flags[NUM_FLAGS] = { cpu.n, cpu.v, cpu.d, cpu.i, cpu.z, cpu.c };
    for (int idx = 0; idx < NUM_FLAGS; idx++) _flag[idx].state = flags[idx] ? NSControlStateValueOn : NSControlStateValueOff;
    _regInfo.stringValue = [NSString stringWithFormat:@"  cycle %llu   scanline %d   dot %d   frame %u%s%s   "
                                                      @"(edit a register and press Enter)",
                            (unsigned long long)cpu.total_cycles, cpu.scanline, cpu.dot, cpu.frame,
                            cpu.irq ? "   IRQ" : "", cpu.nmi ? "   NMI" : ""];
}

/* A note for a row of a space's dump: what the bytes there are. */
static NSString *spaceNote(int space, int64_t addr)
{
    switch (space) {
    case NESDEBUG_SPACE_PPU:
        if (addr < 0x2000) return [NSString stringWithFormat:@"pattern table %d", (int)(addr >> 12)];
        if (addr < 0x3000) return [NSString stringWithFormat:@"nametable %d", (int)((addr - 0x2000) >> 10)];
        return @"mirror of $2000-$2EFF";
    case NESDEBUG_SPACE_OAM:
        return [NSString stringWithFormat:@"sprites %d-%d", (int)(addr / 4), (int)(addr / 4) + 3];
    case NESDEBUG_SPACE_PALETTE:
        return addr < 0x10 ? @"background palettes" : @"sprite palettes";
    default:
        return @"";
    }
}

- (void)refreshMem
{
    const int space = [self memSpaceNow];
    const int size = nesdebug_space_size(_dbg, space);
    uint8_t data[MEM_BYTES];
    nesdebug_region map[MAP_MAX];
    int64_t last = (int64_t)size - MEM_BYTES;
    if (last < 0) last = 0;
    if (_memBase > last) _memBase = last & ~(int64_t)0xF;
    if (_memBase < 0) _memBase = 0;
    memset(data, 0, sizeof data);
    int have = size > 0 ? nesdebug_read_space(_dbg, space, (uint32_t)_memBase, data, MEM_BYTES) : 0;
    if (have < 0) have = 0;
    int rows = (have + 15) / 16;
    if (rows > MEM_ROWS) rows = MEM_ROWS;
    int nmap = space == NESDEBUG_SPACE_CPU ? nesdebug_memory_map(_dbg, map, MAP_MAX) : 0;
    if (nmap < 0) nmap = 0;

    /* the watched bytes, in the CPU's space */
    uint16_t watchLo[NESWATCH_MAX], watchHi[NESWATCH_MAX];
    int nwatch = 0;
    if (space == NESDEBUG_SPACE_CPU)
        for (int idx = 0; idx < neswatch_count() && nwatch < NESWATCH_MAX; idx++)
            if (neswatch_range(_dbg, idx, &watchLo[nwatch], &watchHi[nwatch])) nwatch++;

    NSString *addrFormat = size > 0x10000 ? @"%06llX  " : @"%04llX  ";
    NSMutableString *text = [NSMutableString stringWithString:
        size > 0x10000 ? @"        0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n"
                       : @"      0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n"];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array], *watched = [NSMutableArray array];
    char region[64], lastRegion[64];
    lastRegion[0] = '\0';
    for (int row = 0; row < rows; row++) {
        const int64_t base = _memBase + (int64_t)row * 16;
        [text appendFormat:addrFormat, (unsigned long long)base];
        for (int col = 0; col < 16; col++) {
            const int64_t addr = base + col;
            if (col == 8) [text appendString:@" "];
            if (row * 16 + col >= have) {
                [text appendString:@"   "];
                continue;
            }
            if (_memHl && addr >= _memHlLo && addr <= _memHlHi)
                [marks addObject:[NSValue valueWithRange:NSMakeRange(text.length, 2)]];
            else
                for (int idx = 0; idx < nwatch; idx++)
                    if (addr >= watchLo[idx] && addr <= watchHi[idx]) {
                        [watched addObject:[NSValue valueWithRange:NSMakeRange(text.length, 2)]];
                        break;
                    }
            [text appendFormat:@"%02X ", data[row * 16 + col]];
        }
        [text appendString:@" "];
        for (int col = 0; col < 16 && row * 16 + col < have; col++) {
            const uint8_t chr = data[row * 16 + col];
            [text appendFormat:@"%c", (chr >= 0x20 && chr < 0x7F) ? (char)chr : '.'];
        }
        /* label a row where the region it is in changes */
        region[0] = '\0';
        if (space == NESDEBUG_SPACE_CPU)
            nesmem_region_name(map, nmap, (uint16_t)base, region, (int)sizeof region);
        else
            snprintf(region, sizeof region, "%s", spaceNote(space, base).UTF8String);
        if (region[0] && strcmp(region, lastRegion) != 0) [text appendFormat:@"  ; %s", region];
        snprintf(lastRegion, sizeof lastRegion, "%s", region);
        [text appendString:@"\n"];
    }
    if (rows == 0) [text appendFormat:@"(the %s space is empty or the debugger is not attached)\n", nesdebug_space_name(space)];
    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (NSValue *span in watched) [styled addAttributes:watchAttrs() range:span.rangeValue];
    for (NSValue *span in marks) [styled addAttributes:accentAttrs() range:span.rangeValue];
    setStyledText(_mem, styled);
}

- (void)refreshDisasm
{
    static nesdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        nesdebug_cpu cpu;
        nesdebug_cpu_get(_dbg, &cpu);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)nesdebug_row_address(_dbg, (uint16_t)cpu.pc, -(DISASM_WINDOW / 3));
    }
    int lineTotal = nesdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    if (lineTotal < 0) lineTotal = 0;
    if (lineTotal > DISASM_WINDOW) lineTotal = DISASM_WINDOW;
    _lineCount = lineTotal;

    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW];
    for (int rowNo = 0; rowNo < lineTotal; rowNo++) {
        _lineAddr[rowNo] = lines[rowNo].address;
        const char gutter = !lines[rowNo].has_breakpoint ? ' ' : (lines[rowNo].breakpoint_enabled ? '*' : 'o');
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-9s  %-16s %-24s%s%s", gutter,
                          lines[rowNo].is_pc ? '>' : ' ', lines[rowNo].address, lines[rowNo].bytes, lines[rowNo].label,
                          lines[rowNo].disasm, lines[rowNo].comment[0] ? " ; " : "", lines[rowNo].comment];
        ranges[rowNo] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (lineTotal == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];

    NSMutableAttributedString *styled = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int rowNo = 0; rowNo < lineTotal; rowNo++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[rowNo].has_breakpoint)
            [styled addAttribute:NSForegroundColorAttributeName
                           value:lines[rowNo].breakpoint_enabled ? NSColor.systemRedColor : NSColor.systemGrayColor
                           range:NSMakeRange(ranges[rowNo].location, 1)];
        /* the selected line underlined on a selection fill */
        if ((int)lines[rowNo].address == _selected)
            [styled addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
                                     NSBackgroundColorAttributeName: NSColor.selectedTextBackgroundColor }
                            range:ranges[rowNo]];
    }
    /* the PC's line in the accent colour, last so its colours win */
    for (int rowNo = 0; rowNo < lineTotal; rowNo++) {
        if (!lines[rowNo].is_pc) continue;
        [styled addAttributes:accentAttrs() range:ranges[rowNo]];
        if ((int)lines[rowNo].address == _selected)
            [styled addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[rowNo]];
    }
    [_disasm.textStorage setAttributedString:styled];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

/* The kind of a breakpoint entry, in words. */
static NSString *bpTypeText(int type)
{
    if (type == NESDEBUG_BP_ACCESS) return @"access";
    NSMutableString *out = [NSMutableString string];
    static const struct { int bit; const char *name; } kinds[3] = {
        { NESDEBUG_BP_EXEC, "exec" }, { NESDEBUG_BP_READ, "read" }, { NESDEBUG_BP_WRITE, "write" },
    };
    for (int item = 0; item < 3; item++)
        if (type & kinds[item].bit) {
            if (out.length) [out appendString:@"/"];
            [out appendFormat:@"%s", kinds[item].name];
        }
    return out;
}

- (void)refreshBps
{
    static nesdebug_breakpoint bps[MAX_BPS];
    int count = nesdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (count < 0) count = 0;
    if (count > MAX_BPS) count = MAX_BPS;

    /* why the machine last stopped, worded by the core */
    {
        nesdebug_snapshot snap;
        nesdebug_stop_snapshot(_dbg, &snap);
        NSString *why = stopText(&snap);
        _bpStop.stringValue = !snap.paused ? @"Running." : why.length ? [@"Stopped: " stringByAppendingString:why] : @"Stopped.";
    }

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *sig = [NSMutableString string];
    for (int idx = 0; idx < count; idx++)
        [sig appendFormat:@"%d %d %d %d %d %d %u %s\n", bps[idx].id, bps[idx].type, bps[idx].space, bps[idx].start,
                          bps[idx].end, bps[idx].enabled, bps[idx].hits, bps[idx].condition];
    if ([sig isEqualToString:_bpSig]) return;
    _bpSig = sig;

    for (NSView *rowView in [_bpList.arrangedSubviews copy]) [_bpList removeView:rowView];
    if (count == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int idx = 0; idx < count; idx++) {
        const BOOL cpu = bps[idx].space == NESDEBUG_SPACE_CPU;
        const BOOL wide = bps[idx].space == NESDEBUG_SPACE_CPU || bps[idx].space == NESDEBUG_SPACE_PPU;
        NSMutableString *range = [NSMutableString stringWithFormat:(wide ? @"$%04X" : @"$%02X"), bps[idx].start];
        if (bps[idx].end != bps[idx].start) [range appendFormat:(wide ? @"-$%04X" : @"-$%02X"), bps[idx].end];
        char label[64];
        label[0] = '\0';
        if (cpu && nesdebug_address_label(_dbg, bps[idx].start, label, sizeof label) > 0 && label[0])
            [range appendFormat:@" %s", label];

        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[idx].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[idx].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[idx].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[idx].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *condition = [self button:@"Set Cond." action:@selector(bpSetCondition:)];
        condition.tag = bps[idx].id;
        condition.toolTip = @"Replace this breakpoint's condition with the text in the Condition field (empty removes it)";
        [condition.widthAnchor constraintEqualToConstant:80].active = YES;
        NSStackView *row = hstack(@[box, bpCell(bpTypeText(bps[idx].type), 60, NO),
                                    bpCell(@(nesdebug_space_name(bps[idx].space)), 60, NO), bpCell(range, 170, NO),
                                    bpCell(cstr(bps[idx].condition), 0, NO),
                                    bpCell([NSString stringWithFormat:@"%u", bps[idx].hits], 60, NO), condition, remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int count = neswatch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (int idx = 0; idx < count; idx++) {
        char line[256];
        line[0] = '\0';
        neswatch_format(_dbg, idx, line, (int)sizeof line);
        [lines addObject:cstr(line)];
    }
    if (count == 0) [lines addObject:@"No watches. Type an address, label, expression or lo-hi range above."];
    if (_wtList.picked >= count) _wtList.picked = -1;
    setPickText(_wtList, lines);
}

/* The cartridge (mapper, banks, the FujiNet mailbox), the controllers, the
 * expansion-port keyboard and recorder, the CPU lines and the PPU's state
 * (the core's io text), the cartridge's link, and the CPU memory map: what
 * the machine answers where. */
- (void)refreshCart
{
    static char text[8192];
    static nesdebug_cart cart;
    nesdebug_region map[MAP_MAX];
    NSMutableString *out = [NSMutableString string];

    text[0] = '\0';
    nesdebug_io_text(_dbg, text, (int)sizeof text);
    [out appendFormat:@"%@\n", cstr(text)];

    nesdebug_cart_get(_dbg, &cart);
    if (cart.present) {
        [out appendString:@"\nFujiNet cartridge\n"];
        [out appendFormat:@"  Link   %s%s%s%s\n", cart.link_up ? "up" : "down", cart.busy ? " (busy)" : "",
                          (!cart.link_up && cart.link_error[0]) ? ": " : "",
                          (!cart.link_up && cart.link_error[0]) ? cart.link_error : ""];
        [out appendFormat:@"  Image  %s%@\n", cart.booted_image ? "a game" : "CONFIG",
                          cart.loading ? [NSString stringWithFormat:@" (loading %d%%)", cart.load_pct] : @""];
        [out appendFormat:@"  SRAM   %s\n", cart.sram_enabled ? "enabled" : "off (serving the loader's vectors)"];
    }

    [out appendString:@"\nMemory map\n"];
    const int count = nesdebug_memory_map(_dbg, map, MAP_MAX);
    if (count <= 0) [out appendString:@"  No machine is running.\n"];
    for (int idx = 0; idx < count; idx++)
        [out appendFormat:@"  %04X-%04X  %c%c  %s\n", map[idx].low, map[idx].high, map[idx].readable ? 'r' : '-',
                          map[idx].writable ? 'w' : '-', map[idx].name];
    setText(_cartText, out, NO);
}

/* Trace (live) and History (when stopped), from the one ring. */
- (void)refreshHist:(int)idx force:(BOOL)force
{
    static nesdebug_trace_entry buf[HIST_BUF];
    const BOOL live = idx == 0;
    const BOOL stopped = nesdebug_is_stopped(_dbg) != 0;
    const BOOL traceOn = nesdebug_trace_enabled(_dbg) != 0, histOn = nesdebug_history_enabled(_dbg) != 0;
    const BOOL recording = live ? traceOn : histOn;
    const uint64_t total = live ? nesdebug_trace_total(_dbg) : nesdebug_history_count(_dbg);
    uint16_t first = 0, last = 0xFFFF;
    int filter = NESDEBUG_TRACE_ALL;

    for (int pair = 0; pair < 2; pair++) {      /* the Record buttons follow the engine */
        const BOOL pairOn = pair == 0 ? traceOn : histOn;
        _hRec[pair].state = pairOn ? NSControlStateValueOn : NSControlStateValueOff;
        _hRec[pair].bezelColor = pairOn ? accentColor() : nil;
        _hRec[pair].contentTintColor = pairOn ? accentInk() : nil;
    }
    if (!force && total == _hSeen[idx] && (live || stopped)) return;
    _hSeen[idx] = total;
    if (!live && !stopped) {
        _hNote[idx].stringValue = @"Stop the machine (F5) to read its history.";
        return;
    }
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1: filter = NESDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (nessym_parse_range(_dbg, _hRange[idx].stringValue.UTF8String, &first, &last)) filter = NESDEBUG_TRACE_RANGE;
        break;
    }
    /* the newest matches, scanning back from the newest; shown oldest first */
    int hit[HIST_SHOWN], shown = 0, count;
    if (live) {
        count = nesdebug_trace_read(_dbg, buf, HIST_BUF);                  /* newest first */
        for (int rec = 0; rec < count && shown < HIST_SHOWN; rec++)
            if (nesdebug_trace_match(&buf[rec], filter, first, last)) hit[shown++] = rec;
    } else {
        count = nesdebug_history_copy(_dbg, buf, HIST_BUF);                /* oldest first */
        for (int rec = count - 1; rec >= 0 && shown < HIST_SHOWN; rec--)
            if (nesdebug_history_match(&buf[rec], filter, first, last)) hit[shown++] = rec;
    }
    NSMutableString *slot = [NSMutableString string];
    if (!recording && count == 0) [slot appendString:@"Recording is off. Press Record, then let the machine run."];
    for (int pair = shown - 1; pair >= 0; pair--) {
        char line[256];
        line[0] = '\0';
        if (live) nesdebug_trace_format(&buf[hit[pair]], line, (int)sizeof line);
        else nesdebug_history_format(&buf[hit[pair]], line, (int)sizeof line);
        [slot appendFormat:@"%@\n", cstr(line)];
    }
    setText(_hText[idx], slot, YES);
    _hNote[idx].stringValue = [NSString stringWithFormat:@"%@%llu instructions recorded, last %d shown",
                                                          recording ? @"Recording. " : @"", (unsigned long long)total, shown];
}

/* The 2A03's five channels and $4015, as the core words them. */
- (void)refreshSound
{
    static char text[4096];
    text[0] = '\0';
    nesdebug_sound_text(_dbg, text, (int)sizeof text);
    setText(_soundText, cstr(text), NO);
}

/* The list is rebuilt only when the symbol count or the filter changed. */
- (void)refreshSymbols
{
    NSString *filter = _symFilter.stringValue ?: @"";
    const int total = nesdebug_symbols_count(_dbg);
    NSString *sig = [NSString stringWithFormat:@"%d|%u|%d|%@", total, nesdebug_symbols_generation(_dbg), _symList.picked,
                                               filter];
    if ([sig isEqualToString:_symSig]) return;
    _symSig = sig;

    int *index = total > 0 ? malloc((size_t)total * sizeof *index) : NULL;
    const int shown = index ? nesdebug_symbols_filter(_dbg, filter.UTF8String, index, total) : 0;
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    [_symAddrs removeAllObjects];
    for (int idx = 0; idx < shown; idx++) {
        uint16_t addr;
        char name[96];
        name[0] = '\0';
        if (!nesdebug_symbols_get(_dbg, index[idx], &addr, name, (int)sizeof name)) continue;
        [lines addObject:[NSString stringWithFormat:@"%04X  %s", addr, name]];
        [_symAddrs addObject:@(addr)];
    }
    free(index);
    if (lines.count == 0) [lines addObject:@"(no symbols match)"];
    if (_symList.picked >= (int)_symAddrs.count) _symList.picked = -1;
    setPickText(_symList, lines);
    _symCount.stringValue = [NSString stringWithFormat:@"%d symbol%s%s", (int)_symAddrs.count, _symAddrs.count == 1 ? "" : "s",
                                                       filter.length > 0 ? " (filtered)" : ""];
}

/* The note and the link's state always; the log only when it changed, or a
 * repeat folded into the newest. Built as UTF-8 first: %s in an NSString
 * format is not UTF-8. */
- (void)refreshFujinet:(BOOL)force
{
    static nesdebug_fuji_packet packets[TRACE_MAX];
    static char logText[TRACE_MAX * 336 + 128]; /* an entry is at most ~330 */
    static char state[2048];
    int total = nesdebug_fujinet_trace(_dbg, packets, TRACE_MAX);
    size_t len = 0;
    if (total < 0) total = 0;

    if (!nessession_is_running(_session))
        _fujiNote.stringValue = @"The NES is off.";
    else if (nessession_fujinet_external(_session))
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet at %s; the cartridge is %s.",
                                 nessession_boip_endpoint(_session),
                                 nessession_cart_link_up(_session) == 1 ? "connected" : "not connected"];
    else if (!nessession_fujinet_running(_session))
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ Enable FujiNet).";
    else
        _fujiNote.stringValue = nessession_cart_link_up(_session) == 1
                                    ? @"FujiNet is on; the cartridge is connected."
                                    : @"FujiNet is on; the cartridge is not connected.";
    state[0] = '\0';
    nesdebug_fujinet_state(_dbg, state, (int)sizeof(state));
    setText(_fujiState, cstr(state), NO);

    if (!force && total == _fujiCount && (total == 0 || packets[total - 1].serial == _fujiLast))
        return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = total;
    _fujiLast = total > 0 ? packets[total - 1].serial : 0;
    logText[0] = '\0';
    for (int entry = 0; entry < total; entry++) {
        len += (size_t)snprintf(logText + len, sizeof(logText) - len, "%s%s", entry ? "\n" : "", packets[entry].text);
        if (packets[entry].repeat > 1)
            len += (size_t)snprintf(logText + len, sizeof(logText) - len, " \xc3\x97%u", packets[entry].repeat);
        len += (size_t)snprintf(logText + len, sizeof(logText) - len, "\n     ");
        for (int bytePos = 0; bytePos < packets[entry].nbytes && bytePos < (int)sizeof(packets[entry].data); bytePos++)
            len += (size_t)snprintf(logText + len, sizeof(logText) - len, "%02X ", packets[entry].data[bytePos]);
        if (packets[entry].nbytes > (int)sizeof(packets[entry].data))
            len += (size_t)snprintf(logText + len, sizeof(logText) - len, "\xe2\x80\xa6");
    }
    if (total == 0)
        snprintf(logText, sizeof(logText), "(no FujiBus packets since the trace was cleared)");
    setText(_fujiText, cstr(logText), follow);
}

@end
