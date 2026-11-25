// quesa.cpp - a test of Quesa (QuickDraw 3D over OpenGL) on the classic
// Mac OS for PowerPC, through the QuickDraw 3D header names. It starts
// Quesa, makes a box, gets the bounds of the box through a view, renders
// the box with the interactive renderer into a pixmap, and checks the
// clear colour in the pixmap. In the PowerPC simulator, the fake OpenGL
// (tools/fakegl.py) does the clear. extras/quesa/build.sh makes the
// library; tests/run.sh builds and runs this test when QUESA_SRC is set.
#include <QD3D.h>
#include <QD3DCamera.h>
#include <QD3DDrawContext.h>
#include <QD3DGeometry.h>
#include <QD3DRenderer.h>
#include <QD3DView.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

// Prints a failure and counts it when ok is false.
static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAILED: %s\n", what);
    failures++;
  }
}

// Gives true when a and b differ by less than 0.001.
static bool near(float a, float b) { return a - b < 0.001f && b - a < 0.001f; }

enum { kW = 16, kH = 8 };
static TQ3Uns32 pixels[kW * kH];

// Makes a view with a camera, the interactive renderer and a pixmap
// draw context that clears to red.
static TQ3ViewObject make_view() {
  TQ3ViewObject view = Q3View_New();
  check(view != NULL, "Q3View_New");
  if (!view) return NULL;

  TQ3PixmapDrawContextData pd;
  memset(&pd, 0, sizeof pd);
  pd.drawContextData.clearImageMethod = kQ3ClearMethodWithColor;
  pd.drawContextData.clearImageColor.a = 1.0f;
  pd.drawContextData.clearImageColor.r = 1.0f;
  pd.drawContextData.paneState = kQ3False;
  pd.drawContextData.maskState = kQ3False;
  pd.drawContextData.doubleBufferState = kQ3False;
  pd.pixmap.image = pixels;
  pd.pixmap.width = kW;
  pd.pixmap.height = kH;
  pd.pixmap.rowBytes = kW * 4;
  pd.pixmap.pixelSize = 32;
  pd.pixmap.pixelType = kQ3PixelTypeARGB32;
  pd.pixmap.bitOrder = kQ3EndianBig;
  pd.pixmap.byteOrder = kQ3EndianBig;
  TQ3DrawContextObject dc = Q3PixmapDrawContext_New(&pd);
  check(dc != NULL, "Q3PixmapDrawContext_New");
  if (dc) {
    check(Q3View_SetDrawContext(view, dc) == kQ3Success, "Q3View_SetDrawContext");
    Q3Object_Dispose(dc);
  }

  TQ3ViewAngleAspectCameraData cd;
  memset(&cd, 0, sizeof cd);
  cd.cameraData.placement.cameraLocation.z = 5.0f;
  cd.cameraData.placement.upVector.y = 1.0f;
  cd.cameraData.range.hither = 0.1f;
  cd.cameraData.range.yon = 100.0f;
  cd.cameraData.viewPort.origin.x = -1.0f;
  cd.cameraData.viewPort.origin.y = 1.0f;
  cd.cameraData.viewPort.width = 2.0f;
  cd.cameraData.viewPort.height = 2.0f;
  cd.fov = 1.0f;
  cd.aspectRatioXToY = (float)kW / kH;
  TQ3CameraObject cam = Q3ViewAngleAspectCamera_New(&cd);
  check(cam != NULL, "Q3ViewAngleAspectCamera_New");
  if (cam) {
    Q3View_SetCamera(view, cam);
    Q3Object_Dispose(cam);
  }

  TQ3RendererObject r = Q3Renderer_NewFromType(kQ3RendererTypeInteractive);
  check(r != NULL, "Q3Renderer_NewFromType(interactive)");
  if (r) {
    check(Q3View_SetRenderer(view, r) == kQ3Success, "Q3View_SetRenderer");
    Q3Object_Dispose(r);
  }
  return view;
}

// Runs the tests and prints "all ok" when they pass.
int main() {
  check(Q3Initialize() == kQ3Success, "Q3Initialize");
  check(Q3IsInitialized() == kQ3True, "Q3IsInitialized");

  TQ3Uns32 major = 0, minor = 0;
  check(Q3GetVersion(&major, &minor) == kQ3Success && major >= 1, "Q3GetVersion");

  TQ3BoxData bd;
  memset(&bd, 0, sizeof bd);
  bd.orientation.x = 1.0f;
  bd.majorAxis.y = 1.0f;
  bd.minorAxis.z = 1.0f;
  TQ3GeometryObject box = Q3Box_New(&bd);
  check(box != NULL, "Q3Box_New");
  check(box && Q3Object_IsType(box, kQ3GeometryTypeBox), "box is a box");

  TQ3ViewObject view = make_view();
  if (view && box) {
    // The bounds of the box, through the view.
    TQ3BoundingBox bb;
    memset(&bb, 0, sizeof bb);
    check(Q3View_StartBoundingBox(view, kQ3ComputeBoundsExact) == kQ3Success,
          "Q3View_StartBoundingBox");
    do {
      Q3Geometry_Submit(box, view);
    } while (Q3View_EndBoundingBox(view, &bb) == kQ3ViewStatusRetraverse);
    check(near(bb.min.x, 0) && near(bb.min.y, 0) && near(bb.min.z, 0), "bounds min");
    check(near(bb.max.x, 1) && near(bb.max.y, 1) && near(bb.max.z, 1), "bounds max");
    check(bb.isEmpty == kQ3False, "bounds not empty");

    // A frame with the interactive (OpenGL) renderer.
    TQ3ViewStatus st;
    check(Q3View_StartRendering(view) == kQ3Success, "Q3View_StartRendering");
    do {
      Q3Geometry_Submit(box, view);
      st = Q3View_EndRendering(view);
    } while (st == kQ3ViewStatusRetraverse);
    check(st == kQ3ViewStatusDone, "Q3View_EndRendering");
    check(pixels[0] == 0xFFFF0000u, "red at the top left");
    check(pixels[kW * kH - 1] == 0xFFFF0000u, "red at the bottom right");
    if (pixels[0] != 0xFFFF0000u) printf("pixel 0 is %08lx\n", (unsigned long)pixels[0]);
  }
  if (view) Q3Object_Dispose(view);
  if (box) Q3Object_Dispose(box);

  check(Q3Exit() == kQ3Success, "Q3Exit");
  if (failures == 0) printf("all ok\n");
  return failures ? 1 : 0;
}
