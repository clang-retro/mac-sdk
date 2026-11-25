/* aglRenderers.h - the renderer IDs of AGL, for AGL_RENDERER_ID in
 * aglChoosePixelFormat and for aglDescribeRenderer. The values are those
 * of the same renderers in CGL (kCGLRendererGenericID and
 * kCGLRendererATIRage128ID).
 *
 * Part of mac-sdk.
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#ifndef _AGLRENDERERS_H
#define _AGLRENDERERS_H

/* Apple's software renderer. */
#define AGL_RENDERER_GENERIC_ID 0x00020200
/* The renderer of ATI's Rage 128 and later cards. */
#define AGL_RENDERER_ATI_ID 0x00021000

#endif /* _AGLRENDERERS_H */
