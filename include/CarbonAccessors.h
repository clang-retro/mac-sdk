/* CarbonAccessors.h - the Carbon accessor functions for programs that are
 * not Carbon programs.
 *
 * Carbon hides the Toolbox records. A Carbon program uses functions such
 * as GetPortBounds and GetWindowFromPort to read them. Apple gave the
 * same functions to classic programs in CarbonAccessors.o. This header
 * gives them as inline functions that read the records directly, so that
 * code written for Carbon also compiles for the classic Mac OS.
 *
 * When TARGET_API_MAC_CARBON is not zero, this header gives nothing: the
 * Toolbox headers declare the real functions.
 */
#ifndef __CARBONACCESSORS__
#define __CARBONACCESSORS__

#include "Multiverse.h"

#if !TARGET_API_MAC_CARBON

/* The QuickDraw globals (qd) are below the address in *CurrentA5:
 * *CurrentA5 points at qd.thePort. These are the offsets of the other
 * fields below thePort. */
#define CARBONACCESSORS_QD_RANDSEED 126
#define CARBONACCESSORS_QD_SCREENBITS 122
#define CARBONACCESSORS_QD_ARROW 108
#define CARBONACCESSORS_QD_DKGRAY 40
#define CARBONACCESSORS_QD_LTGRAY 32
#define CARBONACCESSORS_QD_GRAY 24
#define CARBONACCESSORS_QD_BLACK 16
#define CARBONACCESSORS_QD_WHITE 8

/* Gives the address of the field that is off bytes below qd.thePort. */
static __inline__ char *CarbonAccessorsQD(long off) {
    return **(char ***)0x904 - off;
}

/* ---- QuickDraw ports ---- */

/* Gives the bounds (portRect) of a port. */
static __inline__ Rect *GetPortBounds(CGrafPtr p, Rect *r) {
    *r = ((GrafPtr)p)->portRect;
    return r;
}

/* Copies the visible region of a port into rgn, and gives rgn. */
static __inline__ RgnHandle GetPortVisibleRegion(CGrafPtr p, RgnHandle rgn) {
    CopyRgn(((GrafPtr)p)->visRgn, rgn);
    return rgn;
}

/* Copies the clip region of a port into rgn, and gives rgn. */
static __inline__ RgnHandle GetPortClipRegion(CGrafPtr p, RgnHandle rgn) {
    CopyRgn(((GrafPtr)p)->clipRgn, rgn);
    return rgn;
}

/* Gives the bit map of a port, to use as an argument of CopyBits. For a
 * color port, this is the port's pixel map handle in the same place. */
static __inline__ const BitMap *GetPortBitMapForCopyBits(CGrafPtr p) {
    return &((GrafPtr)p)->portBits;
}

/* Gives the pixel map of a color port. */
static __inline__ PixMapHandle GetPortPixMap(CGrafPtr p) {
    return p->portPixMap;
}

/* Gives true if the port is a color port. */
static __inline__ Boolean IsPortColor(CGrafPtr p) {
    return (p->portVersion & 0xC000) == 0xC000;
}

/* Gives the font of a port. */
static __inline__ short GetPortTextFont(CGrafPtr p) {
    return ((GrafPtr)p)->txFont;
}

/* Gives the font size of a port. */
static __inline__ short GetPortTextSize(CGrafPtr p) {
    return ((GrafPtr)p)->txSize;
}

/* Gives the text style of a port. */
static __inline__ Style GetPortTextFace(CGrafPtr p) {
    return ((GrafPtr)p)->txFace;
}

/* Gives the text transfer mode of a port. */
static __inline__ short GetPortTextMode(CGrafPtr p) {
    return ((GrafPtr)p)->txMode;
}

/* Gives the pen size of a port in *pt, and gives pt. */
static __inline__ Point *GetPortPenSize(CGrafPtr p, Point *pt) {
    *pt = ((GrafPtr)p)->pnSize;
    return pt;
}

/* Gives the pen location of a port in *pt, and gives pt. */
static __inline__ Point *GetPortPenLocation(CGrafPtr p, Point *pt) {
    *pt = ((GrafPtr)p)->pnLoc;
    return pt;
}

/* ---- QuickDraw globals ---- */

/* Gives the current port (qd.thePort). */
static __inline__ CGrafPtr GetQDGlobalsThePort(void) {
    return *(CGrafPtr *)CarbonAccessorsQD(0);
}

/* Gives the screen bit map (qd.screenBits) in *bm, and gives bm. */
static __inline__ BitMap *GetQDGlobalsScreenBits(BitMap *bm) {
    *bm = *(BitMap *)CarbonAccessorsQD(CARBONACCESSORS_QD_SCREENBITS);
    return bm;
}

/* Gives the arrow cursor (qd.arrow) in *c, and gives c. */
static __inline__ Cursor *GetQDGlobalsArrow(Cursor *c) {
    *c = *(Cursor *)CarbonAccessorsQD(CARBONACCESSORS_QD_ARROW);
    return c;
}

/* Gives the dark gray pattern (qd.dkGray) in *pat, and gives pat. */
static __inline__ Pattern *GetQDGlobalsDarkGray(Pattern *pat) {
    *pat = *(Pattern *)CarbonAccessorsQD(CARBONACCESSORS_QD_DKGRAY);
    return pat;
}

/* Gives the light gray pattern (qd.ltGray) in *pat, and gives pat. */
static __inline__ Pattern *GetQDGlobalsLightGray(Pattern *pat) {
    *pat = *(Pattern *)CarbonAccessorsQD(CARBONACCESSORS_QD_LTGRAY);
    return pat;
}

/* Gives the gray pattern (qd.gray) in *pat, and gives pat. */
static __inline__ Pattern *GetQDGlobalsGray(Pattern *pat) {
    *pat = *(Pattern *)CarbonAccessorsQD(CARBONACCESSORS_QD_GRAY);
    return pat;
}

/* Gives the black pattern (qd.black) in *pat, and gives pat. */
static __inline__ Pattern *GetQDGlobalsBlack(Pattern *pat) {
    *pat = *(Pattern *)CarbonAccessorsQD(CARBONACCESSORS_QD_BLACK);
    return pat;
}

/* Gives the white pattern (qd.white) in *pat, and gives pat. */
static __inline__ Pattern *GetQDGlobalsWhite(Pattern *pat) {
    *pat = *(Pattern *)CarbonAccessorsQD(CARBONACCESSORS_QD_WHITE);
    return pat;
}

/* Gives the seed of Random (qd.randSeed). */
static __inline__ long GetQDGlobalsRandomSeed(void) {
    return *(long *)CarbonAccessorsQD(CARBONACCESSORS_QD_RANDSEED);
}

/* Sets the seed of Random (qd.randSeed). */
static __inline__ void SetQDGlobalsRandomSeed(long seed) {
    *(long *)CarbonAccessorsQD(CARBONACCESSORS_QD_RANDSEED) = seed;
}

/* ---- Windows ---- */

/* Gives the port of a window. A window record starts with its port. */
static __inline__ CGrafPtr GetWindowPort(WindowRef w) {
    return (CGrafPtr)w;
}

/* Gives the window of a window's port. */
static __inline__ WindowRef GetWindowFromPort(CGrafPtr p) {
    return (WindowRef)p;
}

/* Sets the kind of a window (windowKind). */
static __inline__ void SetWindowKind(WindowRef w, short kind) {
    ((WindowPeek)w)->windowKind = kind;
}

/* Gives true if the window is highlighted. */
static __inline__ Boolean IsWindowHilited(WindowRef w) {
    return ((WindowPeek)w)->hilited != 0;
}

/* Gives true if the window is visible. */
static __inline__ Boolean IsWindowVisible(WindowRef w) {
    return ((WindowPeek)w)->visible != 0;
}

/* Gives the frontmost window in the window list (WindowList). */
static __inline__ WindowRef GetWindowList(void) {
    return (WindowRef)LMGetWindowList();
}

/* Gives the window after w in the window list. */
static __inline__ WindowRef GetNextWindow(WindowRef w) {
    return (WindowRef)((WindowPeek)w)->nextWindow;
}

/* ---- Menus ---- */

/* Gives the ID of a menu. */
static __inline__ short GetMenuID(MenuRef m) {
    return (**m).menuID;
}

/* Sets the ID of a menu. */
static __inline__ void SetMenuID(MenuRef m, short id) {
    (**m).menuID = id;
}

/* Gives the width of a menu. */
static __inline__ short GetMenuWidth(MenuRef m) {
    return (**m).menuWidth;
}

/* Gives the height of a menu. */
static __inline__ short GetMenuHeight(MenuRef m) {
    return (**m).menuHeight;
}

/* Gives the number of items in a menu. */
static __inline__ short CountMenuItems(MenuRef m) {
    return CountMItems(m);
}

/* ---- Dialogs ---- */

/* Gives the window of a dialog. A dialog record starts with its window. */
static __inline__ WindowRef GetDialogWindow(DialogRef d) {
    return (WindowRef)d;
}

/* Gives the port of a dialog. */
static __inline__ CGrafPtr GetDialogPort(DialogRef d) {
    return (CGrafPtr)d;
}

/* Gives the dialog of a dialog's window. */
static __inline__ DialogRef GetDialogFromWindow(WindowRef w) {
    return (DialogRef)w;
}

/* Gives the TextEdit record of a dialog. */
static __inline__ TEHandle GetDialogTextEditHandle(DialogRef d) {
    return ((DialogPeek)d)->textH;
}

/* Gives the item that has the keyboard focus (editField + 1). */
static __inline__ short GetDialogKeyboardFocusItem(DialogRef d) {
    return ((DialogPeek)d)->editField + 1;
}

/* ---- Controls ---- */

/* Gives the bounds of a control in *r, and gives r. */
static __inline__ Rect *GetControlBounds(ControlHandle c, Rect *r) {
    *r = (**c).contrlRect;
    return r;
}

/* Sets the bounds of a control. It does not draw the control again. */
static __inline__ void SetControlBounds(ControlHandle c, const Rect *r) {
    (**c).contrlRect = *r;
}

/* Gives the window of a control. */
static __inline__ WindowRef GetControlOwner(ControlHandle c) {
    return (WindowRef)(**c).contrlOwner;
}

/* Gives the highlight state of a control (contrlHilite). */
static __inline__ unsigned short GetControlHilite(ControlHandle c) {
    return (**c).contrlHilite;
}

/* Gives true if a control is highlighted (a part, not 0 or 255). */
static __inline__ Boolean IsControlHilited(ControlHandle c) {
    return (**c).contrlHilite != 0 && (**c).contrlHilite != 255;
}

#endif /* !TARGET_API_MAC_CARBON */

#endif /* __CARBONACCESSORS__ */
