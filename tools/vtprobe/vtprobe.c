/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause */
/* Print what this machine's hardware H.264 encoder supports: whether a
 * hardware-only session opens at the given size, and its supported property
 * dictionary. The option map in src/hw/vt.c is checked against this output,
 * not against memory (docs/videotoolbox-plan.md step 1). */
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>

static void cb(void *a, void *b, OSStatus s, VTEncodeInfoFlags f, CMSampleBufferRef sb)
{ (void)a; (void)b; (void)s; (void)f; (void)sb; }

static void print_cf(CFTypeRef v, int depth);
static void print_dict_entry(const void *k, const void *v, void *ctx)
{
    int depth = *(int *)ctx;
    char kb[256] = "?";
    if (CFGetTypeID(k) == CFStringGetTypeID()) CFStringGetCString(k, kb, sizeof kb, kCFStringEncodingUTF8);
    printf("%*s%s: ", depth * 2, "", kb);
    print_cf(v, depth + 1);
}
static void print_cf(CFTypeRef v, int depth)
{
    if (!v) { printf("(null)\n"); return; }
    CFTypeID t = CFGetTypeID(v);
    if (t == CFStringGetTypeID()) { char b[512]; CFStringGetCString(v, b, sizeof b, kCFStringEncodingUTF8); printf("%s\n", b); }
    else if (t == CFNumberGetTypeID()) { double d; CFNumberGetValue(v, kCFNumberDoubleType, &d); printf("%g\n", d); }
    else if (t == CFBooleanGetTypeID()) printf("%s\n", CFBooleanGetValue(v) ? "true" : "false");
    else if (t == CFDictionaryGetTypeID()) { printf("\n"); int d = depth; CFDictionaryApplyFunction(v, print_dict_entry, &d); }
    else if (t == CFArrayGetTypeID()) { CFIndex n = CFArrayGetCount(v); printf("[%ld]\n", (long)n); for (CFIndex i = 0; i < n; i++) { printf("%*s- ", depth * 2, ""); print_cf(CFArrayGetValueAtIndex(v, i), depth + 1); } }
    else { CFStringRef d = CFCopyDescription(v); char b[512]; CFStringGetCString(d, b, sizeof b, kCFStringEncodingUTF8); CFRelease(d); printf("%s\n", b); }
}

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 1920, h = argc > 2 ? atoi(argv[2]) : 1080;
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
    VTCompressionSessionRef s = NULL;
    OSStatus st = VTCompressionSessionCreate(NULL, w, h, kCMVideoCodecType_H264, spec, NULL, NULL, cb, NULL, &s);
    CFRelease(spec);
    printf("hardware-only H.264 session at %dx%d: %s (status %d)\n", w, h, st == noErr && s ? "opened" : "REFUSED", (int)st);
    if (st != noErr || !s) return 1;
    CFBooleanRef hw = NULL;
    if (VTSessionCopyProperty(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, NULL, &hw) == noErr && hw) {
        printf("UsingHardwareAcceleratedVideoEncoder: %s\n", CFBooleanGetValue(hw) ? "true" : "false"); CFRelease(hw);
    } else printf("UsingHardwareAcceleratedVideoEncoder: (not reported)\n");
    CFDictionaryRef sup = NULL;
    if (VTSessionCopySupportedPropertyDictionary(s, &sup) == noErr && sup) {
        printf("supported properties (%ld):\n", (long)CFDictionaryGetCount(sup));
        int d = 1; CFDictionaryApplyFunction(sup, print_dict_entry, &d);
        CFRelease(sup);
    } else printf("supported property dictionary: (unavailable)\n");
    VTCompressionSessionInvalidate(s); CFRelease(s);
    return 0;
}
