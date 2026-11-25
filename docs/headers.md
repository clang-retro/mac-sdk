# The Toolbox headers

<!-- include README.md "Headers" only -->

## Old routine names

<!-- include README.md "Old routine names" -->

## Where they come from

<!-- include README.md "Where they come from" -->

## Checked against a real ROM

<!-- include README.md "Checked against a real ROM" -->

## C++

The Toolbox headers declare the Toolbox calls with C linkage, also in
C++. A C++ program can include them without `extern "C"`.

## Carbon accessors

Carbon programs read the Toolbox records with accessor functions, such
as `GetPortBounds` and `GetWindowFromPort`. The Toolbox headers declare
these functions only when `TARGET_API_MAC_CARBON` is 1. Apple gave the
same functions to programs that are not Carbon programs in
CarbonAccessors.o. `CarbonAccessors.h` gives them as inline functions
that read the records:

| Area | Functions |
| --- | --- |
| Ports | `GetPortBounds`, `GetPortVisibleRegion`, `GetPortClipRegion`, `GetPortBitMapForCopyBits`, `GetPortPixMap`, `IsPortColor`, `GetPortTextFont`, `GetPortTextSize`, `GetPortTextFace`, `GetPortTextMode`, `GetPortPenSize`, `GetPortPenLocation` |
| QuickDraw globals | `GetQDGlobalsThePort`, `GetQDGlobalsScreenBits`, `GetQDGlobalsArrow`, `GetQDGlobalsDarkGray`, `GetQDGlobalsLightGray`, `GetQDGlobalsGray`, `GetQDGlobalsBlack`, `GetQDGlobalsWhite`, `GetQDGlobalsRandomSeed`, `SetQDGlobalsRandomSeed` |
| Windows | `GetWindowPort`, `GetWindowFromPort`, `SetWindowKind`, `IsWindowHilited`, `IsWindowVisible`, `GetWindowList`, `GetNextWindow` |
| Menus | `GetMenuID`, `SetMenuID`, `GetMenuWidth`, `GetMenuHeight`, `CountMenuItems` |
| Dialogs | `GetDialogWindow`, `GetDialogPort`, `GetDialogFromWindow`, `GetDialogTextEditHandle`, `GetDialogKeyboardFocusItem` |
| Controls | `GetControlBounds`, `SetControlBounds`, `GetControlOwner`, `GetControlHilite`, `IsControlHilited` |

When `TARGET_API_MAC_CARBON` is 1, `CarbonAccessors.h` gives nothing.
The QuickDraw globals functions read the globals below `*CurrentA5`, so
call them after `InitGraf`.
