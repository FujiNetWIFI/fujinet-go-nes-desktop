/*
 * The debugger's Video tab (AppKit).
 *
 * Everything that decides what to show lives in the core (nesvidtab.h): this file is views.
 * The picture comes from the controller's cached 0xFFRRGGBB buffer and is drawn as a CGImage
 * (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, no interpolation) over a copy of
 * that buffer; the image is rebuilt only when the controller re-rendered. A mouse move redraws
 * only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#import "VideoTab.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nesdebug.h"
#include "nessession.h"
#include "nesvidcap.h"
#include "nesvidtab.h"

#define MAX_OVERLAYS 8
#define SWATCH_ROWS 32         /* the 32 palette RAM entries */

/* A C string as an NSString, never nil (invalid UTF-8 reads as empty). */
static NSString *str(const char *text)
{
    NSString *value = text ? [NSString stringWithUTF8String:text] : nil;
    return value ? value : @"";
}

/* A 0xFFRRGGBB word as a colour. */
static NSColor *colorFromRGB(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                               green:((rgb >> 8) & 255) / 255.0
                                blue:(rgb & 255) / 255.0
                               alpha:1.0];
}

/* The accent (the icon's colour) as a fill, the ink on it, and the accent as a
 * line on the picture's dark ground. */
static NSColor *accentFill(void)
{
    return colorFromRGB(NESSESSION_ACCENT_RGB);
}

static NSColor *accentInk(void)
{
    return colorFromRGB(NESSESSION_ACCENT_INK_RGB);
}

static NSColor *accentLight(void)
{
    return colorFromRGB(NESSESSION_ACCENT_LIGHT_RGB);
}

static NSFont *monoFont11(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

/* A selectable monospace label that wraps at `width` (0: no limit). */
static NSTextField *monoLabel(CGFloat width)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];
    label.font = monoFont11();
    label.selectable = YES;
    if (width > 0) label.preferredMaxLayoutWidth = width;
    return label;
}

/* ---- the picture ------------------------------------------------------------------ */

@interface NESVideoPicture : NSView
@property (nonatomic, assign) nesvid_tab *tab;
@property (nonatomic, copy) void (^onChanged)(void);        /* the inspector changed */
@property (nonatomic, copy) void (^onBreak)(void);          /* double-click: break on the pixel */
- (void)sync;                                               /* the controller re-rendered */
@end

@implementation NESVideoPicture {
    CGImageRef _image;
    NSTrackingArea *_area;
    NSString *_message;
    NSSize _size;
}

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
    if (_image) CGImageRelease(_image);
}

- (NSSize)intrinsicContentSize { return _size.width > 0 ? _size : NSMakeSize(320, 240); }

- (void)sync
{
    int width = 0, height = 0;
    const uint32_t *pixels = nesvid_tab_picture(_tab, &width, &height);
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (pixels && width > 0 && height > 0) {
        /* a copy: the controller's buffer lives only until its next refresh */
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)width * height * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        _image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4, space,
                               kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider, NULL, false,
                               kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(data);
        _message = nil;
        _size = NSMakeSize(width, height);
    } else {
        const char *why = nesvid_tab_why(_tab);
        _message = str(why ? why : "Nothing to show");
        _size = NSMakeSize(0, 0);
    }
    [self invalidateIntrinsicContentSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
    [[NSColor colorWithCalibratedRed:0.07 green:0.07 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    if (!_image) {
        NSDictionary *attrs = @{ NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1],
                                 NSFontAttributeName: [NSFont systemFontOfSize:13] };
        NSString *message = _message ? _message : @"";
        const NSSize textSize = [message sizeWithAttributes:attrs];
        [message drawAtPoint:NSMakePoint((self.bounds.size.width - textSize.width) / 2,
                                         (self.bounds.size.height - textSize.height) / 2)
              withAttributes:attrs];
        return;
    }
    /* the view is flipped: draw the image upright */
    CGContextSaveGState(context);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextTranslateCTM(context, 0, _size.height);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context, CGRectMake(0, 0, _size.width, _size.height), _image);
    CGContextRestoreGState(context);
    int hover[4] = { 0 }, pin[4] = { 0 };
    const int marks = nesvid_tab_marks(_tab, hover, pin);
    if (marks & 1) {
        [[NSColor colorWithCalibratedWhite:1 alpha:0.9] setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(hover[0] + 0.5, hover[1] + 0.5,
                                                                         hover[2] - 1, hover[3] - 1)];
        path.lineWidth = 1;
        [path stroke];
    }
    if (marks & 2) {
        [accentLight() setStroke];
        NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSMakeRect(pin[0] + 1, pin[1] + 1,
                                                                         pin[2] - 2, pin[3] - 2)];
        path.lineWidth = 2;
        [path stroke];
    }
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc] initWithRect:self.bounds
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}

- (NSPoint)local:(NSEvent *)event { return [self convertPoint:event.locationInWindow fromView:nil]; }

- (void)mouseMoved:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (nesvid_tab_hover(_tab, (int)point.x, (int)point.y)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (nesvid_tab_hover(_tab, -1, -1)) {
        [self setNeedsDisplay:YES];
        if (_onChanged) _onChanged();
    }
}

- (void)mouseDown:(NSEvent *)event
{
    const NSPoint point = [self local:event];
    if (event.clickCount >= 2) {
        nesvid_tab_hover(_tab, (int)point.x, (int)point.y);
        if (_onBreak) _onBreak();
    } else {
        nesvid_tab_pin(_tab, (int)point.x, (int)point.y);
    }
    [self setNeedsDisplay:YES];
    if (_onChanged) _onChanged();
}
@end

/* ---- a colour swatch ---------------------------------------------------------------- */

@interface NESSwatchView : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation NESSwatchView
- (NSSize)intrinsicContentSize { return NSMakeSize(22, 16); }

- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ? _color : NSColor.blackColor) setFill];
    NSRectFill(self.bounds);
    [[NSColor colorWithCalibratedWhite:0.5 alpha:1] setStroke];
    NSBezierPath *path = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    path.lineWidth = 1;
    [path stroke];
}
@end

/* ---- the tab ------------------------------------------------------------------------------ */

@interface NESVideoTab () <NSTableViewDataSource, NSTableViewDelegate, NSTabViewDelegate>
@end

@implementation NESVideoTab {
    nesvid_tab *_tab;
    NSView *_root;
    NSPopUpButton *_viewPopup, *_sourcePopup, *_zoomPopup, *_palettePopup;
    NSButton *_breakButton, *_pixelBreak;
    NSButton *_overlayBox[MAX_OVERLAYS];
    NSTextField *_status, *_note, *_pixelText, *_timingText;
    NSTextField *_swatchLabel[SWATCH_ROWS];
    NESSwatchView *_swatchView[SWATCH_ROWS];
    NESVideoPicture *_picture;
    NSScrollView *_scroll;
    NSTabView *_sections;
    nessession *_session;
    NSTableView *_regsTable, *_linesTable, *_spritesTable;
    NSProgressIndicator *_beam;
    NSString *_regsSignature, *_linesSignature, *_spritesSignature;
    BOOL _updating;
    int _regCount, _lineCount, _spriteCount;
    int _viewportWidth, _viewportHeight;
    id _resizeObserver;
}

- (NSView *)view { return _root; }

- (instancetype)initWithSession:(nessession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _tab = nesvid_tab_new(session);
    [self build];
    return self;
}

- (void)dealloc
{
    if (_resizeObserver) [[NSNotificationCenter defaultCenter] removeObserver:_resizeObserver];
    if (_tab) nesvid_tab_free(_tab);
    _tab = NULL;
}

- (void)build
{
    int overlayCount = 0;
    const nesvid_overlay_info *overlays = nesvid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    int zoomCount = 0;
    const int *zooms = nesvid_zooms(&zoomCount);

    _viewPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int idx = 0; idx < NESVID_V_COUNT; idx++) [_viewPopup addItemWithTitle:str(nesvid_view_name(idx))];
    _viewPopup.target = self;
    _viewPopup.action = @selector(onView:);
    _viewPopup.toolTip = @"Display: the last finished frame, as the main window shows it.\n"
                          "Decoded: one source redrawn from the PPU's memory (see Source).\n"
                          "Beam: the frame as the PPU is drawing it, the lines the beam has not reached dimmed (stopped).\n"
                          "Values: each nametable entry as a flat false colour of its value.";
    _sourcePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int idx = 0; idx < NESVID_SRC_COUNT; idx++) [_sourcePopup addItemWithTitle:str(nesvid_source_name(idx))];
    _sourcePopup.target = self;
    _sourcePopup.action = @selector(onSource:);
    _sourcePopup.toolTip = @"What the Decoded view redraws: the four nametables, the pattern tables, the 64 sprites or "
                            "the palette";
    _palettePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_palettePopup addItemsWithTitles:@[@"BG palette 0", @"BG palette 1", @"BG palette 2", @"BG palette 3",
                                        @"Sprite palette 0", @"Sprite palette 1", @"Sprite palette 2",
                                        @"Sprite palette 3"]];
    _palettePopup.target = self;
    _palettePopup.action = @selector(onPalette:);
    _palettePopup.toolTip = @"The Patterns source draws in one of the eight palettes: four background, four sprite";
    _zoomPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int idx = 0; idx < zoomCount; idx++)
        [_zoomPopup addItemWithTitle:zooms[idx] == 0 ? @"Fit" : [NSString stringWithFormat:@"%d×", zooms[idx]]];
    _zoomPopup.target = self;
    _zoomPopup.action = @selector(onZoom:);
    _breakButton = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
    _breakButton.toolTip = @"Add write breakpoints on what the inspected pixel is made from: a nametable entry, its attribute or pattern bytes, a palette entry, a sprite's OAM bytes, or the PPU registers that changed on a band edge";
    NSStackView *bar = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"View"], _viewPopup,
                                                         [NSTextField labelWithString:@"Source"], _sourcePopup, _palettePopup,
                                                         [NSTextField labelWithString:@"Zoom"], _zoomPopup,
                                                         _breakButton]];
    bar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    bar.spacing = 8;

    NSMutableArray<NSView *> *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int idx = 0; idx < overlayCount; idx++) {
        _overlayBox[idx] = [NSButton checkboxWithTitle:str(overlays[idx].label) target:self action:@selector(onOverlay:)];
        [(idx < 3 ? row1 : row2) addObject:_overlayBox[idx]];
    }
    NSStackView *overlayRow1 = [NSStackView stackViewWithViews:row1];
    NSStackView *overlayRow2 = [NSStackView stackViewWithViews:row2];
    overlayRow1.orientation = overlayRow2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    overlayRow1.spacing = overlayRow2.spacing = 10;

    _status = monoLabel(0);
    _picture = [[NESVideoPicture alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    _picture.tab = _tab;
    __weak NESVideoTab *weak = self;
    _picture.onChanged = ^{ [weak updatePixel]; };
    _picture.onBreak = ^{ [weak breakOnPixel]; };
    _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 640, 400)];
    _scroll.hasVerticalScroller = YES;
    _scroll.hasHorizontalScroller = YES;
    _scroll.documentView = _picture;
    _scroll.drawsBackground = NO;
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    [_scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [NSTextField labelWithString:@""];

    NSStackView *left = [NSStackView stackViewWithViews:@[bar, overlayRow1, overlayRow2, _status, _scroll, _note]];
    left.orientation = NSUserInterfaceLayoutOrientationVertical;
    left.alignment = NSLayoutAttributeLeading;
    left.spacing = 6;
    [_scroll.widthAnchor constraintEqualToAnchor:left.widthAnchor].active = YES;

    /* Fit follows the scroll view's size */
    _scroll.postsFrameChangedNotifications = YES;
    _resizeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSViewFrameDidChangeNotification
                    object:_scroll
                     queue:nil
                usingBlock:^(NSNotification *note) {
        (void)note;
        [weak viewportChanged];
    }];

    /* ---- the sections, in nesvid_section_name order */
    _sections = [[NSTabView alloc] init];
    _sections.delegate = self;
    {   /* Pixel */
        _pixelText = monoLabel(370);
        _pixelBreak = [NSButton buttonWithTitle:@"Break on writes" target:self action:@selector(onBreak:)];
        NSButton *unpin = [NSButton buttonWithTitle:@"Unpin" target:self action:@selector(onUnpin:)];
        NSStackView *buttons = [NSStackView stackViewWithViews:@[_pixelBreak, unpin]];
        buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        buttons.spacing = 6;
        NSStackView *page = [NSStackView stackViewWithViews:@[_pixelText, buttons]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [self addSection:@"Pixel" content:page];
    }
    {   /* Registers */
        _regsTable = [[NSTableView alloc] init];
        [self addSection:@"Registers" content:[self scrollForTable:_regsTable
                                                           columns:@[@"name", @"val", @"writes", @"line", @"meaning"]
                                                            titles:@[@"Register", @"Value", @"Writes", @"Line", @"Meaning"]
                                                            widths:@[@90, @46, @50, @44, @400]]];
        _regsTable.target = self;
        _regsTable.doubleAction = @selector(onRegisterBreak:);
    }
    {   /* Colours: the 32 palette RAM entries, with the pixels each has and its role */
        NSMutableArray<NSView *> *rows = [NSMutableArray array];
        for (int idx = 0; idx < SWATCH_ROWS; idx++) {
            _swatchView[idx] = [[NESSwatchView alloc] initWithFrame:NSMakeRect(0, 0, 22, 16)];
            _swatchLabel[idx] = monoLabel(0);
            _swatchLabel[idx].selectable = NO;
            NSStackView *row = [NSStackView stackViewWithViews:@[_swatchView[idx], _swatchLabel[idx]]];
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            row.spacing = 8;
            [rows addObject:row];
        }
        NSStackView *page = [NSStackView stackViewWithViews:rows];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 2;
        NSScrollView *colourScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        colourScroll.hasVerticalScroller = YES;
        colourScroll.drawsBackground = NO;
        page.translatesAutoresizingMaskIntoConstraints = NO;
        colourScroll.documentView = page;
        [page.topAnchor constraintEqualToAnchor:colourScroll.contentView.topAnchor].active = YES;
        [page.leadingAnchor constraintEqualToAnchor:colourScroll.contentView.leadingAnchor].active = YES;
        [self addSection:@"Colours" content:colourScroll];
    }
    {   /* Lines: the bands of picture lines drawn from the same PPU state */
        _linesTable = [[NSTableView alloc] init];
        [self addSection:@"Lines" content:[self scrollForTable:_linesTable
                                                       columns:@[@"band", @"scroll", @"ctrl", @"mask", @"finex", @"vaddr", @"writes", @"mapper"]
                                                        titles:@[@"Lines", @"Scroll X,Y", @"$2000", @"$2001", @"Fine X", @"VRAM", @"Writes", @"Mapper"]
                                                        widths:@[@64, @84, @48, @48, @50, @54, @48, @54]]];
    }
    {   /* Timing */
        _beam = [[NSProgressIndicator alloc] init];
        _beam.indeterminate = NO;
        _beam.minValue = 0;
        _beam.maxValue = 1000;
        _timingText = monoLabel(370);
        NSStackView *page = [NSStackView stackViewWithViews:@[_beam, _timingText]];
        page.orientation = NSUserInterfaceLayoutOrientationVertical;
        page.alignment = NSLayoutAttributeLeading;
        page.spacing = 8;
        [_beam.widthAnchor constraintEqualToAnchor:page.widthAnchor].active = YES;
        [self addSection:@"Timing" content:page];
    }
    {   /* Sprites: the 64 OAM entries, the ones off the picture dimmed */
        _spritesTable = [[NSTableView alloc] init];
        [self addSection:@"Sprites" content:[self scrollForTable:_spritesTable
                                                         columns:@[@"sprite"]
                                                          titles:@[@"OAM    #   X    Y  tile  attr"]
                                                          widths:@[@520]]];
    }
    [_sections.widthAnchor constraintEqualToConstant:560].active = YES;

    NSStackView *root = [NSStackView stackViewWithViews:@[left, _sections]];
    root.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    root.alignment = NSLayoutAttributeTop;
    root.spacing = 10;
    _root = root;
    [left setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* start from the controller's state (the NES_DEBUGGER_VIDEO_* variables) */
    _updating = YES;
    [_viewPopup selectItemAtIndex:nesvid_tab_view(_tab)];
    [_sourcePopup selectItemAtIndex:nesvid_tab_source(_tab)];
    [_palettePopup selectItemAtIndex:nesvid_tab_palette(_tab)];
    for (int idx = 0; idx < zoomCount; idx++)
        if (zooms[idx] == nesvid_tab_zoom(_tab)) [_zoomPopup selectItemAtIndex:idx];
    for (int idx = 0; idx < overlayCount; idx++)
        _overlayBox[idx].state = (nesvid_tab_overlays(_tab) & overlays[idx].bit) ? NSControlStateValueOn
                                                                             : NSControlStateValueOff;
    const int section = nesvid_tab_env_section(_tab);
    if (section > 0 && section < (int)_sections.numberOfTabViewItems) [_sections selectTabViewItemAtIndex:section];
    _updating = NO;
}

/* Sets a table up and returns the scroll view that holds it. */
- (NSScrollView *)scrollForTable:(NSTableView *)table
                         columns:(NSArray<NSString *> *)ids
                          titles:(NSArray<NSString *> *)titles
                          widths:(NSArray<NSNumber *> *)widths
{
    for (NSUInteger idx = 0; idx < ids.count; idx++) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:ids[idx]];
        column.title = titles[idx];
        column.width = widths[idx].doubleValue;
        [table addTableColumn:column];
    }
    table.dataSource = self;
    table.delegate = self;
    table.usesAlternatingRowBackgroundColors = YES;
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.documentView = table;
    return scroll;
}

- (void)addSection:(NSString *)title content:(NSView *)content
{
    NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    if ([content isKindOfClass:[NSScrollView class]])
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    item.view = holder;
    [_sections addTabViewItem:item];
}

/* ---- actions -------------------------------------------------------------------------- */

- (void)onView:(id)sender
{
    (void)sender;
    if (_updating) return;
    nesvid_tab_set_view(_tab, (int)_viewPopup.indexOfSelectedItem);
    [self refreshView:NESVID_TAB_PICTURE | NESVID_TAB_TABLES];
}

- (void)onSource:(id)sender
{
    (void)sender;
    if (_updating) return;
    nesvid_tab_set_source(_tab, (int)_sourcePopup.indexOfSelectedItem);
    [self refreshView:NESVID_TAB_PICTURE | NESVID_TAB_TABLES];
}

- (void)onPalette:(id)sender
{
    (void)sender;
    if (_updating) return;
    nesvid_tab_set_palette(_tab, (int)_palettePopup.indexOfSelectedItem);
    [self refreshView:NESVID_TAB_PICTURE | NESVID_TAB_TABLES];
}

/* A double-click on a Registers row: a write breakpoint on that register. */
- (void)onRegisterBreak:(id)sender
{
    (void)sender;
    const NSInteger row = _regsTable.clickedRow;
    if (row < 0) return;
    const int ident = nesvid_tab_break_on_register(_tab, (int)row);
    _note.stringValue = ident >= 0 ? @"Break on writes to that register (see Breakpoints)"
                                   : @"Only $2000, $2001, $2003, $2005 and $2006 can be broken on here";
}

- (void)onZoom:(id)sender
{
    (void)sender;
    int zoomCount = 0;
    const int *zooms = nesvid_zooms(&zoomCount);
    const NSInteger index = _zoomPopup.indexOfSelectedItem;
    if (_updating || index < 0 || index >= zoomCount) return;
    nesvid_tab_set_zoom(_tab, zooms[index]);
    [self refreshView:NESVID_TAB_PICTURE];
}

- (void)onOverlay:(id)sender
{
    (void)sender;
    if (_updating) return;
    int overlayCount = 0;
    const nesvid_overlay_info *overlays = nesvid_overlays(&overlayCount);
    if (overlayCount > MAX_OVERLAYS) overlayCount = MAX_OVERLAYS;
    unsigned mask = 0;
    for (int idx = 0; idx < overlayCount; idx++)
        if (_overlayBox[idx].state == NSControlStateValueOn) mask |= overlays[idx].bit;
    nesvid_tab_set_overlays(_tab, mask);
    [self refreshView:NESVID_TAB_PICTURE];
}

/* Write breakpoints on what the inspected pixel is made from, with a note of what happened. */
- (void)breakOnPixel
{
    const int ident = nesvid_tab_break_on_pixel(_tab);
    _note.stringValue = ident >= 0 ? @"Break on writes to what the pixel is made from (see Breakpoints)"
                                   : @"Nothing to break on here: pick a pixel that something drew";
}

- (void)onBreak:(id)sender
{
    (void)sender;
    [self breakOnPixel];
}

- (void)onUnpin:(id)sender
{
    (void)sender;
    nesvid_tab_unpin(_tab);
    [_picture setNeedsDisplay:YES];
    [self updatePixel];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    if (!_updating) [self refreshView:NESVID_TAB_TABLES];
}

/* The picture's room changed: Fit needs a new picture. */
- (void)viewportChanged
{
    const NSSize viewport = _scroll.contentSize;
    if ((int)viewport.width == _viewportWidth && (int)viewport.height == _viewportHeight) return;
    if (nesvid_tab_zoom(_tab) == 0) {
        [self refreshView:NESVID_TAB_PICTURE];
    } else {
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        nesvid_tab_set_viewport(_tab, _viewportWidth > 2 ? _viewportWidth - 2 : 1,
                                _viewportHeight > 2 ? _viewportHeight - 2 : 1);
    }
}

/* ---- the sections ----------------------------------------------------------------------- */

- (void)updatePixel
{
    const nesvid_pixel *pixel = nesvid_tab_pixel(_tab);
    _pixelText.stringValue = pixel ? str(pixel->text)
                                   : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                      "Double-click to break on writes to it.";
    _pixelBreak.enabled = nesvid_tab_can_break(_tab) != 0;
    _breakButton.enabled = _pixelBreak.enabled;
    /* the swatch rows mark the inspected colour */
    if ([self sectionIndex] == NESVID_S_COLOURS) [self fillColours];
}

- (NSInteger)sectionIndex
{
    return (NSInteger)[_sections.tabViewItems indexOfObject:_sections.selectedTabViewItem];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)table
{
    if (table == _linesTable) return _lineCount;
    if (table == _spritesTable) return _spriteCount;
    return _regCount;
}

- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row
{
    NSString *text = @"";
    BOOL hot = NO, dim = NO;
    if (table == _regsTable) {
        const nesvid_reg *regs = NULL;
        const int count = nesvid_tab_regs(_tab, &regs);
        if (!regs || row < 0 || row >= count) return nil;
        if ([column.identifier isEqualToString:@"name"]) text = str(regs[row].name);
        else if ([column.identifier isEqualToString:@"val"]) text = str(regs[row].value);
        else if ([column.identifier isEqualToString:@"writes"])
            text = regs[row].writes ? [NSString stringWithFormat:@"%d", regs[row].writes] : @"";
        else if ([column.identifier isEqualToString:@"line"])
            text = regs[row].first_line >= 0 ? [NSString stringWithFormat:@"%d", regs[row].first_line] : @"";
        else text = str(regs[row].meaning);
        hot = regs[row].age < NESVID_REG_FADE;
    } else if (table == _spritesTable) {
        const nesvid_spriterow *sprites = NULL;
        const int count = nesvid_tab_sprites(_tab, &sprites);
        if (!sprites || row < 0 || row >= count) return nil;
        text = str(sprites[row].text);
        dim = !sprites[row].visible;
    } else {
        const nesvid_band *bands = NULL;
        const int count = nesvid_tab_lines(_tab, &bands);
        if (!bands || row < 0 || row >= count) return nil;
        NSString *identifier = column.identifier;
        if ([identifier isEqualToString:@"band"])
            text = [NSString stringWithFormat:@"%d-%d", bands[row].first, bands[row].last];
        else if ([identifier isEqualToString:@"scroll"])
            text = [NSString stringWithFormat:@"%d,%d", bands[row].scroll_x, bands[row].scroll_y];
        else if ([identifier isEqualToString:@"ctrl"]) text = [NSString stringWithFormat:@"%02X", bands[row].ctrl];
        else if ([identifier isEqualToString:@"mask"]) text = [NSString stringWithFormat:@"%02X", bands[row].mask];
        else if ([identifier isEqualToString:@"finex"]) text = [NSString stringWithFormat:@"%d", bands[row].fine_x];
        else if ([identifier isEqualToString:@"vaddr"]) text = [NSString stringWithFormat:@"%04X", bands[row].vaddr];
        else if ([identifier isEqualToString:@"writes"]) text = [NSString stringWithFormat:@"%d", bands[row].writes];
        else text = [NSString stringWithFormat:@"%d", bands[row].mapper_writes];
        hot = bands[row].writes != 0 || bands[row].mapper_writes != 0;
    }
    NSTextField *field = [table makeViewWithIdentifier:column.identifier owner:self];
    if (![field isKindOfClass:[NSTextField class]]) {
        field = [NSTextField labelWithString:@""];
        field.identifier = column.identifier;
        field.font = monoFont11();
    }
    field.stringValue = text;
    field.drawsBackground = hot;
    field.backgroundColor = hot ? accentFill() : NSColor.clearColor;
    field.textColor = hot ? accentInk() : (dim ? NSColor.secondaryLabelColor : NSColor.labelColor);
    return field;
}

- (void)fillRegs
{
    const nesvid_reg *regs = NULL;
    const int count = nesvid_tab_regs(_tab, &regs);
    NSMutableString *signature = [NSMutableString string];
    for (int idx = 0; regs && idx < count; idx++)
        [signature appendFormat:@"%s%c", regs[idx].value, regs[idx].age < NESVID_REG_FADE ? '*' : '.'];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_regsSignature]) return;
    _regsSignature = signature;
    _regCount = regs ? count : 0;
    [_regsTable reloadData];
}

- (void)fillLines
{
    const nesvid_band *bands = NULL;
    const int count = nesvid_tab_lines(_tab, &bands);
    NSMutableString *signature = [NSMutableString string];
    for (int idx = 0; bands && idx < count; idx++)
        [signature appendFormat:@"%d:%d:%d,%d:%X:%X:%d:%X:%d:%d;", bands[idx].first, bands[idx].last, bands[idx].scroll_x,
                                bands[idx].scroll_y, bands[idx].ctrl, bands[idx].mask, bands[idx].fine_x,
                                bands[idx].vaddr, bands[idx].writes, bands[idx].mapper_writes];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_linesSignature]) return;
    _linesSignature = signature;
    _lineCount = bands ? count : 0;
    [_linesTable reloadData];
}

- (void)fillSprites
{
    const nesvid_spriterow *sprites = NULL;
    const int count = nesvid_tab_sprites(_tab, &sprites);
    NSMutableString *signature = [NSMutableString string];
    for (int idx = 0; sprites && idx < count; idx++)
        [signature appendFormat:@"%c%s;", sprites[idx].visible ? '+' : '-', sprites[idx].text];
    [signature appendFormat:@"%d", count];
    if ([signature isEqualToString:_spritesSignature]) return;
    _spritesSignature = signature;
    _spriteCount = sprites ? count : 0;
    [_spritesTable reloadData];
}

- (void)fillColours
{
    const nesvid_swatch *swatches = NULL;
    const int count = nesvid_tab_swatches(_tab, &swatches);
    for (int idx = 0; idx < SWATCH_ROWS; idx++) {
        if (!swatches || idx >= count) {
            _swatchLabel[idx].stringValue = @"";
            _swatchView[idx].color = nil;
            continue;
        }
        _swatchView[idx].color = colorFromRGB(swatches[idx].rgb);
        _swatchLabel[idx].stringValue = [NSString stringWithFormat:@"%@%-12s #%06X  %6ld px  %@",
                                                                 swatches[idx].inspected ? @"> " : @"  ",
                                                                 swatches[idx].name, swatches[idx].rgb & 0xFFFFFF,
                                                                 swatches[idx].uses, str(swatches[idx].role)];
    }
}

- (void)fillTiming
{
    const char *const *lines = NULL;
    const int count = nesvid_tab_timing(_tab, &lines);
    NSMutableString *text = [NSMutableString string];
    for (int idx = 0; lines && idx < count; idx++) {
        NSString *line = str(lines[idx]);
        const NSRange tab = [line rangeOfString:@"\t"];
        if (tab.location != NSNotFound)
            [text appendFormat:@"%-13s %@\n", [[line substringToIndex:tab.location] UTF8String],
                               [line substringFromIndex:tab.location + 1]];
        else
            [text appendFormat:@"%@\n", line];
    }
    _timingText.stringValue = count ? text : @"The machine is off";
    _beam.doubleValue = nesvid_tab_beam_permille(_tab);
}

/* ---- refresh ------------------------------------------------------------------------------ */

- (void)refreshView:(unsigned)what
{
    if (what & NESVID_TAB_PICTURE) {
        const NSSize viewport = _scroll.contentSize;
        _viewportWidth = (int)viewport.width;
        _viewportHeight = (int)viewport.height;
        if (viewport.width > 4 && viewport.height > 4)
            nesvid_tab_set_viewport(_tab, (int)viewport.width - 2, (int)viewport.height - 2);
        [_picture sync];
        [_picture setFrameSize:_picture.intrinsicContentSize];
    }
    if (what & NESVID_TAB_TABLES) {
        _status.stringValue = str(nesvid_tab_status(_tab));
        switch ([self sectionIndex]) {
        case NESVID_S_REGISTERS: [self fillRegs]; break;
        case NESVID_S_COLOURS: [self fillColours]; break;
        case NESVID_S_LINES: [self fillLines]; break;
        case NESVID_S_TIMING: [self fillTiming]; break;
        case NESVID_S_SPRITES: [self fillSprites]; break;
        default: break;
        }
        const nesvid_pixel *pixel = nesvid_tab_pixel(_tab);
        _pixelText.stringValue = pixel ? str(pixel->text)
                                       : @"Move over the picture to inspect a pixel.\nClick to pin it; click again to unpin.\n"
                                          "Double-click to break on writes to it.";
        _pixelBreak.enabled = nesvid_tab_can_break(_tab) != 0;
        _breakButton.enabled = _pixelBreak.enabled;
    }
}

- (void)refreshForce:(BOOL)force
{
    unsigned what = nesvid_tab_refresh(_tab, force ? 1 : 0);
    if (force) what |= NESVID_TAB_PICTURE | NESVID_TAB_TABLES;
    if (what & NESVID_TAB_OFF) what |= NESVID_TAB_PICTURE | NESVID_TAB_TABLES;     /* the message */
    [self refreshView:what];
}

@end
