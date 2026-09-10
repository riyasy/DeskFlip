// The clock face: device, baked bitmaps, flip state, and the frame. Everything Direct2D
// lives here; main.cpp owns only the window and the message loop.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// The round resize button, centred on the case's bottom-right corner while Resize mode is on.
// Screen pixels, deliberately not scaled with the clock: a grip has to stay grabbable when the
// clock is tiny. main.cpp hit-tests GripRect(), so it never has to know these numbers.
constexpr float GRIP_R = 13.f;       // the button's radius
constexpr float GRIP_SHADOW = 5.f;   // how far its soft shadow reaches past the rim

struct Card {
    int cur = 0;
    int next = 0;
    bool flipping = false;
    double startTime = 0.0;
};

// Every colour the clock draws, so a theme is a data swap rather than a parallel drawing path.
// Defined in clockwindow.cpp -- nothing outside needs its fields, only which one is active.
struct Palette;

class Clock {
public:
    bool Init(HWND hwnd, bool showSeconds, bool useWarp, bool lightMode);
    void Resize();
    void SetShowSeconds(bool on);   // drops the last card pair and one colon; the case narrows
    void SetLightMode(bool on);     // swaps the palette and rebakes; geometry is untouched
    bool Animating() const;
    bool Tick(double now);          // advance state; start flips on a second boundary. true = something moved
    void Render(double now);
    void Shutdown();

    // Resize mode: the clock draws a grip in its bottom-right corner and main.cpp drags it.
    // Moving needs no mode and no drawing at all -- WM_NCHITTEST answers HTCAPTION over the case,
    // so the window manager's own move loop does it (see main.cpp).
    void SetResizeMode(bool on) { resizeMode_ = on; }
    D2D1_RECT_F GripRect() const;               // the grip's hit/draw box, window-client px
    float ClampScale(float s) const;            // sane bounds for a live drag-resize
    float Scale() const { return scale_; }
    D2D1_RECT_F CaseRect() const;               // the case (no shadow slack), window-client px
    float CaseW() const;                        // unscaled CSS px, for the drag math
    float CaseH() const;

    // The OS window's size/position math lives here, not in main.cpp, so it can never drift
    // from what Resize() derives back out of the window's actual client size.
    static RECT WindowRectFor(bool showSeconds, float scalePct, int x, int y);  // scalePct<=0 => automatic; x/y<0 => centred
    static float PadFor(float scale);            // whole-px shadow margin around the case

private:
    bool CreateDeviceResources();
    bool CreateSizeResources();
    void ReleaseSizeResources();
    void DrawResizeGrip();

    ComPtr<ID2D1Bitmap1> NewTarget(float w, float h);
    void BakeChrome();
    void DrawHinges(const D2D1_MATRIX_3X2_F& origin);
    bool CreateDrawResources();
    bool LoadFont();
    void BakeDigit(int d, ID2D1Bitmap1* glyph, ID2D1Bitmap1* full, ID2D1Effect* shadow);
    void BakeFace(ID2D1Bitmap1* target, ID2D1Bitmap1* digit, bool top);
    void DrawFlap(int i, const Card& c, float eased);
    void DrawColons();

    ComPtr<ID2D1Geometry> HalfGeometry(bool top);

    HWND hwnd_ = nullptr;
    HANDLE frameWait_ = nullptr;
    bool occluded_ = false;
    bool warp_ = false;  // Init() always assigns it; false (hardware) is the ini's default
    const Palette* pal_ = nullptr;  // the active theme; set by Init, swapped by SetLightMode

    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> d3dCtx_;
    ComPtr<IDXGIDevice3> dxgiDev_;
    ComPtr<IDXGISwapChain2> swap_;

    // The swapchain is composed, not bound to the HWND's redirection surface -- that is what
    // buys a genuinely transparent background instead of a coloured one. Same trick as
    // let-it-rain: DComp visual holds the swapchain, the window is just a hole in the desktop.
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> dcompVisual_;

    ComPtr<ID2D1Device> d2dDev_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> backBuffer_;
    ComPtr<IDWriteFactory5> dw_;
    ComPtr<IDWriteTextFormat> fmt_;

    // The font collection is device-level, not size-level. Rebuilding a custom
    // IDWriteFontCollection on every WM_SIZE piles up DWrite's internal font caches
    // faster than they are reclaimed -- that is most of the "RAM balloons while I drag
    // the corner, then settles minutes later". Only the text format is size-dependent.
    ComPtr<IDWriteFontCollection1> fontColl_;
    const wchar_t* fontFamily_ = L"Cascadia Mono";  // fallback: present on stock Windows
    DWRITE_FONT_WEIGHT fontWeight_ = DWRITE_FONT_WEIGHT_BOLD;

    // Baked once per size.
    ComPtr<ID2D1Bitmap1> bmpChrome_;     // case + slots + card backs, incl. CASE_PAD slack
    ComPtr<ID2D1Bitmap1> bmpTop_[10];    // top half of each digit's card face
    ComPtr<ID2D1Bitmap1> bmpBot_[10];    // bottom half
    ComPtr<ID2D1Bitmap1> bmpFlap_[6];    // per-card flap scratch: face + its shadow, pre-rotation

    // One perspective effect per card. Reusing a single effect across six DrawImage
    // calls in one batch would mean betting on when D2D snapshots properties; six
    // effects cost nothing and remove the bet.
    ComPtr<ID2D1Effect> flapFx_[6];

    ComPtr<ID2D1Geometry> geomTop_, geomBot_;  // unit scale, built once

    // Brushes and geometries for the animated pass. Rebuilding these per frame is the kind
    // of quiet CPU churn this whole port exists to avoid, so they are built once per size and
    // only ever mutated (SetOpacity / SetColor / SetCenter) at draw time.
    ComPtr<ID2D1Geometry> geomTopS_, geomBotS_;  // scaled to the current size
    ComPtr<ID2D1SolidColorBrush> brush_;         // the hinges; recoloured, never rebuilt
    ComPtr<ID2D1LinearGradientBrush> shFront_, shBack_, shBottom_;
    ComPtr<ID2D1RadialGradientBrush> gripShadow_;  // the resize button's soft shadow, re-centred
    ComPtr<ID2D1StrokeStyle> gripStroke_;          // round caps for its arrow

    Card cards_[6];
    int lastSecond_ = -1;

    // Hiding the seconds is a geometry change, not a draw-time skip: the case itself loses a
    // group and a colon, so anything derived from the case width is derived from groups_.
    int groups_ = 3;    // hours, minutes, seconds
    int cardCount_ = 6; // 2 per group
    float caseW_ = 0.f; // CaseW(groups_), in CSS px

    bool resizeMode_ = false;
    float scale_ = 1.f;

    // A DrawBitmap is a 1:1 texel copy only if it lands on a whole pixel; at a fractional
    // offset the GPU bilinear-resamples the entire bitmap, which costs sampling work and
    // softens the glyphs. Everything that ends up as a *draw offset* is therefore snapped
    // once, here, so the chrome, the faces and the flaps all agree on the same grid.
    // (Sizes need no snapping -- only positions do -- and the vector geometry inside a
    // baked bitmap can stay fractional, since it is rasterised, not sampled.)
    float pad_ = 0.f;   // CASE_PAD * scale_, whole px
    float half_ = 0.f;  // HALF_H  * scale_, whole px: the top face's height *and* the
                        // bottom face's y-offset, so they must be the same integer.
    UINT winW_ = 0, winH_ = 0;
};

#ifdef _DEBUG
void SelfTest();  // the easing curve and the digit split; the only real math in the program
#endif
