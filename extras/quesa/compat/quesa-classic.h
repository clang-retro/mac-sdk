/* quesa-classic.h - included before each Quesa source in the build for
 * the classic Mac OS with clang and mac-sdk. It sets what Quesa's
 * CodeWarrior prefix files set, and gives the few names that Apple's
 * Universal Interfaces had and mac-sdk does not. */
#ifndef QUESA_CLASSIC_H
#define QUESA_CLASSIC_H
#define QUESA_OS_MACINTOSH 1
#define Q3_EXPORT_SYMBOLS 1
#define Q3_DEBUG 0
#define Q3_PROFILE 0
#define ACCESSOR_CALLS_ARE_FUNCTIONS 1
/* The error that the old Viewer calls give for a failure. QD3D did not
 * document one; this is a stand-in. */
#ifndef kGeneralFailureErr
#define kGeneralFailureErr (-1)
#endif
/* Quesa uses ULONG_MAX and others without <limits.h>; CodeWarrior's
 * prefix gave them. */
#include <limits.h>
/* Quesa uses the Carbon accessor functions also in its classic build. */
#include <CarbonAccessors.h>
#endif
