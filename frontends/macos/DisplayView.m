/*
 * NESDisplayView -- the framebuffer, on a CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * Its callback runs on its OWN high-priority thread, not the main one. So it
 * does the two cheap things (hand over the tick, pull the frame) and then
 * asks AppKit to redraw on the main thread. Drawing from the callback thread
 * would be a use of AppKit off the main thread, which is undefined.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <stdlib.h>
#include <string.h>

@implementation NESDisplayView {
    nessession *_session;
    CVDisplayLinkRef _link;
    uint32_t *_fb;
    int _height;
    uint64_t _serial;
    CGContextRef _ctx;
    CGColorSpaceRef _cs;
    BOOL _tv;
    BOOL _smooth;
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    NESDisplayView *self = (__bridge NESDisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(nessession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 960, 720)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _smooth = NO;

    const size_t n = (size_t)NESSESSION_FB_WIDTH * NESSESSION_FB_MAX_HEIGHT;
    _fb = calloc(n, sizeof *_fb);
    _cs = CGColorSpaceCreateDeviceRGB();
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit context with the alpha byte skipped reads them as they are. */
    _ctx = CGBitmapContextCreate(_fb, NESSESSION_FB_WIDTH, NESSESSION_FB_MAX_HEIGHT, 8,
                                 NESSESSION_FB_WIDTH * 4, _cs,
                                 kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    CVDisplayLinkStart(_link);
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_ctx) CGContextRelease(_ctx);
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
}

- (void)tick:(uint64_t)hostTime
{
    static double toNs = 0.0;
    if (toNs == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        toNs = (double)tb.numer / (double)tb.denom;
    }
    nessession_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

    int h = 0;
    if (!nessession_copy_frame(_session, _fb, &h, &_serial)) return;
    _height = h;
    /* AppKit is main-thread only; the display link's callback is not. */
    dispatch_async(dispatch_get_main_queue(), ^{ [self setNeedsDisplay:YES]; });
}

- (void)setTvAspect:(BOOL)tv { _tv = tv; [self setNeedsDisplay:YES]; }
- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }

- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);
    if (_height <= 0) return;

    CGImageRef whole = CGBitmapContextCreateImage(_ctx);
    if (!whole) return;
    /* Only the lines the PPU produced (240). */
    CGImageRef img = CGImageCreateWithImageInRect(whole, CGRectMake(0, 0, NESSESSION_FB_WIDTH, _height));
    CGImageRelease(whole);
    if (!img) return;

    /* On a television each NES pixel is 8:7 (wider than tall): 256 of them
     * span the width of 292 square ones. Square pixels show 256x240 as is. */
    const double par = _tv ? (8.0 / 7.0) : 1.0;
    const double want = (double)NESSESSION_FB_WIDTH * par / (double)_height;
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), img);
    CGImageRelease(img);
}
@end
