/*
 * Debugger window (AppKit) over MesenCE's own debugger engine, via
 * core/include/nesdebug.h. Mirrors the GTK, Qt and Win32 debuggers tab
 * for tab: Prompt, CPU & RAM, Disassembly, PPU, APU & Input, Breakpoints,
 * Cart.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it detaches, and the machine runs on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../KeyForward.h"

#include <stdlib.h>
#include <string.h>

#include "nesdebug.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64

static NESDebuggerWindow *g_debugger;

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line; the wheel browses. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onToggleLine)(int line);
@property (nonatomic, copy) void (^onScroll)(int lines);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < idx && i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if (self.onToggleLine) self.onToggleLine(line);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (self.onScroll) self.onScroll((int)(-event.scrollingDeltaY / 6.0));
}
@end

/* A PPU view: nearest-neighbour, at the image's own proportions, letterboxed. */
@interface PpuPictureView : NSView
@property (nonatomic) CGImageRef image;
@end

@implementation PpuPictureView
- (void)setImage:(CGImageRef)image
{
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}
- (void)dealloc { if (_image) CGImageRelease(_image); }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];
    CGContextSetRGBFillColor(dc, 0.1, 0.1, 0.1, 1);
    CGContextFillRect(dc, b);
    if (!_image) return;
    const double want = (double)CGImageGetWidth(_image) / (double)CGImageGetHeight(_image);
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; } else { sw = w; sh = sw / want; }
    CGContextSetInterpolationQuality(dc, kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), _image);
}
@end

@interface NESDebuggerWindow ()
- (instancetype)initWithSession:(nessession *)session;
- (void)refreshAll;
@end

@implementation NESDebuggerWindow {
    nessession *_session;
    nesdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _runningTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_promptOut;
    NSTextField *_promptIn;

    NSTextField *_reg[6];
    NSButton *_flag[6];
    NSTextField *_cycles;
    NSTextView *_ram;
    NSTextField *_ramAddr, *_ramVal;

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    NSTextView *_ppuText;
    NSPopUpButton *_ppuViewSel;
    NSStepper *_palStepper;
    NSTextField *_palLabel;
    PpuPictureView *_ppuPic;
    uint32_t *_viewPx;

    NSTextView *_apu;

    NSPopUpButton *_bpType;
    NSTextField *_bpRange, *_bpCond;
    NSStackView *_bpList;

    NSTextView *_cart;
}

static const char *const kFileKinds[5] = { "dis", "prg", "chr", "ram", "wram" };
static NSString *const kFileTitles[5] = { @"Disassembly…", @"PRG…", @"CHR…", @"RAM…", @"WRAM…" };

/* ---- helpers --------------------------------------------------------------- */

static NSString *stripControl(const char *s)
{
    NSMutableString *out = [NSMutableString string];
    char buf[2] = { 0, 0 };
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') {
            buf[0] = *s;
            [out appendString:[NSString stringWithUTF8String:buf] ?: @""];
        }
    }
    return out;
}

/* $hex, 0xhex, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A label, or a number. -1 when neither. */
- (int)resolveAddr:(NSString *)text
{
    long v;
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (t.length == 0) return -1;
    int addr = nesdebug_label_address(_dbg, t.UTF8String);
    if (addr < 0 && parseNum(t, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
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

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(nessession *)session
{
    if (!g_debugger) g_debugger = [[NESDebuggerWindow alloc] initWithSession:session];
    [g_debugger->_window makeKeyAndOrderFront:nil];
    nesdebug_attach(g_debugger->_dbg);
    [g_debugger refreshAll];
}

+ (void)toggleForSession:(nessession *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (instancetype)initWithSession:(nessession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = nessession_debugger(session);
    _viewPx = calloc(NESDEBUG_VIEW_MAX_PIXELS, sizeof *_viewPx);
    [self buildWindow];
    __weak NESDebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    free(_viewPx);
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    nesdebug_detach(_dbg);
}

- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = nesdebug_generation(_dbg);
    const BOOL stopped = nesdebug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        _seenGen = gen;
        _wasStopped = stopped;
        [self refreshAll];
    } else if (!stopped && ++_runningTicks >= 5) {
        /* running: the live values twice a second */
        _runningTicks = 0;
        [self refreshStatus];
        [self refreshCpu];
        [self refreshApu];
        [self refreshCart];
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = monoView(&outScroll, YES);
    _promptOut.string = @"Debugger prompt. Type 'help' for every command.\n";
    _promptIn = [self field:@"command (help, step, break $C000, bpw $0300, print a, mem $6000 32, ...) — Tab completes"
                      width:0 action:@selector(runPrompt:)];
    _promptIn.delegate = self;
    NSButton *sym = [self button:@"Load symbols…" action:@selector(loadSymbols:)];
    NSStackView *row = hstack(@[_promptIn, sym]);

    NSMutableArray *files = [NSMutableArray arrayWithObject:[self label:@"Save"]];
    for (int i = 0; i < 5; i++) {
        NSButton *b = [self button:kFileTitles[i] action:@selector(saveFile:)];
        b.tag = i;
        [files addObject:b];
    }
    NSStackView *saves = hstack(files);

    NSStackView *v = vstack(@[outScroll, row, saves]);
    fill(v, outScroll);
    fill(v, row);
    return v;
}

- (NSView *)buildCpu
{
    static NSString *const names[6] = { @"PC", @"SP", @"A", @"X", @"Y", @"P" };
    NSMutableArray *regs = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        [regs addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:56 action:@selector(applyRegister:)];
        _reg[i].tag = i;
        [regs addObject:_reg[i]];
    }
    static NSString *const fnames[6] = { @"N", @"V", @"D", @"I", @"Z", @"C" };
    NSMutableArray *flags = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    _cycles = [self label:@""];
    _cycles.textColor = NSColor.secondaryLabelColor;
    [flags addObject:_cycles];

    NSScrollView *ramScroll;
    _ram = monoView(&ramScroll, NO);
    _ramAddr = [self field:@"$0300" width:80 action:@selector(writeRam:)];
    _ramVal = [self field:@"$00" width:60 action:@selector(writeRam:)];
    NSTextField *hint = [self label:@"RAM, WRAM, the cartridge's SRAMs and its mailbox all take a write"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *edit = hstack(@[[self label:@"Write address"], _ramAddr, [self label:@"value"], _ramVal, hint]);

    NSStackView *v = vstack(@[hstack(regs), hstack(flags), [self label:@"Console RAM ($0000-$07FF)"], ramScroll, edit]);
    fill(v, ramScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSTextField *hint = [self label:@"Click a line to toggle its breakpoint; scroll to browse"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump, hint]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak NESDebuggerWindow *weakSelf = self;
    _disasm.onToggleLine = ^(int line) {
        NESDebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        nesdebug_breakpoint_toggle(s->_dbg, s->_lineAddr[line]);
        [s refreshDisasm];
        [s refreshBps];
    };
    _disasm.onScroll = ^(int lines) {
        NESDebuggerWindow *s = weakSelf;
        if (!s || lines == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmAddr = (uint16_t)nesdebug_row_address(s->_dbg, s->_disasmAddr, lines);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, scroll);
    return v;
}

- (NSView *)buildPpu
{
    NSScrollView *textScroll;
    _ppuText = monoView(&textScroll, YES);
    NSStackView *left = vstack(@[textScroll]);
    fill(left, textScroll);

    _ppuViewSel = [[NSPopUpButton alloc] init];
    [_ppuViewSel addItemsWithTitles:@[@"Nametables", @"Patterns", @"Sprites", @"Palette"]];
    _ppuViewSel.target = self;
    _ppuViewSel.action = @selector(refreshPpu);
    _palStepper = [[NSStepper alloc] init];
    _palStepper.minValue = 0;
    _palStepper.maxValue = 7;
    _palStepper.increment = 1;
    _palStepper.valueWraps = NO;
    _palStepper.integerValue = 0;
    _palStepper.target = self;
    _palStepper.action = @selector(refreshPpu);
    _palLabel = [self label:@"palette 0"];
    NSStackView *controls = hstack(@[_ppuViewSel, _palLabel, _palStepper]);

    _ppuPic = [[PpuPictureView alloc] initWithFrame:NSMakeRect(0, 0, 512, 480)];
    [_ppuPic.widthAnchor constraintGreaterThanOrEqualToConstant:512].active = YES;
    [_ppuPic.heightAnchor constraintGreaterThanOrEqualToConstant:480].active = YES;
    NSStackView *right = vstack(@[controls, _ppuPic]);
    fill(right, _ppuPic);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:360].active = YES;
    return h;
}

- (NSView *)buildApu
{
    NSScrollView *scroll;
    _apu = monoView(&scroll, YES);
    return scroll;
}

- (NSView *)buildBreaks
{
    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write"]];
    _bpRange = [self field:@"$C000 or $0300-$03FF or a label" width:220 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): a == $10 && x > 2" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear all" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, _bpRange, _bpCond, add, clear]);

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
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
    NSTextField *hint = [self label:@"Untick to disable; conditions are Mesen expressions"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *v = vstack(@[row, hint, bpScroll]);
    fill(v, row);
    fill(v, bpScroll);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

- (NSView *)buildCart
{
    NSScrollView *scroll;
    _cart = monoView(&scroll, YES);
    return scroll;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1100, 760)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. F5/F7/F8/Shift+F8 work as the buttons' key equivalents
     * wherever the focus is inside the window. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *scan = [self button:@"Scanline+1" action:@selector(scanline:)];
    NSButton *frame = [self button:@"Frame+1" action:@selector(frame:)];
    _runBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    _runBtn.keyEquivalentModifierMask = 0;
    step.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    step.keyEquivalentModifierMask = 0;
    over.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    over.keyEquivalentModifierMask = 0;
    outBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    outBtn.keyEquivalentModifierMask = NSEventModifierFlagShift;
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, scan, frame, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Prompt", [self buildPrompt]], @[@"CPU & RAM", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"PPU", [self buildPpu]],
                        @[@"APU & Input", [self buildApu]], @[@"Breakpoints", [self buildBreaks]],
                        @[@"Cart", [self buildCart]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [NSTabViewItem tabViewItemWithViewController:nil];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("NES_DEBUGGER_TAB");
    if (tab && *tab) {
        const int t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }

    NSStackView *root = vstack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (nesdebug_is_stopped(_dbg)) nesdebug_resume(_dbg); else nesdebug_stop(_dbg);
    [self refreshAll];
}
- (void)step:(id)sender { (void)sender; nesdebug_step(_dbg); [self refreshAll]; }
- (void)stepOver:(id)sender { (void)sender; nesdebug_step_over(_dbg); [self refreshAll]; }
- (void)stepOut:(id)sender { (void)sender; nesdebug_step_out(_dbg); [self refreshAll]; }
- (void)scanline:(id)sender { (void)sender; nesdebug_scanline(_dbg, 1); [self refreshAll]; }
- (void)frame:(id)sender { (void)sender; nesdebug_frame(_dbg, 1); [self refreshAll]; }

- (void)appendPrompt:(NSString *)text
{
    [_promptOut.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:text attributes:@{ NSFontAttributeName: _promptOut.font ?: [NSFont userFixedPitchFontOfSize:11] }]];
    [_promptOut scrollRangeToVisible:NSMakeRange(_promptOut.string.length, 0)];
}

- (void)runPrompt:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_promptIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    [self appendPrompt:[NSString stringWithFormat:@"> %@\n", cmd]];
    nesdebug_command(_dbg, cmd.UTF8String, out, sizeof out);
    [self appendPrompt:[stripControl(out) stringByAppendingString:@"\n"]];
    _promptIn.stringValue = @"";
    [self refreshAll];
}

/* Tab in the prompt completes instead of moving the focus. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _promptIn || sel != @selector(insertTab:)) return NO;
    NSString *text = _promptIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = sp.location == NSNotFound ? text : [text substringFromIndex:sp.location + 1];
    char comps[4096];
    const int n = nesdebug_completions(_dbg, word.UTF8String, comps, sizeof comps);
    if (n == 1) {
        NSString *c = [[NSString stringWithUTF8String:comps] componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _promptIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
    } else if (n > 1) {
        [self appendPrompt:[NSString stringWithUTF8String:comps] ?: @""];
    }
    return YES;
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Load Symbols (ld65 -Ln / VICE, ca65 .dbg, Mesen .mlb)";
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    nesdebug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
    [self refreshAll];
}

- (void)saveFile:(NSButton *)sender
{
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[sender.tag]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    nesdebug_save(_dbg, kFileKinds[sender.tag], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[6] = { NES_REG_PC, NES_REG_SP, NES_REG_A, NES_REG_X, NES_REG_Y, NES_REG_PS };
    long v;
    if (parseNum(sender.stringValue, &v)) nesdebug_cpu_set(_dbg, regIds[sender.tag], (int)v);
    [self refreshAll];
}

- (void)flagClicked:(NSButton *)sender
{
    static const int flagIds[6] = { NES_FLAG_N, NES_FLAG_V, NES_FLAG_D, NES_FLAG_I, NES_FLAG_Z, NES_FLAG_C };
    if (nesdebug_is_stopped(_dbg))
        nesdebug_cpu_set(_dbg, flagIds[sender.tag], sender.state == NSControlStateValueOn);
    [self refreshCpu];
}

- (void)writeRam:(id)sender
{
    (void)sender;
    long v;
    const int a = [self resolveAddr:_ramAddr.stringValue];
    if (a >= 0 && parseNum(_ramVal.stringValue, &v))
        nesdebug_write(_dbg, (uint16_t)a, (uint8_t)v);
    [self refreshAll];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) return;
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[4] = { NESDEBUG_BP_EXEC, NESDEBUG_BP_READ, NESDEBUG_BP_WRITE,
                                  NESDEBUG_BP_READ | NESDEBUG_BP_WRITE };
    NSString *range = [_bpRange.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (range.length == 0) return;
    NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
    const int start = [self resolveAddr:ends[0]];
    const int end = ends.count > 1 ? [self resolveAddr:ends[1]] : start;
    if (start < 0 || end < 0) {
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: bad address %@\n", range]];
        _status.stringValue = [NSString stringWithFormat:@"Bad address: %@", range];
        return;
    }
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int bpId = nesdebug_breakpoint_add(_dbg, types[sel >= 0 && sel < 4 ? sel : 0],
                                           (uint16_t)start, (uint16_t)end, _bpCond.stringValue.UTF8String);
    if (bpId < 0) {
        _status.stringValue = @"The condition does not parse";
        return;
    }
    _bpRange.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshAll];
}

- (void)bpEnabled:(NSButton *)sender
{
    nesdebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshAll];
}

- (void)bpRemove:(NSButton *)sender
{
    nesdebug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshAll];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    nesdebug_breakpoint_clear(_dbg);
    [self refreshAll];
}

/* ---- refreshers ------------------------------------------------------------------ */

- (void)refreshStatus
{
    char reason[160];
    int addr;
    const BOOL stopped = nesdebug_is_stopped(_dbg) != 0;
    nesdebug_stop_reason(_dbg, reason, sizeof reason, &addr);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%s%s", reason[0] ? ": " : "", reason] : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? NESAccentColor() : nil;
}

- (void)refreshCpu
{
    nesdebug_cpu c;
    nesdebug_cpu_get(_dbg, &c);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; i++)
        if (!_reg[i].currentEditor)
            _reg[i].stringValue = [NSString stringWithFormat:(i == 0 ? @"%04X" : @"%02X"), vals[i]];
    const int flags[6] = { c.n, c.v, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 6; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _cycles.stringValue = [NSString stringWithFormat:@"cycle %llu   scanline %d   dot %d   frame %u%s%s",
                           (unsigned long long)c.total_cycles, c.scanline, c.dot, c.frame,
                           c.irq ? "   IRQ" : "", c.nmi ? "   NMI" : ""];
}

- (void)refreshRam
{
    static uint8_t ram[2048];
    nesdebug_ram_get(_dbg, ram);
    NSMutableString *text = [NSMutableString stringWithString:@"       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"];
    for (int row = 0; row < 128; row++) {
        [text appendFormat:@"$%04X: ", row * 16];
        for (int col = 0; col < 16; col++) [text appendFormat:@"%02X ", ram[row * 16 + col]];
        [text appendString:@"\n"];
    }
    /* keep the reader's place */
    NSScrollView *scroll = _ram.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    _ram.string = text;
    if (scroll) [scroll.contentView scrollToPoint:at];
}

- (void)refreshDisasm
{
    static nesdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        nesdebug_cpu c;
        nesdebug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)nesdebug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    const int n = nesdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    NSUInteger pcStart = 0, pcLen = 0;
    BOOL havePc = NO;
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-9s %-12s %s\n",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm];
        if (lines[i].is_pc) { havePc = YES; pcStart = text.length; pcLen = line.length - 1; }
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];
    _disasm.string = text;
    if (havePc) {
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: NESAccentColor(),
                                              NSForegroundColorAttributeName: NSColor.whiteColor }
                                     range:NSMakeRange(pcStart, pcLen)];
    }
}

- (void)refreshPpu
{
    nesdebug_ppu p;
    nesdebug_ppu_get(_dbg, &p);
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"Frame %u   scanline %d   dot %d\n\n", p.frame, p.scanline, p.dot];
    [s appendFormat:@"PPUCTRL   $%02X   NMI %s  sprites 8x%d  BG $%s  SPR $%s  +%d\n",
        p.ctrl, p.nmi_on_vblank ? "on" : "off", p.sprite_size_16 ? 16 : 8,
        p.bg_table_1000 ? "1000" : "0000", p.spr_table_1000 ? "1000" : "0000", p.increment_32 ? 32 : 1];
    [s appendFormat:@"PPUMASK   $%02X   BG %s%s  SPR %s%s%s\n",
        p.mask, p.show_bg ? "on" : "off", p.show_bg_left ? "" : " (not left 8)",
        p.show_spr ? "on" : "off", p.show_spr_left ? "" : " (not left 8)", p.grayscale ? "  grayscale" : ""];
    [s appendFormat:@"PPUSTATUS $%02X   vblank %d  sprite 0 hit %d  overflow %d\n",
        p.status, p.vblank, p.sprite0_hit, p.sprite_overflow];
    [s appendFormat:@"OAMADDR   $%02X\n\n", p.oam_addr];
    [s appendFormat:@"v $%04X   t $%04X   fine X %d   w %d\nmirroring %s\n\n",
        p.vram_addr, p.tmp_addr, p.fine_x, p.write_toggle, p.mirroring];

    nesdebug_sprite oam[64];
    nesdebug_oam_get(_dbg, oam);
    [s appendString:@"OAM   #   Y   tile attr  X\n"];
    for (int i = 0; i < 64; i++)
        [s appendFormat:@"     %2d  %3d  $%02X  $%02X  %3d\n", i, oam[i].y, oam[i].tile, oam[i].attr, oam[i].x];
    _ppuText.string = s;

    const int pal = (int)_palStepper.integerValue;
    _palLabel.stringValue = [NSString stringWithFormat:@"palette %d", pal];
    int w = 0, h = 0;
    if (nesdebug_ppu_view(_dbg, (int)_ppuViewSel.indexOfSelectedItem, pal, _viewPx, &w, &h) && w > 0 && h > 0) {
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)_viewPx, (CFIndex)w * h * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                       kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
                                       NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CFRelease(data);
        CGColorSpaceRelease(space);
        _ppuPic.image = img;   /* the view owns it now */
    }
}

- (void)refreshApu
{
    nesdebug_apu a;
    nesdebug_apu_get(_dbg, &a);
    static const char *const names[8] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right" };
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"Pulse 1    %s  period %4d  volume %2d  duty %d\n",
        a.pulse1_enabled ? "on " : "off", a.pulse1_period, a.pulse1_volume, a.pulse1_duty];
    [s appendFormat:@"Pulse 2    %s  period %4d  volume %2d  duty %d\n",
        a.pulse2_enabled ? "on " : "off", a.pulse2_period, a.pulse2_volume, a.pulse2_duty];
    [s appendFormat:@"Triangle   %s  period %4d\n", a.triangle_enabled ? "on " : "off", a.triangle_period];
    [s appendFormat:@"Noise      %s  period %4d  volume %2d\n", a.noise_enabled ? "on " : "off", a.noise_period, a.noise_volume];
    [s appendFormat:@"DMC        %s  %d bytes left\n", a.dmc_enabled ? "on " : "off", a.dmc_bytes_left];
    [s appendFormat:@"IRQs       frame counter %s   DMC %s\n\n", a.frame_irq ? "on" : "off", a.dmc_irq ? "on" : "off"];
    for (int port = 0; port < 2; port++) {
        [s appendFormat:@"Player %d   $%02X ", port + 1, a.pad[port]];
        BOOL any = NO;
        for (int b = 0; b < 8; b++)
            if (a.pad[port] & (1u << b)) { [s appendFormat:@" %s", names[b]]; any = YES; }
        if (!any) [s appendString:@" (nothing held)"];
        [s appendString:@"\n"];
    }
    const char *kbd = nes_keyboard_name(a.keyboard);
    static const char *const tapes[3] = { "idle", "playing", "recording" };
    [s appendFormat:@"\nExpansion  %s\n", kbd ? kbd : "?"];
    if (a.keyboard != NES_KBD_NONE) {
        if (a.keyboard == NES_KBD_FAMILY_BASIC)
            [s appendFormat:@"Tape       %s\n", (a.tape >= 0 && a.tape < 3) ? tapes[a.tape] : "?"];
        [s appendFormat:@"Keys held  %s\n", a.keys_held[0] ? a.keys_held : "(nothing held)"];
    }
    _apu.string = s;
}

- (void)refreshBps
{
    static nesdebug_breakpoint bps[MAX_BPS];
    const int n = nesdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        NSMutableString *what = [NSMutableString stringWithFormat:@"#%d  %s%s%s  $%04X", bps[i].id,
            (bps[i].type & NESDEBUG_BP_EXEC) ? "x" : "", (bps[i].type & NESDEBUG_BP_READ) ? "r" : "",
            (bps[i].type & NESDEBUG_BP_WRITE) ? "w" : "", bps[i].start];
        if (bps[i].end != bps[i].start) [what appendFormat:@"-$%04X", bps[i].end];
        char label[64];
        if (nesdebug_address_label(_dbg, bps[i].start, label, sizeof label) > 0) [what appendFormat:@"  %s", label];
        if (bps[i].condition[0]) [what appendFormat:@"  if %s", bps[i].condition];
        NSButton *box = [NSButton checkboxWithTitle:what target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:hstack(@[box, remove])];
    }
}

- (void)refreshCart
{
    nesdebug_cart c;
    char info[256];
    nesdebug_cart_get(_dbg, &c);
    nesdebug_cart_info(_dbg, info, sizeof info);
    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", stripControl(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge running)\n"];
        _cart.string = s;
        return;
    }
    [s appendFormat:@"Link        %s%s%s\n", c.link_up ? "up" : "down", c.busy ? " (busy)" : "",
        (!c.link_up && c.link_error[0]) ? [[NSString stringWithFormat:@": %s", c.link_error] UTF8String] : ""];
    [s appendFormat:@"Mailbox     %s   ACKSEQ $%02X   status $%02X   last error %u   queue %u\n",
        c.mailbox_live ? "open" : "closed", c.ackseq, c.status, c.last_error, c.queue_depth];
    [s appendFormat:@"Boot        state $%02X   %u%%   error %u\n", c.boot_state, c.boot_pct, c.boot_err];
    [s appendFormat:@"Image       %s%s\n", c.booted_image ? "a game" : "CONFIG",
        c.loading ? [[NSString stringWithFormat:@" (loading %d%%)", c.load_pct] UTF8String] : ""];
    [s appendFormat:@"SRAM        %s\n", c.sram_enabled ? "enabled" : "off (serving the loader's vectors)"];
    [s appendFormat:@"Mapper      %d %s\n", c.mapper, c.mapper_name];
    [s appendFormat:@"PRG slots   $%02X $%02X $%02X $%02X\n", c.prg_slot[0], c.prg_slot[1], c.prg_slot[2], c.prg_slot[3]];
    [s appendString:@"CHR slots  "];
    for (int i = 0; i < 8; i++) [s appendFormat:@" $%03X", c.chr_slot[i]];
    [s appendFormat:@"\nMirroring   %s\n", c.mirroring];
    [s appendFormat:@"WRAM        %s%s   CHR %s\n", c.wram_enabled ? "on" : "off",
        c.wram_protected ? " (write-protected)" : "", c.chr_writable ? "RAM (writable)" : "ROM"];
    [s appendFormat:@"IRQ         %s   line %d   latch %d   counter %d\n",
        c.irq_enabled ? "enabled" : "disabled", c.irq_line, c.irq_latch, c.irq_counter];
    [s appendFormat:@"RMW dummy writes dropped: %u\n", c.diag_rmw];
    _cart.string = s;
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshCpu];
    [self refreshRam];
    [self refreshDisasm];
    [self refreshPpu];
    [self refreshApu];
    [self refreshBps];
    [self refreshCart];
}
@end
