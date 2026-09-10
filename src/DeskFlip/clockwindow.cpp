// The clock face. Direct2D on D3D11.
//
// Port of html/index.html (clock face only: 6 cards, 2 colons, transparent background).
// Every geometry constant below is lifted verbatim from that file's CSS, and so is the dark
// Palette. The light Palette has no CSS counterpart yet, but it reuses this file's geometry
// exactly, so the two differ only in colour.
//
// The point of the native port is churn, so two rules govern the whole design:
//   1. Nothing is rasterized twice. Card faces, digits, glyph shadows and the case
//      chrome are baked into GPU bitmaps once per size. A frame is then a handful of
//      textured blits and a perspective pass per flipping card.
//   2. Nothing renders unless it moves. No requestAnimationFrame-style spin loop:
//      idle blocks in MsgWaitForMultipleObjects (see main.cpp), and an occluded window
//      renders nothing.

#include "clockwindow.h"

#include <d2d1effects.h>

#include <algorithm>
#include <cassert>
#include <cmath>

using std::max;
using std::min;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxguid.lib")  // CLSID_D2D1Shadow, CLSID_D2D13DPerspectiveTransform

// ---------------------------------------------------------------------------
// Geometry, straight from the CSS. Units are CSS px; everything is multiplied by
// a single `scale` derived from the window size.
// ---------------------------------------------------------------------------
constexpr float CARD_W = 130.f, CARD_H = 200.f, HALF_H = 100.f;
constexpr float FACE_R = 10.f, CARD_R = 12.f;
constexpr float SLOT_PAD = 4.f, SLOT_R = 14.f;
constexpr float SLOT_W = CARD_W + 2 * SLOT_PAD;   // 138
constexpr float SLOT_H = CARD_H + 2 * SLOT_PAD;   // 208
constexpr float DIGIT_GAP = 8.f, GROUP_GAP = 20.f;
constexpr float COLON_W = 24.f, COLON_DOT = 12.f, COLON_GAP = 28.f;
constexpr float CASE_PAD_X = 40.f, CASE_PAD_Y = 36.f, CASE_R = 28.f;
constexpr float FONT_SIZE = 160.f, LETTER_SPACING = -0.05f * FONT_SIZE;

constexpr float CONTENT_H = SLOT_H;                          // 208
constexpr float CASE_H = CONTENT_H + 2 * CASE_PAD_Y;         // 280
// The case's drop shadow (`box-shadow: 0 40px 80px -15px`) is baked into the chrome bitmap, so
// the bitmap needs slack around the case -- otherwise the gaussian is sliced off square at the
// bitmap's edge, which is what a too-small CASE_PAD looks like. A gaussian is done at ~3 sigma,
// so the reach is SHADOW_DY + 3*SHADOW_SIGMA - SHADOW_SPREAD, and CASE_PAD must cover it.
//
// The CSS blur of 80px means sigma 40, reaching ~145px: an enormous diffuse cloud once the
// backdrop is the desktop rather than a dark page. Deliberately halved here.
constexpr float SHADOW_SIGMA = 20.f;   // CSS blur radius ~= 2 sigma, so this is a 40px blur
constexpr float SHADOW_DY = 20.f;      // CSS `0 40px`, halved along with the blur
constexpr float SHADOW_SPREAD = 15.f;  // CSS `-15px`: shrink the silhouette before blurring
constexpr float CASE_PAD = 90.f;       // >= SHADOW_DY + 3*SHADOW_SIGMA - SHADOW_SPREAD (= 65)

// A group is one digit pair; between two groups sits a colon with a gap either side. Three
// groups (HH:MM:SS) is the 980px content box the CSS lays out; two (HH:MM) is the same
// formula with one group and one colon removed.
static float ContentW(int groups) {
    return groups * (2 * SLOT_W + DIGIT_GAP) + (groups - 1) * (COLON_W + 2 * GROUP_GAP);
}
static float CaseWidthFor(int groups) { return ContentW(groups) + 2 * CASE_PAD_X; }  // 1060 at 3 groups

constexpr float PERSPECTIVE = 1000.f;  // CSS `perspective: 1000px`
constexpr float FLIP_SECS = 0.250f;    // CSS is `450ms`; deliberately snappier, and it halves the duty cycle

// The CSS `rotateX(3deg)` on .clock-outer-case is deliberately NOT ported. Tilting the case means
// compositing it offscreen first so the perspective effect has an input, and that scratch bitmap
// scales with window area -- it was the single largest allocation in the program. The clock now
// draws straight to the backbuffer. Don't re-add the tilt without re-reading the memory numbers.

// The CSS colon dots pulse forever (`pulse-glow 2.5s infinite alternate`). That is deliberately
// NOT ported: an animation that never ends means the process can never idle, and it would cost
// ~40% more CPU to make two dots breathe. The dots are baked static at the `.colon-dot` base
// style, so the only thing that ever moves here is a flip.

static D2D1_COLOR_F Rgb(UINT32 hex, float a = 1.f) {
    return D2D1::ColorF(hex, a);
}

// ---------------------------------------------------------------------------
// The palette. Every colour the clock draws lives in here and nowhere else, so a theme is a
// data swap rather than a second set of drawing code that would have to be kept in step with
// this one forever. Light mode therefore runs the *same* bake and the same frame path.
//
// Geometry is deliberately absent, including shadow offsets and blur radii: the two themes are
// pixel-identical apart from colour, so only alphas vary where the dark theme leaned on a
// near-opaque black. SHADOW_SIGMA/SHADOW_DY could not vary anyway -- CASE_PAD is derived from
// them, so touching either would change the window size.
// ---------------------------------------------------------------------------
struct Palette {
    UINT32 caseBg;                 // .clock-outer-case background
    float  caseShadowA;            //   its drop shadow's alpha
    UINT32 caseLip; float caseLipA;    //   inset 0 1px 0 -- lit lip along the top edge
    UINT32 slotBg;                 // .card-slot background
    UINT32 slotLip; float slotLipA;    //   0 1px 0 -- the slot's lower lip
    UINT32 cardBg;                 // .flip-card background (the sliver between the faces)
    UINT32 faceTopA, faceTopB;     // .static-top / .flap-front gradient
    UINT32 faceBotA, faceBotB;     // .static-bottom / .flap-back gradient
    UINT32 bevel; float bevelA0, bevelA1;   // .static-top::before, the 135deg plastic highlight
    UINT32 edgeDark; float edgeDarkA;       // .static-top border-bottom -- the cut at the hinge
    UINT32 edgeLight; float edgeLightA;     // .static-bottom border-top
    UINT32 hingeLine, hingePin;    // .hinge-line / .hinge-left + .hinge-right
    UINT32 ink;                    // .digit colour, and the colon dots
    UINT32 inkHi; float inkHiA;    //   text-shadow 0 1px 0 -- skipped entirely when alpha is 0
    float  inkShadowA;             //   text-shadow 0 4px 10px, alpha only
    float  colonGlowA;             // .colon-dot glow -- 0 means draw no glow at all
    UINT32 flipShadow;             // the three .shadow-overlay gradients
    float  flipStrongA, flipWeakA; //   .shadow-front / .shadow-back stops
    float  flipBottomA, flipBottomTailA;    //   .shadow-bottom stops
    UINT32 gripFill; float gripFillA;       // the resize button (ours, not from the CSS)
    UINT32 gripRing; float gripRingA;       //   its hairline rim
    UINT32 gripIcon; float gripIconA;       //   the diagonal arrow
    float  gripShadowA;                     //   its soft drop shadow, always black
};

// Dark: every value here is the one already in html/index.html's CSS, unchanged.
static constexpr Palette MakeDark() {
    Palette p{};
    p.caseBg = 0x080808;          p.caseShadowA = 0.95f;
    p.caseLip = 0xFFFFFF;         p.caseLipA = 0.04f;
    p.slotBg = 0x000000;
    p.slotLip = 0xFFFFFF;         p.slotLipA = 0.03f;
    p.cardBg = 0x181818;
    p.faceTopA = 0x2A2A2A;        p.faceTopB = 0x1A1A1A;
    p.faceBotA = 0x151515;        p.faceBotB = 0x1A1A1A;
    p.bevel = 0xFFFFFF;           p.bevelA0 = 0.05f;  p.bevelA1 = 0.01f;
    p.edgeDark = 0x000000;        p.edgeDarkA = 0.90f;
    p.edgeLight = 0xFFFFFF;       p.edgeLightA = 0.02f;
    p.hingeLine = 0x000000;       p.hingePin = 0x111111;
    p.ink = 0xE8E8E8;
    p.inkHi = 0xFFFFFF;           p.inkHiA = 0.10f;
    p.inkShadowA = 0.80f;
    p.colonGlowA = 0.35f;
    p.flipShadow = 0x000000;
    p.flipStrongA = 0.90f;        p.flipWeakA = 0.10f;
    p.flipBottomA = 0.85f;        p.flipBottomTailA = 0.20f;
    // The button stays light in both themes -- it is a control sitting on top of the clock, not
    // part of the case, and a dark one on a near-black case would be invisible.
    p.gripFill = 0xF4F7FB;        p.gripFillA = 0.97f;
    p.gripRing = 0x000000;        p.gripRingA = 0.18f;
    p.gripIcon = 0x44586E;        p.gripIconA = 1.00f;
    p.gripShadowA = 0.55f;
    return p;
}

// Light ("Soft"): cool light-grey cards, slate ink, every shadow about half strength. Two things
// are removed rather than inverted, because inverting them is what makes a naive light mode look
// wrong: the digit's white top highlight (relief only reads on a dark card) and the colon glow
// (a 20px bloom on a light card is a smudge, not a light).
static constexpr Palette MakeLight() {
    Palette p{};
    p.caseBg = 0xE7EAEE;          p.caseShadowA = 0.30f;
    p.caseLip = 0xFFFFFF;         p.caseLipA = 0.90f;
    // Darker than the design's #D3D8DF on purpose. The CSS gives .card-slot an
    // `inset 0 4px 12px` recess shadow that this port has never drawn -- dark mode does not
    // need it, since #000 against a #080808 case reads as a hole on its own. Light mode has far
    // less room, so the flat fill has to do the recess's work by itself.
    p.slotBg = 0xC2C9D2;
    p.slotLip = 0xFFFFFF;         p.slotLipA = 0.85f;
    p.cardBg = 0xEEF1F5;
    p.faceTopA = 0xFAFBFC;        p.faceTopB = 0xECF0F4;
    p.faceBotA = 0xE4E8EE;        p.faceBotB = 0xEDF0F4;
    p.bevel = 0xFFFFFF;           p.bevelA0 = 0.70f;  p.bevelA1 = 0.18f;
    p.edgeDark = 0x262C36;        p.edgeDarkA = 0.12f;
    p.edgeLight = 0xFFFFFF;       p.edgeLightA = 0.90f;
    p.hingeLine = 0xBCC3CC;       p.hingePin = 0xA6AEB9;
    p.ink = 0x333B47;
    p.inkHi = 0xFFFFFF;           p.inkHiA = 0.f;     // no relief highlight -- see above
    p.inkShadowA = 0.10f;
    p.colonGlowA = 0.f;                               // no glow -- see above
    p.flipShadow = 0x262C36;
    p.flipStrongA = 0.45f;        p.flipWeakA = 0.05f;
    p.flipBottomA = 0.42f;        p.flipBottomTailA = 0.08f;
    p.gripFill = 0xFFFFFF;        p.gripFillA = 0.98f;
    // Rim and shadow both run stronger than the dark theme's *relative* to their backdrop: here
    // the button is white on a near-white case, so they are the only things separating the two.
    p.gripRing = 0x262C36;        p.gripRingA = 0.22f;
    p.gripIcon = 0x53687E;        p.gripIconA = 1.00f;
    p.gripShadowA = 0.30f;   // a light desktop cannot carry the dark theme's near-opaque shadow
    return p;
}

constexpr Palette kDark = MakeDark();
constexpr Palette kLight = MakeLight();

// ---------------------------------------------------------------------------
// Easing. CSS transitions ease the *angle*, not time, and cubic-bezier is a
// parametric curve: you must solve x(t) = progress for t, then evaluate y(t).
// Getting this wrong yields a flip that looks plausible but isn't the CSS curve,
// which is exactly why it's the one thing with a self-test.
// ---------------------------------------------------------------------------
struct Bezier { float x1, y1, x2, y2; };

constexpr Bezier kFlipEase{ 0.25f, 1.f, 0.5f, 1.f };  // CSS cubic-bezier(.25,1,.5,1)

// One axis of a cubic Bezier with p0 = 0 and p3 = 1.
static float BezAxis(float a1, float a2, float t) {
    const float u = 1.f - t;
    return 3.f * u * u * t * a1 + 3.f * u * t * t * a2 + t * t * t;
}
static float BezAxisDeriv(float a1, float a2, float t) {
    const float u = 1.f - t;
    return 3.f * a1 * (u * u - 2.f * t * u) + 3.f * a2 * (2.f * t * u - t * t) + 3.f * t * t;
}

static float Ease(const Bezier& b, float x) {
    if (x <= 0.f) return 0.f;
    if (x >= 1.f) return 1.f;

    float t = x;
    for (int i = 0; i < 8; ++i) {
        const float err = BezAxis(b.x1, b.x2, t) - x;
        if (fabsf(err) < 1e-6f) break;
        const float d = BezAxisDeriv(b.x1, b.x2, t);
        if (fabsf(d) < 1e-6f) break;  // flat spot: Newton is useless here
        t = min(1.f, max(0.f, t - err / d));
    }
    if (fabsf(BezAxis(b.x1, b.x2, t) - x) > 1e-4f) {  // Newton stalled, bisect instead
        float lo = 0.f, hi = 1.f;
        for (int i = 0; i < 40; ++i) {
            t = 0.5f * (lo + hi);
            (BezAxis(b.x1, b.x2, t) < x ? lo : hi) = t;
        }
    }
    return BezAxis(b.y1, b.y2, t);
}

static void Digits(const SYSTEMTIME& st, int out[6]) {
    out[0] = st.wHour / 10;   out[1] = st.wHour % 10;
    out[2] = st.wMinute / 10; out[3] = st.wMinute % 10;
    out[4] = st.wSecond / 10; out[5] = st.wSecond % 10;
}

#ifdef _DEBUG
void SelfTest() {
    assert(Ease(kFlipEase, 0.f) == 0.f);
    assert(Ease(kFlipEase, 1.f) == 1.f);
    // cubic-bezier(.25,1,.5,1) is front-loaded: half the time is ~93% of the travel.
    assert(fabsf(Ease(kFlipEase, 0.5f) - 0.9341f) < 0.005f);
    float prev = -1.f;
    for (int i = 0; i <= 100; ++i) {  // strictly monotonic, or the flap stutters
        const float y = Ease(kFlipEase, i / 100.f);
        assert(y > prev);
        prev = y;
    }
    SYSTEMTIME st{}; int d[6];
    st.wHour = 23; st.wMinute = 59; st.wSecond = 59;
    Digits(st, d);
    assert(d[0] == 2 && d[1] == 3 && d[2] == 5 && d[3] == 9 && d[4] == 5 && d[5] == 9);
    st.wHour = st.wMinute = st.wSecond = 0;
    Digits(st, d);
    for (int i = 0; i < 6; ++i) assert(d[i] == 0);
}
#endif

// Card top-left within the case, in unscaled CSS px.
static void CardOrigin(int i, float* x, float* y) {
    const int group = i / 2;      // 0 = hours, 1 = minutes, 2 = seconds
    const int slot = i % 2;
    const float groupX = group * (2 * SLOT_W + DIGIT_GAP + COLON_W + 2 * GROUP_GAP);
    *x = CASE_PAD_X + groupX + slot * (SLOT_W + DIGIT_GAP) + SLOT_PAD;
    *y = CASE_PAD_Y + SLOT_PAD;
}

// The same, scaled into padded-case px and snapped to the pixel grid. Every place that
// positions a card -- the baked slot, the baked hinge, the drawn face, the flap -- goes
// through here, so they cannot drift apart by a subpixel.
static void CardPx(int i, float pad, float s, float* x, float* y) {
    float cx, cy;
    CardOrigin(i, &cx, &cy);
    *x = pad + floorf(cx * s);
    *y = pad + floorf(cy * s);
}

// Colon separator centre-x within the case, for group boundary g (0 or 1).
static float ColonX(int g) {
    const float groupW = 2 * SLOT_W + DIGIT_GAP;
    return CASE_PAD_X + (g + 1) * groupW + (2 * g + 1) * GROUP_GAP + g * COLON_W + COLON_W / 2;
}

bool Clock::Init(HWND hwnd, bool showSeconds, bool useWarp, bool lightMode) {
    hwnd_ = hwnd;
    warp_ = useWarp;
    pal_ = lightMode ? &kLight : &kDark;
    groups_ = showSeconds ? 3 : 2;
    cardCount_ = 2 * groups_;
    caseW_ = CaseWidthFor(groups_);
    if (!CreateDeviceResources()) return false;

    SYSTEMTIME st;
    GetLocalTime(&st);
    int d[6];
    Digits(st, d);
    for (int i = 0; i < 6; ++i) cards_[i].cur = cards_[i].next = d[i];
    lastSecond_ = st.wSecond;

    Resize();
    return dc_ != nullptr;
}

// The case gets narrower/wider, so every baked bitmap is wrong -- but this only updates the
// numbers. main.cpp resizes the actual window to match (the window IS the case, padded), and
// that WM_SIZE is what triggers Resize()'s rebake; doing it here too would just bake once at
// the stale window size and again once the window catches up.
void Clock::SetShowSeconds(bool on) {
    const int groups = on ? 3 : 2;
    if (groups == groups_) return;
    groups_ = groups;
    cardCount_ = 2 * groups;
    caseW_ = CaseWidthFor(groups);
    for (int i = cardCount_; i < 6; ++i) cards_[i].flipping = false;  // or Animating() never settles
}

// Unlike show-seconds, a theme changes no geometry at all -- the window keeps its exact size, so
// there is no WM_SIZE coming to trigger the rebake. Every baked bitmap holds colour though, so
// all of them have to be rebuilt. Rather than a second rebake path to keep in step with Resize(),
// this clears winW_ to defeat Resize()'s "same size, nothing to do" guard and reuses it whole.
void Clock::SetLightMode(bool on) {
    const Palette* pal = on ? &kLight : &kDark;
    if (pal == pal_) return;
    pal_ = pal;
    if (!dc_ || !swap_) return;  // called before Init: the palette is all that was needed
    winW_ = 0;
    Resize();
}

bool Clock::CreateDeviceResources() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    // Hardware by default -- the GPU -- and the ini's UseWarp=1 is what asks for the software
    // device instead. That is a memory-for-CPU trade and nothing else: a hardware device drags in
    // the GPU's user-mode driver and its shader compiler (igd*/igc64 on Intel) and never gives the
    // effect pipeline's intermediates back, where WARP's are ordinary heap and the post-bake drain in
    // CreateSizeResources() reclaims them: 18.0 MB against 10.1 MB idle. WARP pays for it by
    // waking the CPU to rasterise every flip, which is the worse half on a laptop, so it is off.
    //
    // The retry is WARP either way: it is part of Windows and cannot be absent, so it is both the
    // default and the only sane fallback when a requested hardware device is unavailable (RDP,
    // headless). When WARP was asked for first the retry is unreachable, which is fine.
    const D3D_DRIVER_TYPE driver = warp_ ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    if (FAILED(D3D11CreateDevice(nullptr, driver, nullptr, flags, nullptr, 0,
            D3D11_SDK_VERSION, &d3d_, nullptr, &d3dCtx_)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
            D3D11_SDK_VERSION, &d3d_, nullptr, &d3dCtx_)))
        return false;

    ComPtr<IDXGIDevice1> dxgiDev;
    if (FAILED(d3d_.As(&dxgiDev))) return false;
    dxgiDev->SetMaximumFrameLatency(1);
    d3d_.As(&dxgiDev_);  // IDXGIDevice3 for Trim(); optional, so failure is not fatal

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    RECT rc;
    GetClientRect(hwnd_, &rc);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)max(1L, rc.right - rc.left);
    sd.Height = (UINT)max(1L, rc.bottom - rc.top);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Premultiplied, not IGNORE: the case's drop shadow fades to nothing over the desktop, so
    // the alpha we render has to survive to the compositor.
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    // The waitable object is what lets us pace to vsync by *blocking*, rather than
    // spinning or sleep-polling. It is the whole reason the animating path is cheap.
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // Composed, not bound to the window's redirection surface. A CreateSwapChainForHwnd chain
    // is opaque -- the alpha is thrown away -- so a transparent background needs the DComp
    // path: the swapchain becomes the content of a visual, and the visual is the window.
    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForComposition(d3d_.Get(), &sd, nullptr, &swap1)))
        return false;
    if (FAILED(swap1.As(&swap_))) return false;
    swap_->SetMaximumFrameLatency(1);
    frameWait_ = swap_->GetFrameLatencyWaitableObject();

    if (FAILED(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) return false;
    if (FAILED(dcomp_->CreateVisual(&dcompVisual_))) return false;
    if (FAILED(dcompVisual_->SetContent(swap_.Get()))) return false;
    if (FAILED(dcompTarget_->SetRoot(dcompVisual_.Get()))) return false;
    if (FAILED(dcomp_->Commit())) return false;

    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    ComPtr<ID2D1Factory1> d2dFactory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, opts, d2dFactory.GetAddressOf())))
        return false;
    if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDev_))) return false;
    if (FAILED(d2dDev_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    dc_->SetDpi(96.f, 96.f);  // work in raw pixels; we own the DPI scaling ourselves

    ComPtr<IDWriteFactory> dwBase;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwBase.GetAddressOf()))))
        return false;
    if (FAILED(dwBase.As(&dw_))) return false;

    for (int i = 0; i < 6; ++i)
        if (FAILED(dc_->CreateEffect(CLSID_D2D13DPerspectiveTransform, &flapFx_[i]))) return false;

    geomTop_ = HalfGeometry(true);
    geomBot_ = HalfGeometry(false);
    return geomTop_ && geomBot_ && LoadFont();
}

// The page loads JetBrains Mono at weights 700/800 and asks for 900; since 900 isn't in the
// loaded set the browser clamps to 800, so ExtraBold is the weight that matches.
// Loaded from disk rather than embedded as a resource -- an .rc reference to a missing .ttf
// breaks the *build*, and a missing font should only cost you the typeface.
bool Clock::LoadFont() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
    wcscat_s(path, L"\\assets\\JetBrainsMono-ExtraBold.ttf");

    ComPtr<IDWriteFontFile> file;
    ComPtr<IDWriteFontSetBuilder1> builder;
    ComPtr<IDWriteFontSet> set;
    if (SUCCEEDED(dw_->CreateFontFileReference(path, nullptr, &file)) &&
        SUCCEEDED(dw_->CreateFontSetBuilder(&builder)) &&
        SUCCEEDED(builder->AddFontFile(file.Get())) &&
        SUCCEEDED(builder->CreateFontSet(&set)) &&
        SUCCEEDED(dw_->CreateFontCollectionFromFontSet(set.Get(), &fontColl_))) {
        fontFamily_ = L"JetBrains Mono";
        fontWeight_ = DWRITE_FONT_WEIGHT_EXTRA_BOLD;
    }
    return true;  // no .ttf costs you the typeface, not the clock
}

// A card half: rounded on two corners, square on the other two (CSS `border-radius`
// on .static-top / .static-bottom). Built at unit scale; scaled at draw time.
ComPtr<ID2D1Geometry> Clock::HalfGeometry(bool top) {
    ComPtr<ID2D1Factory> f;
    dc_->GetFactory(&f);
    ComPtr<ID2D1Factory1> f1;
    if (FAILED(f.As(&f1))) return nullptr;

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(f1->CreatePathGeometry(&path))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return nullptr;

    const float w = CARD_W, h = HALF_H, r = FACE_R;
    auto arc = [&](float x, float y) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(r, r), 0.f,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        };

    if (top) {
        sink->BeginFigure(D2D1::Point2F(0, h), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(0, r));
        arc(r, 0);
        sink->AddLine(D2D1::Point2F(w - r, 0));
        arc(w, r);
        sink->AddLine(D2D1::Point2F(w, h));
    }
    else {
        sink->BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(w, 0));
        sink->AddLine(D2D1::Point2F(w, h - r));
        arc(w - r, h);
        sink->AddLine(D2D1::Point2F(r, h));
        arc(0, h - r);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    ComPtr<ID2D1Geometry> geom;
    path.As(&geom);
    return geom;
}

ComPtr<ID2D1Bitmap1> Clock::NewTarget(float w, float h) {
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    ComPtr<ID2D1Bitmap1> bmp;
    dc_->CreateBitmap(D2D1::SizeU(max(1u, (UINT)ceilf(w)), max(1u, (UINT)ceilf(h))), nullptr, 0,
        &props, &bmp);
    return bmp;
}

void Clock::ReleaseSizeResources() {
    dc_->SetTarget(nullptr);
    backBuffer_.Reset();
    bmpChrome_.Reset();
    for (auto& b : bmpTop_) b.Reset();
    for (auto& b : bmpBot_) b.Reset();
    for (auto& b : bmpFlap_) b.Reset();
    geomTopS_.Reset(); geomBotS_.Reset();
    brush_.Reset();
    shFront_.Reset(); shBack_.Reset(); shBottom_.Reset();
    gripShadow_.Reset(); gripStroke_.Reset();
    for (auto& fx : flapFx_) fx->SetInput(0, nullptr);  // effects hold a ref to their input bitmap

    // Releasing a D3D11 resource only *queues* its destruction; the memory is not handed back
    // until the command buffer that last referenced it retires. A drag-resize fires WM_SIZE
    // dozens of times a second, so without this the queue grows far faster than it drains and
    // the process balloons, then "settles later" as the driver catches up. Flush drains it now;
    // Trim then hands the driver's own cached allocations back to the OS.
    dc_->Flush();
    d2dDev_->ClearResources(0);  // D2D's own cache of everything we just dropped
    if (d3dCtx_) d3dCtx_->Flush();
    if (dxgiDev_) dxgiDev_->Trim();
}

// Every brush and geometry the per-frame path touches, built once for this size.
bool Clock::CreateDrawResources() {
    const float s = scale_;
    const float h = half_;

    ComPtr<ID2D1Factory> f;
    dc_->GetFactory(&f);
    ComPtr<ID2D1Factory1> f1;
    if (FAILED(f.As(&f1))) return false;

    ComPtr<ID2D1TransformedGeometry> tg;
    if (FAILED(f1->CreateTransformedGeometry(geomTop_.Get(), D2D1::Matrix3x2F::Scale(s, s), &tg)))
        return false;
    geomTopS_ = tg;
    tg.Reset();
    if (FAILED(f1->CreateTransformedGeometry(geomBot_.Get(), D2D1::Matrix3x2F::Scale(s, s), &tg)))
        return false;
    geomBotS_ = tg;

    dc_->CreateSolidColorBrush(Rgb(pal_->hingeLine), &brush_);

    auto linear = [&](D2D1_GRADIENT_STOP a, D2D1_GRADIENT_STOP b, float y0, float y1,
        ComPtr<ID2D1LinearGradientBrush>& out) {
            D2D1_GRADIENT_STOP stops[2] = { a, b };
            ComPtr<ID2D1GradientStopCollection> gsc;
            dc_->CreateGradientStopCollection(stops, 2, &gsc);
            dc_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y0), D2D1::Point2F(0, y1)),
                gsc.Get(), &out);
        };

    // The three .shadow-overlay gradients. Light mode keeps the same shape and runs the alphas at
    // about half strength, tinted to the card instead of neutral black -- these cannot be
    // inverted, because light does not cast light.
    const UINT32 sh = pal_->flipShadow;
    // .shadow-front: to top, rgba(0,0,0,.9) -> rgba(0,0,0,.1)   [opacity 0 -> .95]
    linear({ 0.f, Rgb(sh, pal_->flipWeakA) }, { 1.f, Rgb(sh, pal_->flipStrongA) }, 0, h, shFront_);
    // .shadow-back: to bottom, rgba(0,0,0,.9) -> rgba(0,0,0,.1) [opacity .95 -> 0]
    linear({ 0.f, Rgb(sh, pal_->flipStrongA) }, { 1.f, Rgb(sh, pal_->flipWeakA) }, 0, h, shBack_);
    // .shadow-bottom: to bottom, rgba(0,0,0,.85) -> rgba(0,0,0,.2) [opacity 0 -> 1]
    linear({ 0.f, Rgb(sh, pal_->flipBottomA) }, { 1.f, Rgb(sh, pal_->flipBottomTailA) }, 0, h, shBottom_);

    // The resize button's soft shadow: a radial falloff re-centred on the button at draw time.
    // Built here rather than in Render() for the usual reason -- creating a gradient brush per
    // frame measured at 44% of a core once. Not fatal if it fails; DrawResizeGrip skips it.
    D2D1_GRADIENT_STOP gs[] = { { 0.f, Rgb(0x000000, pal_->gripShadowA) },
                                { GRIP_R / (GRIP_R + GRIP_SHADOW), Rgb(0x000000, pal_->gripShadowA * 0.55f) },
                                { 1.f, Rgb(0x000000, 0.f) } };
    ComPtr<ID2D1GradientStopCollection> ggsc;
    dc_->CreateGradientStopCollection(gs, 3, &ggsc);
    if (ggsc)
        dc_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F(),
                GRIP_R + GRIP_SHADOW, GRIP_R + GRIP_SHADOW), ggsc.Get(), &gripShadow_);

    // Round caps, or the arrow's heads notch where they meet the shaft.
    f1->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &gripStroke_);

    return brush_ && shFront_ && shBack_ && shBottom_;
}

void Clock::Resize() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = (UINT)max(1L, rc.right - rc.left), h = (UINT)max(1L, rc.bottom - rc.top);

    // Not built yet: CreateWindowEx sends a WM_SIZE of its own before Init has run, and caseW_ is
    // still 0 at that point -- the scale below would be nonsense. Init calls us again once the
    // device is up, which is the first call that means anything.
    if (!dc_ || !swap_) return;

    // Geometry next, unconditionally, and above everything that can fail. The window is sized to
    // exactly contain the padded case (see WindowRectFor) -- nothing else -- so its width alone
    // determines the scale: w == scale*(caseW_+2*CASE_PAD). main.cpp is the only place that ever
    // picks a scale; this just recovers it from what it built.
    //
    // The ordering is load-bearing, not tidiness. main.cpp hit-tests CaseRect() -- built from
    // scale_ and pad_ -- on every mouse message. Leave these below the rebake and one transient
    // ResizeBuffers/GetBuffer failure strands them at the *previous* scale, so the clock answers
    // HTTRANSPARENT over everything but a corner of itself: it cannot be dragged and the
    // right-click menu only opens inside that corner. Permanently, too, because Resize() is
    // driven by WM_SIZE and no further WM_SIZE is coming once the grip is released. Measured, with
    // the failure injected: a 1116x414 window whose hit rect was still (36,36)-(462,144).
    scale_ = w / (caseW_ + 2 * CASE_PAD);
    pad_ = floorf(CASE_PAD * scale_);
    half_ = floorf(HALF_H * scale_);
#ifdef _DEBUG
    // The invariant main.cpp's whole interaction model rests on: the case plus the shadow's
    // padding on both sides *is* the window. If this trips, WM_NCHITTEST is answering for a rect
    // that is not the clock.
    assert(pad_ + caseW_ * scale_ <= (float)w && pad_ + CASE_H * scale_ <= (float)h);
#endif

    if (w == winW_ && h == winH_ && backBuffer_) return;

    ReleaseSizeResources();
    if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)))
        return;

    // winW_/winH_ only commit once every fallible step below has actually succeeded, so that the
    // guard above cannot mistake a half-built size for a finished one and skip the retry. Render()
    // is what drives that retry -- see the top of it.
    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &backBuffer_))) return;
    winW_ = w; winH_ = h;

    // A bake that failed part-way leaves winW_ matching the window, which the guard above would
    // read as "already done" -- clear it so the retry actually gets in.
    if (!CreateSizeResources()) winW_ = 0;
}

float Clock::CaseW() const { return caseW_; }
float Clock::CaseH() const { return CASE_H; }

D2D1_RECT_F Clock::CaseRect() const {
    return D2D1::RectF(pad_, pad_, pad_ + caseW_ * scale_, pad_ + CASE_H * scale_);
}

// Sane bounds for a live drag-resize: never so small the case degenerates, never bigger than
// the primary monitor can hold (case + shadow). Monitor metrics, not the window's own size --
// the window IS the case now, so there's nothing else left to fit against.
float Clock::ClampScale(float s) const {
    const float scrW = (float)GetSystemMetrics(SM_CXSCREEN), scrH = (float)GetSystemMetrics(SM_CYSCREEN);
    const float fit = min(scrW / (caseW_ + 2 * CASE_PAD), scrH / (CASE_H + 2 * CASE_PAD));
    return max(0.15f, min(fit, s));
}

float Clock::PadFor(float scale) { return floorf(CASE_PAD * scale); }

// The OS window RECT (screen px) that exactly holds the case at this layout, padded for the
// shadow. `scalePct` <= 0 means automatic: fit the primary monitor, capped at 1:1 (the CSS's
// native size). `x`/`y` < 0 means centred. The only place outside a live drag that window
// size/position gets decided -- main.cpp calls it before creating the window and whenever
// ShowSeconds changes the case width.
RECT Clock::WindowRectFor(bool showSeconds, float scalePct, int x, int y) {
    const float caseW = CaseWidthFor(showSeconds ? 3 : 2);
    const int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
    float scale = scalePct;
    if (scale <= 0.f) {
        const float fit = min(scrW / (caseW + 2 * CASE_PAD), scrH / (CASE_H + 2 * CASE_PAD));
        scale = min(1.f, fit);
    }
    scale = max(0.08f, scale);
    const float pad = floorf(CASE_PAD * scale);
    const int w = (int)ceilf(caseW * scale + 2 * pad), h = (int)ceilf(CASE_H * scale + 2 * pad);
    const int wx = x >= 0 ? x - (int)pad : (scrW - w) / 2;
    const int wy = y >= 0 ? y - (int)pad : (scrH - h) / 2;
    return { wx, wy, wx + w, wy + h };
}

bool Clock::CreateSizeResources() {
    const float s = scale_;
    bmpChrome_ = NewTarget(caseW_ * s + 2 * pad_, CASE_H * s + 2 * pad_);
    if (!bmpChrome_) return false;
    for (int i = 0; i < cardCount_; ++i) {
        bmpFlap_[i] = NewTarget(CARD_W * s, half_);
        if (!bmpFlap_[i]) return false;
    }
    for (int d = 0; d < 10; ++d) {
        bmpTop_[d] = NewTarget(CARD_W * s, half_);
        bmpBot_[d] = NewTarget(CARD_W * s, half_);
        if (!bmpTop_[d] || !bmpBot_[d]) return false;
    }

    fmt_.Reset();
    if (FAILED(dw_->CreateTextFormat(fontFamily_, fontColl_.Get(), fontWeight_,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, FONT_SIZE * s, L"", &fmt_)))
        return false;
    fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    if (!CreateDrawResources()) return false;
    BakeChrome();

    // One scratch pair and one shadow effect for all ten digits, not ten of each. They are
    // pure staging -- nothing outside this function ever reads them -- and cutting the churn
    // matters because a drag-resize runs this whole function on every WM_SIZE.
    ComPtr<ID2D1Bitmap1> glyph = NewTarget(CARD_W * s, CARD_H * s);  // text only, for the shadow's alpha
    ComPtr<ID2D1Bitmap1> full = NewTarget(CARD_W * s, CARD_H * s);   // text + both shadows
    ComPtr<ID2D1Effect> shadow;
    dc_->CreateEffect(CLSID_D2D1Shadow, &shadow);
    if (!glyph || !full || !shadow) return false;
    for (int d = 0; d < 10; ++d) BakeDigit(d, glyph.Get(), full.Get(), shadow.Get());
    shadow->SetInput(0, nullptr);
    shadow.Reset(); glyph.Reset(); full.Reset();

    // Hand the bake's leftovers back before the first frame. Dropping the ComPtrs above only
    // *queues* the scratch pair's destruction, and D2D keeps its own cache of the shadow
    // effect's intermediates on top of that -- so without this the bake's peak stays resident
    // for the life of the process. Same drain sequence ReleaseSizeResources() uses, just at the
    // other end of the function. Measured -1.1 MB idle, and -4.4 MB off the drag-storm peak.
    dc_->Flush();
    d2dDev_->ClearResources(0);
    if (d3dCtx_) d3dCtx_->Flush();
    if (dxgiDev_) dxgiDev_->Trim();

    return true;
}

void Clock::BakeChrome() {
    const float s = scale_;
    const float ox = pad_, oy = pad_;  // case origin inside the padded bitmap

    // The case's outer drop shadow. It falls outside the case rect, which is what CASE_PAD
    // is for. Baked once, so a real gaussian is free here.
    //
    // The shadow's input is a *command list*, not a bitmap. It only ever holds one rounded
    // rectangle, and a command list stores that as commands (kilobytes) rather than as a
    // case-sized sheet of mostly-transparent pixels. D2D still rasterises it internally to run
    // the gaussian, but it does so under its own bounds and lifetime rather than ours -- which
    // is what keeps it out of the resize path. Measured over a 40-cycle drag storm: the peak
    // drops from 42.7 MB to 39.4 MB (ranges do not overlap) at no CPU cost.
    ComPtr<ID2D1CommandList> silhouette;
    dc_->CreateCommandList(&silhouette);
    if (silhouette) {
        dc_->SetTarget(silhouette.Get());
        dc_->BeginDraw();
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        ComPtr<ID2D1SolidColorBrush> b;
        dc_->CreateSolidColorBrush(Rgb(0x000000, pal_->caseShadowA), &b);
        // Inset by the CSS spread: D2D's shadow effect has no spread parameter, but a negative
        // spread is just a smaller silhouette, which is the only place it could have applied.
        const float sp = SHADOW_SPREAD * s;
        dc_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(ox + sp, oy + sp, ox + caseW_ * s - sp, oy + CASE_H * s - sp),
                CASE_R * s, CASE_R * s), b.Get());
        dc_->EndDraw();
        silhouette->Close();  // a command list must be closed before it can be drawn or read
        dc_->SetTarget(nullptr);
    }

    dc_->SetTarget(bmpChrome_.Get());
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0.f));

    if (silhouette) {  // box-shadow: 0 40px 80px -15px rgba(0,0,0,.95), tightened -- see CASE_PAD
        ComPtr<ID2D1Effect> shadow;
        if (SUCCEEDED(dc_->CreateEffect(CLSID_D2D1Shadow, &shadow))) {
            shadow->SetInput(0, silhouette.Get());
            shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, SHADOW_SIGMA * s);
            dc_->DrawImage(shadow.Get(), D2D1::Point2F(0.f, SHADOW_DY * s));
        }
    }

    ComPtr<ID2D1SolidColorBrush> solid;
    dc_->CreateSolidColorBrush(Rgb(pal_->caseBg), &solid);
    const D2D1_ROUNDED_RECT caseRect =
        D2D1::RoundedRect(D2D1::RectF(ox, oy, ox + caseW_ * s, oy + CASE_H * s), CASE_R * s, CASE_R * s);
    dc_->FillRoundedRectangle(caseRect, solid.Get());

    // inset 0 1px 0 rgba(255,255,255,.04) -- the thin lit lip along the top of the case
    solid->SetColor(Rgb(pal_->caseLip, pal_->caseLipA));
    dc_->DrawLine(D2D1::Point2F(ox + CASE_R * s, oy + 0.5f * s),
        D2D1::Point2F(ox + (caseW_ - CASE_R) * s, oy + 0.5f * s), solid.Get(), s);

    for (int i = 0; i < cardCount_; ++i) {
        float x, y;
        CardPx(i, pad_, s, &x, &y);

        // .card-slot: the recess the card sits in -- darker than the case in either theme, or it
        // stops reading as a recess at all.
        solid->SetColor(Rgb(pal_->slotBg));
        dc_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x - SLOT_PAD * s, y - SLOT_PAD * s,
                x + (CARD_W + SLOT_PAD) * s, y + (CARD_H + SLOT_PAD) * s),
                SLOT_R * s, SLOT_R * s), solid.Get());
        // 0 1px 0 rgba(255,255,255,.03) -- the slot's lower lip catching light
        solid->SetColor(Rgb(pal_->slotLip, pal_->slotLipA));
        dc_->DrawLine(D2D1::Point2F(x, y + (CARD_H + SLOT_PAD) * s + 0.5f * s),
            D2D1::Point2F(x + CARD_W * s, y + (CARD_H + SLOT_PAD) * s + 0.5f * s),
            solid.Get(), s);

        // .flip-card background. Only ever visible in the sliver between the two faces,
        // but that sliver is what reads as "there is a card in there".
        solid->SetColor(Rgb(pal_->cardBg));
        dc_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x, y, x + CARD_W * s, y + CARD_H * s), CARD_R * s, CARD_R * s),
            solid.Get());
    }

    DrawColons();  // static now, so they belong here rather than in the frame loop

    dc_->EndDraw();
    dc_->SetTarget(nullptr);
}

// The hinge hardware (CSS z-index 18 and 22): the only thing left that has to paint ABOVE the
// rotating flaps. It used to share a case-sized bitmap with the acrylic sheen, because the sheen
// needed a PushLayer clipped to the case radius and that was worth baking away. The sheen is gone
// -- it was white at 2.5% alpha over a near-black case, which is not a thing you can see -- and
// what remains is 18 solid rectangles. Those are cheaper to fill than the bitmap was to blit, so
// the bitmap (one of the two that scaled with window area) goes.
//
// Coordinates are in padded-case space, like everywhere else; `origin` maps them to the window.
void Clock::DrawHinges(const D2D1_MATRIX_3X2_F& origin) {
    const float s = scale_;
    dc_->SetTransform(origin);

    for (int pass = 0; pass < 2; ++pass) {
        // .hinge-line first, then .hinge-left / .hinge-right (the rotation brackets) over it.
        // Two passes so the brush is recoloured twice, not twelve times.
        brush_->SetColor(pass == 0 ? Rgb(pal_->hingeLine) : Rgb(pal_->hingePin));
        for (int i = 0; i < cardCount_; ++i) {
            float x, y;
            CardPx(i, pad_, s, &x, &y);
            const float w = CARD_W * s, h = half_;
            if (pass == 0) {
                dc_->FillRectangle(
                    D2D1::RectF(x - 2 * s, y + h - 2 * s, x + w + 2 * s, y + h + 2 * s), brush_.Get());
            }
            else {
                dc_->FillRectangle(
                    D2D1::RectF(x - 1.5f * s, y + h - 5 * s, x + 1.5f * s, y + h + 5 * s), brush_.Get());
                dc_->FillRectangle(
                    D2D1::RectF(x + w - 1.5f * s, y + h - 5 * s, x + w + 1.5f * s, y + h + 5 * s),
                    brush_.Get());
            }
        }
    }
}

// Renders one digit's glyph (with both text-shadows) over a full 130x200 card, then
// splits it into the top and bottom face bitmaps. `glyph`, `full` and `shadow` are scratch,
// shared across all ten digits and reused; each call clears them before use.
void Clock::BakeDigit(int d, ID2D1Bitmap1* glyph, ID2D1Bitmap1* full, ID2D1Effect* shadow) {
    const float s = scale_;
    const wchar_t txt[2] = { (wchar_t)(L'0' + d), 0 };

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw_->CreateTextLayout(txt, 1, fmt_.Get(), CARD_W * s, CARD_H * s, &layout))) return;

    // CSS `line-height: 200px` on a 160px font: the font's content box is centred in the
    // 200px line box (half-leading), and the glyph rides the baseline of that. DWrite's
    // default line metrics give us the numbers to reproduce it exactly.
    DWRITE_LINE_METRICS lm{};
    UINT32 count = 1;
    if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &count)) && count == 1) {
        const float lineH = CARD_H * s;
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH,
            (lineH - lm.height) * 0.5f + lm.baseline);
    }
    // CSS `letter-spacing: -.05em`. It applies after the glyph too, which tightens the
    // advance and therefore shifts where `text-align: center` lands it.
    ComPtr<IDWriteTextLayout1> layout1;
    if (SUCCEEDED(layout.As(&layout1))) {
        DWRITE_TEXT_RANGE all{ 0, 1 };
        layout1->SetCharacterSpacing(0.f, LETTER_SPACING * s, 0.f, all);
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    dc_->CreateSolidColorBrush(Rgb(pal_->ink), &brush);

    dc_->SetTarget(glyph);
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0.f));
    dc_->DrawTextLayout(D2D1::Point2F(0, 0), layout.Get(), brush.Get());
    dc_->EndDraw();

    dc_->SetTarget(full);
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0.f));

    // text-shadow paints back-to-front: the *last* shadow in the list goes down first.
    // The 1px relief highlight only reads on a dark card, so the light palette sets its alpha
    // to 0 and it is skipped rather than inverted into a dark line nobody asked for.
    if (pal_->inkHiA > 0.f) {  // 0 1px 0 rgba(255,255,255,.1)
        brush->SetColor(Rgb(pal_->inkHi, pal_->inkHiA));
        dc_->DrawTextLayout(D2D1::Point2F(0, 1.f * s), layout.Get(), brush.Get());
    }

    shadow->SetInput(0, glyph);  // 0 4px 10px rgba(0,0,0,.8)
    shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 5.f * s);  // CSS blur radius ~= 2 sigma
    shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.f, 0.f, 0.f, pal_->inkShadowA));
    dc_->DrawImage(shadow, D2D1::Point2F(0.f, 4.f * s));

    dc_->DrawBitmap(glyph);
    dc_->EndDraw();
    dc_->SetTarget(nullptr);

    BakeFace(bmpTop_[d].Get(), full, true);
    BakeFace(bmpBot_[d].Get(), full, false);
}

void Clock::BakeFace(ID2D1Bitmap1* target, ID2D1Bitmap1* digit, bool top) {
    const float s = scale_;
    const float w = CARD_W * s, h = half_;

    dc_->SetTarget(target);
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0.f));

    // Same scaled clip CreateDrawResources() already built into geomTopS_/geomBotS_ for the
    // per-frame path -- reuse it here instead of rebuilding an identical geometry per digit.
    ID2D1Geometry* const clip = top ? geomTopS_.Get() : geomBotS_.Get();

    // .card-half has overflow:hidden, so the 160px glyph and its shadow are cut off by the
    // rounded face -- including at the hinge, which is what makes the split look like a cut.
    dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip), nullptr);

    D2D1_GRADIENT_STOP stops[2];
    if (top) {  // linear-gradient(to bottom, #2a2a2a, #1a1a1a)
        stops[0] = { 0.f, Rgb(pal_->faceTopA) };
        stops[1] = { 1.f, Rgb(pal_->faceTopB) };
    }
    else {    // linear-gradient(to bottom, #151515, #1a1a1a)
        stops[0] = { 0.f, Rgb(pal_->faceBotA) };
        stops[1] = { 1.f, Rgb(pal_->faceBotB) };
    }
    ComPtr<ID2D1GradientStopCollection> gsc;
    dc_->CreateGradientStopCollection(stops, 2, &gsc);
    ComPtr<ID2D1LinearGradientBrush> grad;
    dc_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, h)), gsc.Get(), &grad);
    if (grad) dc_->FillRectangle(D2D1::RectF(0, 0, w, h), grad.Get());

    dc_->SetTransform(D2D1::Matrix3x2F::Translation(0.f, top ? 0.f : -h));
    dc_->DrawBitmap(digit);
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());

    if (top) {
        // ::before -- linear-gradient(135deg, rgba(255,255,255,.05), rgba(255,255,255,.01) 35%,
        // transparent 36%). A CSS 135deg gradient runs top-left to bottom-right; its line is
        // |W*sin| + |H*cos| long and centred on the box.
        const float k = 0.70710678f;
        const float len = (CARD_W * k + HALF_H * k) * s;
        const D2D1_POINT_2F mid = D2D1::Point2F(w * 0.5f, h * 0.5f);
        const D2D1_POINT_2F p0 = D2D1::Point2F(mid.x - len * 0.5f * k, mid.y - len * 0.5f * k);
        const D2D1_POINT_2F p1 = D2D1::Point2F(mid.x + len * 0.5f * k, mid.y + len * 0.5f * k);
        D2D1_GRADIENT_STOP bev[] = { { 0.f, Rgb(pal_->bevel, pal_->bevelA0) },
                                     { 0.35f, Rgb(pal_->bevel, pal_->bevelA1) },
                                     { 0.36f, Rgb(pal_->bevel, 0.f) },
                                     { 1.f, Rgb(pal_->bevel, 0.f) } };
        ComPtr<ID2D1GradientStopCollection> bgsc;
        dc_->CreateGradientStopCollection(bev, 4, &bgsc);
        ComPtr<ID2D1LinearGradientBrush> bbrush;
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), bgsc.Get(), &bbrush);
        if (bbrush) dc_->FillRectangle(D2D1::RectF(0, 0, w, h), bbrush.Get());
    }

    dc_->PopLayer();

    ComPtr<ID2D1SolidColorBrush> line;
    if (top) {  // border-bottom: 1px solid rgba(0,0,0,.9) -- the dark edge at the hinge
        dc_->CreateSolidColorBrush(Rgb(pal_->edgeDark, pal_->edgeDarkA), &line);
        dc_->DrawLine(D2D1::Point2F(0, h - 0.5f * s), D2D1::Point2F(w, h - 0.5f * s), line.Get(), s);
    }
    else {    // border-top: 1px solid rgba(255,255,255,.02)
        dc_->CreateSolidColorBrush(Rgb(pal_->edgeLight, pal_->edgeLightA), &line);
        dc_->DrawLine(D2D1::Point2F(0, 0.5f * s), D2D1::Point2F(w, 0.5f * s), line.Get(), s);
    }

    dc_->EndDraw();
    dc_->SetTarget(nullptr);
}

// ---------------------------------------------------------------------------
bool Clock::Animating() const {
    if (occluded_) return false;
    for (const Card& c : cards_)
        if (c.flipping) return true;
    return false;
}

// Returns true if state changed -- a flip started, or one ended. The end matters as much as the
// start: Animating() goes false the instant the last card lands, so without this the settled card
// would never get drawn.
bool Clock::Tick(double now) {
    bool changed = false;
    SYSTEMTIME st;
    GetLocalTime(&st);

    if (st.wSecond != lastSecond_) {
        lastSecond_ = st.wSecond;
        int d[6];
        Digits(st, d);
        for (int i = 0; i < 6; ++i) {
            if (i >= cardCount_) {  // hidden: keep it current, but never animate it
                cards_[i].cur = cards_[i].next = d[i];
                continue;
            }
            if (cards_[i].flipping || cards_[i].cur == d[i]) continue;
            cards_[i].next = d[i];
            cards_[i].flipping = true;
            cards_[i].startTime = now;
            changed = true;
        }
    }

    for (Card& c : cards_) {
        if (c.flipping && now - c.startTime >= FLIP_SECS) {
            c.cur = c.next;
            c.flipping = false;
            changed = true;
        }
    }

    return changed;
}

// Composes one card's rotating flap into its scratch bitmap, then draws it through the
// perspective effect. `eased` is the CSS-eased progress of the flip, 0..1.
void Clock::DrawFlap(int i, const Card& c, float eased) {
    const float s = scale_;
    const float w = CARD_W * s, h = half_;
    const float angle = 180.f * eased;  // CSS rotateX(-180deg); D2D's sign is the mirror of CSS's

    // Past 90deg the flap's front face turns away and CSS's backface-visibility swaps in
    // .flap-back, which carries its own rotateX(180deg). About an axis in the plane, that
    // 180deg is just a vertical mirror -- so we draw the mirrored bottom face and let the
    // single outer rotation carry it the rest of the way.
    const bool showBack = angle >= 90.f;

    dc_->SetTarget(bmpFlap_[i].Get());
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(D2D1::ColorF(0, 0.f));

    ID2D1Geometry* clip;
    ID2D1LinearGradientBrush* shadow;
    if (showBack) {
        // Everything for the back face is drawn mirrored, because in CSS the shadow overlay
        // is a *child* of .flap-back and inherits its flip.
        dc_->SetTransform(D2D1::Matrix3x2F::Scale(1.f, -1.f, D2D1::Point2F(w * 0.5f, h * 0.5f)));
        dc_->DrawBitmap(bmpBot_[c.next].Get());
        clip = geomBotS_.Get();
        shadow = shBack_.Get();
        shadow->SetOpacity(0.95f * (1.f - eased));
    }
    else {
        dc_->DrawBitmap(bmpTop_[c.cur].Get());
        clip = geomTopS_.Get();
        shadow = shFront_.Get();
        shadow->SetOpacity(0.95f * eased);
    }
    dc_->FillGeometry(clip, shadow);  // clipped to the face, rounded corners and all

    dc_->EndDraw();
    dc_->SetTarget(nullptr);

    // The flap pivots on its bottom edge, which sits at the card's centre -- the same point
    // the card's `perspective: 1000px` projects through. So the rotation origin and the
    // perspective origin are the same point, and both are (CARD_W/2, HALF_H) in flap space.
    const D2D1_VECTOR_3F pivot{ w * 0.5f, h, 0.f };
    flapFx_[i]->SetInput(0, bmpFlap_[i].Get());
    flapFx_[i]->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_DEPTH, PERSPECTIVE * s);
    flapFx_[i]->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_PERSPECTIVE_ORIGIN,
        D2D1::Vector2F(pivot.x, pivot.y));
    flapFx_[i]->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION_ORIGIN, pivot);
    flapFx_[i]->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION,
        D2D1::Vector3F(angle, 0.f, 0.f));
}

// The colon dots. Static, so they are baked into the chrome and never touch the per-frame path.
// Geometry: two 12px dots, 28px apart, centred on the card height; glow from the `.colon-dot`
// base box-shadow (0 0 10px + 0 0 20px), which is the state CSS shows with the pulse removed.
void Clock::DrawColons() {
    const float s = scale_;
    const float ox = pad_, oy = pad_;
    const float glowR = (COLON_DOT * 0.5f + 20.f) * s;  // dot radius + the outer glow's reach

    ComPtr<ID2D1SolidColorBrush> dot;
    dc_->CreateSolidColorBrush(Rgb(pal_->ink), &dot);

    // A box-shadow glow is a blurred copy of the shape. For a circle that is exactly a radial
    // gradient, so that's what this is -- no blur pass, same result.
    //
    // The light palette sets colonGlowA to 0 and the glow is skipped outright, not inverted: a
    // 20px bloom around a dark dot on a light card reads as a smudge rather than as light. The
    // CSS's 1px contact shadow that would replace it is invisible at every scale this runs at,
    // so the dot is simply solid: no glow, no substitute.
    ComPtr<ID2D1RadialGradientBrush> glow;
    if (pal_->colonGlowA > 0.f) {
        D2D1_GRADIENT_STOP stops[] = { { 0.f, Rgb(pal_->ink, pal_->colonGlowA) },
                                       { 1.f, Rgb(pal_->ink, 0.f) } };
        ComPtr<ID2D1GradientStopCollection> gsc;
        dc_->CreateGradientStopCollection(stops, 2, &gsc);
        // One radial brush, re-centred per dot -- the radius and stops are the same for all of
        // them, so there's nothing dot-specific to justify a fresh brush each time.
        dc_->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F(), glowR, glowR),
            gsc.Get(), &glow);
    }

    for (int g = 0; g < groups_ - 1; ++g) {
        const float x = ox + ColonX(g) * s;
        const float top = CASE_PAD_Y + (CONTENT_H - (2 * COLON_DOT + COLON_GAP)) * 0.5f;
        for (int k = 0; k < 2; ++k) {
            const float y = oy + (top + COLON_DOT * 0.5f + k * (COLON_DOT + COLON_GAP)) * s;
            const D2D1_POINT_2F c = D2D1::Point2F(x, y);
            if (glow) {
                glow->SetCenter(c);
                dc_->FillEllipse(D2D1::Ellipse(c, glowR, glowR), glow.Get());
            }
            dc_->FillEllipse(D2D1::Ellipse(c, COLON_DOT * 0.5f * s, COLON_DOT * 0.5f * s), dot.Get());
        }
    }
}

// The button's box: a square bounding the circle, centred on the case's bottom-right corner, so
// the button straddles that edge -- half on the clock, half out over the shadow's padding.
// main.cpp hit-tests this exact rect, which is the point of it being a method: the thing you can
// grab and the thing you can see cannot drift apart. Two consequences it has to honour --
// main.cpp must check the grip *before* it decides the padding is not ours, or the outer half
// would decline clicks; and the centre is pulled inward when the padding is thinner than the
// button plus its shadow, which happens at the smallest scales, since the padding scales with the
// clock (CASE_PAD * scale_) and deliberately the button does not.
D2D1_RECT_F Clock::GripRect() const {
    const D2D1_RECT_F r = CaseRect();
    const float m = GRIP_R + GRIP_SHADOW;
    const float cx = min(r.right, (float)winW_ - m), cy = min(r.bottom, (float)winH_ - m);
    return D2D1::RectF(cx - GRIP_R, cy - GRIP_R, cx + GRIP_R, cy + GRIP_R);
}

// The one piece of interactive chrome, and only while Resize mode is on: a round button on the
// case's corner -- soft shadow, light face, hairline rim, and the diagonal double arrow. Drawn by
// us because the window has no frame and never will, and a real WS_THICKFRAME could not hold the
// case's aspect ratio without fighting the sizing loop.
void Clock::DrawResizeGrip() {
    const D2D1_RECT_F g = GripRect();
    const float cx = (g.left + g.right) * 0.5f, cy = (g.top + g.bottom) * 0.5f;

    // Nudged down, the way a real shadow falls. The brush is built per size (CreateDrawResources)
    // and only re-centred here.
    if (gripShadow_) {
        const D2D1_POINT_2F c = D2D1::Point2F(cx, cy + 1.5f);
        gripShadow_->SetCenter(c);
        dc_->FillEllipse(D2D1::Ellipse(c, GRIP_R + GRIP_SHADOW, GRIP_R + GRIP_SHADOW), gripShadow_.Get());
    }

    const D2D1_ELLIPSE face = D2D1::Ellipse(D2D1::Point2F(cx, cy), GRIP_R, GRIP_R);
    brush_->SetColor(Rgb(pal_->gripFill, pal_->gripFillA));
    dc_->FillEllipse(face, brush_.Get());
    brush_->SetColor(Rgb(pal_->gripRing, pal_->gripRingA));
    dc_->DrawEllipse(face, brush_.Get(), 1.f);

    // The NWSE double arrow -- the same axis as the IDC_SIZENWSE cursor WM_SETCURSOR puts up over
    // this button, and the only diagonal the case's locked aspect ratio can actually move along.
    // One shaft, plus a two-stroke head at each end.
    brush_->SetColor(Rgb(pal_->gripIcon, pal_->gripIconA));
    constexpr float a = 4.6f, head = 3.4f, wd = 1.7f;
    ID2D1StrokeStyle* ss = gripStroke_.Get();
    auto line = [&](float x0, float y0, float x1, float y1) {
        dc_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_.Get(), wd, ss);
        };
    line(cx - a, cy - a, cx + a, cy + a);
    line(cx - a, cy - a, cx - a + head, cy - a);
    line(cx - a, cy - a, cx - a, cy - a + head);
    line(cx + a, cy + a, cx + a - head, cy + a);
    line(cx + a, cy + a, cx + a, cy + a - head);
}

void Clock::Render(double now) {
    // No target or nothing baked means the last Resize() failed somewhere fallible. Retry it from
    // here rather than wait for the next WM_SIZE: this window only resizes when the user drags the
    // grip, so that next WM_SIZE may never come, and a one-frame glitch would otherwise be a
    // permanently frozen clock. The loop calls Render() at least once a second, which paces the
    // retry for free.
    if (!backBuffer_ || !bmpChrome_) Resize();
    if (!dc_ || !backBuffer_ || !bmpChrome_) return;

    // Covered, minimised, or the lock screen is up: nobody can see this. Render nothing at
    // all -- a cheap test call tells us when we're visible again. This is the one path that
    // truly drops us to zero CPU *and* zero GPU.
    if (occluded_) {
        if (swap_->Present(0, DXGI_PRESENT_TEST) != DXGI_STATUS_OCCLUDED) occluded_ = false;
        if (occluded_) return;
    }

    // The pacer, and the whole reason the animating path needs no timer of its own: this blocks
    // until DWM is ready for another buffer, which is the display's rate.
    //
    // It is a semaphore, not an event. Every wait spends a credit and only a Present pays one
    // back, and MaximumFrameLatency(1) means there is exactly one -- so it has to stay paired 1:1
    // with the Present at the bottom of this function. Hoisting it into the message loop (where
    // renders are conditional) starves it, and every later wait then burns its full 100ms timeout;
    // that measured as a flip running at 5fps. Do not move it.
    if (frameWait_) WaitForSingleObjectEx(frameWait_, 100, TRUE);

    const float s = scale_;

    float eased[6];
    for (int i = 0; i < cardCount_; ++i) {
        const Card& c = cards_[i];
        eased[i] = c.flipping ? Ease(kFlipEase, (float)((now - c.startTime) / FLIP_SECS)) : 0.f;
    }

    // Every flap's scratch bitmap is composed FIRST, before the backbuffer's BeginDraw opens.
    // D2D forbids retargeting a device context mid-draw, and doing so silently drops every
    // command that follows -- so the scratch passes cannot be interleaved with the main pass.
    for (int i = 0; i < cardCount_; ++i)
        if (cards_[i].flipping) DrawFlap(i, cards_[i], eased[i]);

    // The case is drawn straight to the backbuffer. It used to be composed into an offscreen
    // bitmap purely so the 3deg tilt effect had something to read; with the tilt gone there is
    // nothing to compose for, and that bitmap -- one of the two that scale with window area --
    // goes with it. The window is sized to the padded case exactly (see WindowRectFor), so
    // padded-case space and window space are the same thing -- no origin translation needed.
    auto at = [&](float x, float y) {
        return D2D1::Matrix3x2F::Translation(x, y);
        };

    dc_->SetTarget(backBuffer_.Get());
    dc_->BeginDraw();

    // Fully transparent: everything outside the case and its shadow is desktop.
    dc_->Clear(D2D1::ColorF(0, 0.f));

    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->DrawBitmap(bmpChrome_.Get());

    for (int i = 0; i < cardCount_; ++i) {
        const Card& c = cards_[i];
        float x, y;
        CardPx(i, pad_, s, &x, &y);
        const float h = half_;

        // Same trick as the DOM version: the incoming value is already sitting on the
        // static top half, hidden behind the flap, and is revealed as the flap swings away.
        const int topVal = c.flipping ? c.next : c.cur;

        dc_->SetTransform(at(x, y));
        dc_->DrawBitmap(bmpTop_[topVal].Get());
        dc_->SetTransform(at(x, y + h));
        dc_->DrawBitmap(bmpBot_[c.cur].Get());

        if (c.flipping) {
            // .shadow-bottom: the lower half darkens as the flap closes over it. Without this
            // the flap reads as flat paper rather than a card casting onto the one beneath.
            shBottom_->SetOpacity(eased[i]);
            dc_->FillGeometry(geomBotS_.Get(), shBottom_.Get());

            dc_->SetTransform(at(x, y));
            dc_->DrawImage(flapFx_[i].Get());
        }
    }

    DrawHinges(D2D1::Matrix3x2F::Identity());  // the only thing that paints above the flaps

    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (resizeMode_) DrawResizeGrip();

    dc_->EndDraw();
    dc_->SetTarget(nullptr);

    const HRESULT hr = swap_->Present(1, 0);
    occluded_ = (hr == DXGI_STATUS_OCCLUDED);
}

void Clock::Shutdown() {
    if (dc_) dc_->SetTarget(nullptr);
}
