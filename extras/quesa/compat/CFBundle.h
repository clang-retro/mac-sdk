/* CFBundle.h - the Core Foundation types that Quesa names in its
 * prototypes. The classic Mac OS has no Core Foundation without
 * CarbonLib; Quesa calls it only in Carbon builds. */
#ifndef QUESA_COMPAT_CFBUNDLE_H
#define QUESA_COMPAT_CFBUNDLE_H
typedef const struct __CFString *CFStringRef;
typedef struct __CFBundle *CFBundleRef;
typedef const struct __CFURL *CFURLRef;
typedef const struct __CFArray *CFArrayRef;
#endif
