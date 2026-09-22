#include "face.hpp"
#include "link.hpp"
#include <M5Unified.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "kiro_body.h"

namespace face {

static constexpr int kCX = 233, kCY = 233;
static constexpr int CANW = 320, CANH = 400;          // room for the body, bob and orbiting stars
static constexpr int BX = (CANW - KIRO_BODY_W) / 2;
static constexpr int BY = 40;
static constexpr uint16_t C_EYE = TFT_BLACK, C_BG = TFT_BLACK;

static M5Canvas s_canvas(&M5.Display);
static bool     s_ready = false;
static Mood     s_base = SLEEP, s_cur = SLEEP;
static uint32_t s_revertAt = 0;             // 0 = no timed override
static char     s_holdTag[16] = "";
static uint32_t s_t0, s_blinkStart, s_nextBlink, s_lookNext, s_nextFrame;
static float    s_lx, s_ly, s_ltx, s_lty;
static bool     s_dirty = true;
static char     s_foot1[64] = "", s_foot2[64] = "";

static const char* kName[N] = { "sleep", "idle", "thinking", "working", "waiting", "question", "error", "celebrate", "listening", "dizzy" };
static const char* kLabel[N] = { "zzz", "", "thinking", "working", "waiting", "hmm?", "error", "celebrate!", "listening...", "whoa..." };
static float s_level = 0, s_levelShown = 0;

void setLevel(float rms01) { s_level = rms01 < 0 ? 0 : rms01 > 1 ? 1 : rms01; }

const char* name(Mood m) { return m < N ? kName[m] : "idle"; }

int fromName(const char* k)
{
    if (!k) return -1;
    for (int i = 0; i < N; i++) if (!strcmp(k, kName[i])) return i;
    if (!strcmp(k, "sleeping")) return SLEEP;
    if (!strcmp(k, "running"))  return WORKING;
    if (!strcmp(k, "stuck"))    return ERROR;
    if (!strcmp(k, "hmm"))      return QUESTION;
    if (!strcmp(k, "done"))     return CELEBRATE;
    if (!strcmp(k, "listen") || !strcmp(k, "voice") || !strcmp(k, "recording")) return LISTENING;
    if (!strcmp(k, "shaken") || !strcmp(k, "woozy")) return DIZZY;
    return -1;
}

void begin()
{
    s_canvas.setColorDepth(16);
#if defined(ESP_PLATFORM)
    s_canvas.setPsram(true);
#endif
    s_ready = s_canvas.createSprite(CANW, CANH) != nullptr;
    if (!s_ready) swl::sendLog("face: sprite alloc failed");
    s_t0 = swl::nowMs(); s_nextBlink = s_t0 + 1400; s_lookNext = s_t0 + 900;
}

Mood current() { return s_cur; }

static void apply(Mood m) { if (m != s_cur) { s_cur = m; s_dirty = true; } }

void setBase(Mood m)
{
    s_base = m;
    if (!s_revertAt && !s_holdTag[0]) apply(m);
}

void set(Mood m, uint32_t ms, const char* tag)
{
    if (ms) { s_revertAt = swl::nowMs() + ms; s_holdTag[0] = 0; }
    else    { s_revertAt = 0; strncpy(s_holdTag, tag ? tag : "", sizeof s_holdTag - 1); }
    apply(m);
}

void release(const char* tag)
{
    if (s_holdTag[0] && tag && !strcmp(s_holdTag, tag)) { s_holdTag[0] = 0; apply(s_base); }
}

void invalidate() { s_dirty = true; }

// ---- drawing --------------------------------------------------------------
static void seg(int x1, int y1, int x2, int y2, int w) { s_canvas.drawWideLine(x1, y1, x2, y2, w, C_EYE); }

// Anti-aliased filled ellipse: 4x4 coverage sampling per pixel, blended into the
// sprite buffer. M5GFX's fillEllipse is hard-edged, which is very visible on the
// eyes at this size.
static void fillEllipseAA(int cx, int cy, int a, int b, uint16_t col)
{
    if (a < 1) a = 1;
    if (b < 1) b = 1;
    uint16_t* buf = (uint16_t*)s_canvas.getBuffer();
    if (!buf) { s_canvas.fillEllipse(cx, cy, a, b, col); return; }
    const float inva2 = 1.0f / (float)(a * a), invb2 = 1.0f / (float)(b * b);
    const int cr = (col >> 11) & 0x1F, cg = (col >> 5) & 0x3F, cb = col & 0x1F;
    for (int y = cy - b - 1; y <= cy + b + 1; y++) {
        if (y < 0 || y >= CANH) continue;
        for (int x = cx - a - 1; x <= cx + a + 1; x++) {
            if (x < 0 || x >= CANW) continue;
            int cnt = 0;
            for (int sj = 0; sj < 4; sj++) {
                float sy = (float)(y - cy) + (sj + 0.5f) * 0.25f;
                float ty = sy * sy * invb2;
                if (ty > 1.0f) continue;
                for (int si = 0; si < 4; si++) {
                    float sx = (float)(x - cx) + (si + 0.5f) * 0.25f;
                    if (sx * sx * inva2 + ty <= 1.0f) cnt++;
                }
            }
            if (!cnt) continue;
            uint16_t* px = &buf[y * CANW + x];
            if (cnt >= 16) { *px = col; continue; }
            uint16_t e = *px;
            int er = (e >> 11) & 0x1F, eg = (e >> 5) & 0x3F, eb = e & 0x1F;
            int f = (cnt * 256) / 16;
            *px = (uint16_t)((((er + (((cr - er) * f) >> 8)) & 0x1F) << 11)
                           | (((eg + (((cg - eg) * f) >> 8)) & 0x3F) << 5)
                           |  ((eb + (((cb - eb) * f) >> 8)) & 0x1F));
        }
    }
}

// Five-pointed star, filled as a triangle fan. Drawn twice (dark, slightly
// larger, then the colour) so it stays visible over the white body.
static void star(int cx, int cy, float r, float rot, uint16_t col)
{
    for (int pass = 0; pass < 2; pass++) {
        float rad = pass ? r : r + 2.0f;
        uint16_t c = pass ? col : 0x2124;               // dark grey outline
        float inner = rad * 0.42f;
        int px = 0, py = 0; bool first = true;
        for (int i = 0; i <= 10; i++) {
            float a = rot + i * (float)M_PI / 5.0f - (float)M_PI / 2.0f;
            float rr = (i & 1) ? inner : rad;
            int x = cx + (int)(cosf(a) * rr), y = cy + (int)(sinf(a) * rr);
            if (!first) s_canvas.fillTriangle(cx, cy, px, py, x, y, c);
            px = x; py = y; first = false;
        }
    }
}

// Dizzy eye: white disc with a rotating dark spiral arc on it, so the swirl is
// legible at this size instead of collapsing into a filled blob.
static void spiralEye(int cx, int cy, float phase)
{
    fillEllipseAA(cx, cy, 14, 16, TFT_WHITE);
    const int steps = 26;
    int px = cx, py = cy; bool first = true;
    for (int i = 1; i <= steps; i++) {
        float f = (float)i / steps;
        float a = phase + f * 3.2f * (float)M_PI;
        float r = 1.5f + f * 11.0f;
        int x = cx + (int)(cosf(a) * r), y = cy + (int)(sinf(a) * r * 1.1f);
        if (!first) s_canvas.drawWideLine(px, py, x, y, 3, C_EYE);
        px = x; py = y; first = false;
    }
}

static void eyeShape(int cx, int cy, int style)
{
    switch (style) {
    case 1: seg(cx-12, cy-12, cx+12, cy+12, 6); seg(cx-12, cy+12, cx+12, cy-12, 6); break;
    case 2: seg(cx-13, cy+7,  cx,    cy-9,  6); seg(cx,    cy-9,  cx+13, cy+7,  6); break;
    case 3: seg(cx-13, cy-4,  cx,    cy+5,  6); seg(cx,    cy+5,  cx+13, cy-4,  6); break;
    }
}

static void drawFrame(float t)
{
    float bob = sinf(t * 2.0f) * 6.0f;
    int jitter = 0; float wsc = 1, hsc = 1, bias_x = 0, bias_y = 0;
    int estyle = 0; bool wander = false, frown = false;
    switch (s_cur) {
    case DIZZY:     bob = sinf(t*3.2f)*9.0f; jitter = (int)(sinf(t*6.5f)*7.0f); estyle = 4; break;
    case SLEEP:     bob = sinf(t*0.9f)*4.0f; estyle = 3; break;
    case IDLE:      wander = true; break;
    case THINKING:  bob = sinf(t*1.3f)*4.0f; hsc = 0.9f; bias_x = -6; bias_y = -8; break;
    case WORKING:   bob = sinf(t*3.0f)*4.0f; hsc = 0.8f; wsc = 1.02f; bias_y = 4; frown = true; break;
    case WAITING:   bob = -fabsf(sinf(t*4.0f))*12.0f; hsc = 1.14f; wsc = 1.08f; wander = true; break;
    case QUESTION:  bob = sinf(t*1.6f)*5.0f; hsc = 1.05f; bias_x = -7; bias_y = -6; break;
    case ERROR:     jitter = (((int)(t*34)) & 1) ? 3 : -3; estyle = 1; break;
    case CELEBRATE: bob = -fabsf(sinf(t*5.0f))*18.0f; estyle = 2; break;
    case LISTENING: bob = sinf(t*1.5f)*3.0f; wsc = 1.25f; hsc = 1.2f; bias_y = -2; break;   // wide, attentive eyes
    default: break;
    }
    const int dx = BX + jitter, dy = BY + (int)bob;
    s_canvas.fillScreen(C_BG);
    s_canvas.pushImage(dx, dy, KIRO_BODY_W, KIRO_BODY_H, kiro_body);

    uint32_t ms = swl::nowMs();
    float open = 1.0f;
    if (ms - s_blinkStart < 150) { float p = (ms - s_blinkStart) / 150.0f; open = fabsf(p - 0.5f) * 2.0f; }
    float lookx = (wander ? s_lx : 0) + bias_x, looky = (wander ? s_ly : 0) + bias_y;
    int lx = dx + KIRO_EYE_L_X + (int)lookx, rx = dx + KIRO_EYE_R_X + (int)lookx, ey = dy + KIRO_EYE_L_Y + (int)looky;
    if (estyle == 0) {
        int ew = (int)(KIRO_EYE_W * wsc), eh = (int)(KIRO_EYE_H * hsc * open); if (eh < 3) eh = 3;
        fillEllipseAA(lx, ey, ew / 2, eh / 2, C_EYE);
        fillEllipseAA(rx, ey, ew / 2, eh / 2, C_EYE);
    } else if (estyle == 4) {
        // counter-rotating spirals sell the woozy look
        spiralEye(lx, ey, t * 5.0f);
        spiralEye(rx, ey, -t * 5.0f);
    } else { eyeShape(lx, ey, estyle); eyeShape(rx, ey, estyle); }
    if (frown) { seg(lx-15, ey-20, lx+11, ey-11, 5); seg(rx-11, ey-11, rx+15, ey-20, 5); }
    if (s_cur == LISTENING) {
        // pulsing rings around the ghost + sound bars under the chin, driven by mic level
        s_levelShown += (s_level - s_levelShown) * 0.4f;
        int gcx = dx + KIRO_BODY_W/2, gcy = dy + KIRO_BODY_H/2;
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * 0.8f + i / 3.0f, 1.0f);
            int r = KIRO_BODY_H/2 + 8 + (int)(ph * 34);
            uint16_t col = ph < 0.33f ? 0x7D7F : ph < 0.66f ? 0x4C1C : 0x2A8E;    // cyan fading out
            s_canvas.drawCircle(gcx, gcy, r, col);
        }
        const int bars = 7, bw = 10, gap = 6, x0 = CANW/2 - (bars*bw + (bars-1)*gap)/2, base = CANH - 70;
        for (int i = 0; i < bars; i++) {
            float wave = 0.5f + 0.5f * sinf(t * 9.0f + i * 1.1f);
            int h = 4 + (int)((10 + 44 * s_levelShown) * wave);
            s_canvas.fillRoundRect(x0 + i * (bw + gap), base - h, bw, h, 3, 0x07FF);
        }
    }
    if (s_cur == DIZZY) {                              // stars circling overhead, spinning as they go
        int hcx = dx + KIRO_BODY_W/2, hcy = dy - 12;   // orbit sits above the head, not on it
        for (int i = 0; i < 4; i++) {
            float a = t*3.4f + i * (float)M_PI / 2;
            int sx = hcx + (int)(cosf(a) * 74), sy = hcy + (int)(sinf(a) * 13);
            float depth = 0.75f + 0.25f * sinf(a);     // nearer stars (front of the orbit) look bigger
            star(sx, sy, 15.0f * depth, t * 2.2f + i, (i & 1) ? 0xFEE0 : 0xFFF4);
        }
    }
    if (s_cur == CELEBRATE) {
        int gcx = dx + KIRO_BODY_W/2, gcy = dy + KIRO_BODY_H/2;
        for (int i = 0; i < 7; i++) {
            float a = t*3.0f + i*0.9f;
            s_canvas.fillCircle(gcx + (int)(cosf(a)*(KIRO_BODY_W/2 + 12)), gcy + (int)(sinf(a)*(KIRO_BODY_H/2 + 8)),
                                ((i + (int)(t*4)) & 1) ? 5 : 3, (i & 1) ? 0xFED9 : 0x67F8);
        }
    }
    s_canvas.setTextDatum(middle_center);
    s_canvas.setFont(&fonts::FreeSans12pt7b);
    s_canvas.setTextColor(TFT_LIGHTGREY, C_BG);
    s_canvas.drawString(kLabel[s_cur], CANW / 2, CANH - 56);
    s_canvas.setFont(&fonts::FreeSans9pt7b);
    s_canvas.setTextColor(TFT_DARKGREY, C_BG);
    s_canvas.drawString(s_foot1, CANW / 2, CANH - 30);
    s_canvas.drawString(s_foot2, CANW / 2, CANH - 10);
    s_canvas.pushSprite(kCX - CANW / 2, kCY - CANH / 2 + 6);
}

void tick(const char* footer1, const char* footer2, bool force)
{
    if (!s_ready) return;
    uint32_t ms = swl::nowMs();
    if (s_revertAt && (int32_t)(ms - s_revertAt) >= 0) { s_revertAt = 0; apply(s_base); }
    if (strcmp(footer1, s_foot1)) { strncpy(s_foot1, footer1, sizeof s_foot1 - 1); s_dirty = true; }
    if (strcmp(footer2, s_foot2)) { strncpy(s_foot2, footer2, sizeof s_foot2 - 1); s_dirty = true; }

    bool canBlink = (s_cur == IDLE || s_cur == WORKING || s_cur == WAITING || s_cur == THINKING || s_cur == QUESTION);
    if (canBlink && ms >= s_nextBlink) { s_blinkStart = ms; s_nextBlink = ms + 2200 + (rand() % 2600); }
    if (ms >= s_lookNext) { s_ltx = (float)(rand() % 21 - 10); s_lty = (float)(rand() % 13 - 6); s_lookNext = ms + 900 + (rand() % 1600); }
    s_lx += (s_ltx - s_lx) * 0.15f; s_ly += (s_lty - s_ly) * 0.15f;

    // sleeping face barely moves: 5 fps is enough and saves power; otherwise 20 fps
    uint32_t period = (s_cur == SLEEP) ? 200 : 50;
    if (!force && !s_dirty && (int32_t)(ms - s_nextFrame) < 0) return;
    s_nextFrame = ms + period; s_dirty = false;
    drawFrame((ms - s_t0) / 1000.0f);
}

} // namespace face
