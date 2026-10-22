// Cloud-shaped face drawn with M5GFX.
// Geometry constants come from an earlier Python prototype of the same face so the look matches
// the PIL previews (no supersampling here; eyes/arcs use M5GFX anti-aliased primitives).
#include "face.h"
#include "app.h"
#include "head_touch.h"
#include <M5Unified.h>
#include <cmath>
#include <algorithm>
#include "esp_log.h"
#include "esp_random.h"

static const char* TAG = "face";

namespace face {
namespace {

constexpr int W = 320, H = 240;
constexpr float CX = 160, CY = 127;
constexpr float BODY_W = 225.f;
constexpr float ASPECT = 0.8617021276595744f;              // cloud_lobes.json "aspect"
const float BODY_H = std::round(BODY_W * ASPECT);           // 194
constexpr float K_REF = 225.f / 94.f;
constexpr float CAP_W = 9.6f * K_REF, CAP_H = 23.0f * K_REF, CAP_GAP = 19.8f * K_REF, CAP_DY = -5.4f * K_REF;
constexpr float CAP_TILT = -26.0f;
constexpr float EYE_OX = 11.9f * K_REF, EYE_OY = -14.0f * K_REF;
constexpr float GAZE_X = 14, GAZE_Y = 9;
constexpr float BLINK_MS = 320, BLINK_CLOSE = 0.42f;

// cloud_lobes.json: (x/W, y/H, r/W) relative to body bbox centre
constexpr float LOBES[7][3] = {
    {-0.1473f, -0.0871f, 0.3408f}, {0.2045f, 0.0029f, 0.2896f}, {0.0951f, -0.2411f, 0.2153f},
    {-0.2599f, -0.1103f, 0.2329f}, {0.0454f, -0.2541f, 0.2153f}, {-0.2308f, 0.1845f, 0.2710f},
    {-0.0101f, 0.1262f, 0.3116f},
};

struct RGB { float r, g, b; };
constexpr RGB BG{16, 16, 20}, BODY{0x1E, 0x88, 0xFF}, EYE{0xF5, 0xF2, 0xEA};
inline uint16_t c565(const RGB& c) { return lgfx::color565((uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b); }

enum class Shape : uint8_t { Oval, Closed, Round };
enum class Motion : uint8_t { None, Hop, Rebound, BodyBlink };

struct P {
    float dy = 0, dx = 0, sx = 1, sy = 1, wob = 1, gx = 0, gy = 0, scale = 1;
    float openL = 1, openR = 1, hL = 1, hR = 1, lidL = 0, lidR = 0, tilt = 0;
    float body_scale = 1, body_h = 1, anchor = 0, lean = 0, eye_dx = 0, eye_dy = 0;
    Shape shape = Shape::Oval;
};

M5Canvas* s_canvas = nullptr;
State s_state = State::Idle;
uint32_t s_state_t0 = 0, s_change_ms = 0;
P s_from, s_last_pose;
Motion s_motion = Motion::None;
uint32_t s_motion_ms = 0;
uint32_t s_idle_ms = 0, s_body_blink_ms = 0, s_shrink_ms = 0;
uint32_t s_body_blink_wait = 8000, s_shrink_wait = 60000;
bool s_shrinking = false, s_anim_ready = false;
uint32_t s_think_ms = 0, s_think_wait = 0, s_nod_ms = 0, s_nod_wait = 4000;
int s_think_pose = 1;
bool s_composing = false;
float s_nod_side = 1, s_idle_open = 1;
float s_level = 0.f;
bool s_has_gaze = false;
float s_target_x = CX, s_target_y = CY;
// idle random blink scheduler (2.5-6 s interval, 10 % double blink)
float s_next_blink = 1.2f, s_blink_a = -10.f, s_blink_b = -10.f;

inline float ease_io(float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3 - 2 * x); }
inline float spring(float tau, float w = 7.f) { tau = std::max(tau, 0.f); return 1 - (1 + w * tau) * std::exp(-w * tau); }
float blink_open(float ms, float dur = BLINK_MS, float minh = 0.04f)
{
    if (ms < 0 || ms >= dur) return 1.f;
    float c = dur * BLINK_CLOSE;
    if (ms < c) return 1 - (1 - minh) * ease_io(ms / c);
    return minh + (1 - minh) * ease_io((ms - c) / (dur - c));
}
inline float fmodp(float a, float m) { float r = std::fmod(a, m); return r < 0 ? r + m : r; }
inline float frand(float lo, float hi) { return lo + (hi - lo) * (esp_random() / 4294967296.f); }

float idle_blink(float t)
{
    if (t >= s_next_blink) {
        s_blink_a = s_next_blink;
        s_blink_b = (frand(0, 1) < 0.1f) ? s_next_blink + 0.45f : -10.f;
        s_next_blink += frand(2.5f, 6.0f);
    }
    return std::min(blink_open((t - s_blink_a) * 1000), blink_open((t - s_blink_b) * 1000));
}

void update_animation(uint32_t now)
{
    if (!s_anim_ready) {
        s_anim_ready = true;
        s_idle_ms = s_body_blink_ms = now;
        s_body_blink_wait = (uint32_t)frand(8000, 20000);
        s_shrink_wait = (uint32_t)frand(60000, 90000);
    }
    if (s_state == State::Thinking && now - s_think_ms >= s_think_wait) {
        s_think_ms = now;
        s_think_pose = (s_think_pose + 1 + (int)frand(0, 2)) % 3;
        s_composing = frand(0, 1) < 0.2f;
        s_think_wait = (uint32_t)(s_composing ? frand(600, 1500) : frand(300, 1000));
    }
    if (s_state == State::Sleepy && now - s_nod_ms >= s_nod_wait + 1400) {
        s_nod_ms = now;
        s_nod_wait = (uint32_t)frand(4000, 6000) - 1400;
        s_nod_side = frand(0, 1) < 0.5f ? -1.f : 1.f;
    }
    s_idle_open = idle_blink(now / 1000.f);
    if (s_state == State::Idle && !s_has_gaze) s_idle_open = 1;
    if (s_state != State::Idle || s_has_gaze) return;
    if (!s_shrinking && s_motion == Motion::None && now - s_idle_ms >= s_shrink_wait) {
        s_shrinking = true; s_shrink_ms = now;
    }
    if (s_shrinking) {
        if (now - s_shrink_ms < 5700) return;
        s_shrinking = false; s_idle_ms = s_body_blink_ms = now;
        s_shrink_wait = (uint32_t)frand(60000, 90000);
    }
    if (s_motion == Motion::None && now - s_body_blink_ms >= s_body_blink_wait) {
        s_motion = Motion::BodyBlink; s_motion_ms = now;
        s_blink_a = s_blink_b = -10.f;
        s_next_blink = now / 1000.f + frand(2.5f, 6.f);
        s_body_blink_ms = now;
        s_body_blink_wait = (uint32_t)frand(8000, 20000);
    }
}

void apply_motion(P& p, uint32_t now)
{
    if (s_shrinking) {
        float ms = now - s_shrink_ms;
        float k = ms <= 5500 ? ms / 5500 : 1 - ease_io((ms - 5500) / 200);
        p.shape = Shape::Round; p.scale = (0.30f - 0.11f * k) * BODY_W / CAP_W;
        p.body_scale = 1 - 0.29f * k; p.anchor = 1;
        p.openL = p.openR = 1; p.gx = p.gy = 0; p.sx = p.sy = 1;
    }
    if (s_motion == Motion::None) return;
    float ms = now - s_motion_ms;
    float height = 1;
    if (s_motion == Motion::Hop) {
        if (ms < 80) height = 0.60f;
        else if (ms < 200) height = 1.10f;
        else if (ms < 350) height = 1 + 0.10f * (1 - ease_io((ms - 200) / 150));
        else s_motion = Motion::None;
    } else if (s_motion == Motion::Rebound) {
        if (ms < 120) height = 1.10f;
        else if (ms < 270) height = 1 + 0.10f * (1 - ease_io((ms - 120) / 150));
        else {
            s_motion = Motion::BodyBlink; s_motion_ms = now;
            ms = 0; height = 0.50f;
        }
    } else {
        if (ms < 100) height = 0.50f;
        else if (ms < 280) height = 1 + (0.50f - 1) * (1 - spring((ms - 100) / 1000, 35));
        else s_motion = Motion::None;
    }
    if (s_motion == Motion::None) return;
    p.body_h = height; p.body_scale = 1; p.anchor = 1;
    if (s_motion == Motion::BodyBlink || (s_motion == Motion::Hop && ms < 80)) p.shape = Shape::Closed;
}

P params(State st, float t, float global_t)
{
    P p;
    const float TAU = 2 * (float)M_PI;
    float bob = 3 * std::sin(TAU * global_t / 3.0f);
    float br = 1 + 0.01f * (1 + std::sin(TAU * global_t / 3.0f));
    switch (st) {
        case State::Blink: {
            float o = blink_open((fmodp(t, 0.8f) - 0.2f) * 1000);
            p.openL = p.openR = o; p.dy = bob;
        } break;
        case State::Listening: {
            p.scale = 1.10f; p.sx = 1.03f; p.sy = 0.97f; p.dy = 2; p.gy = -0.25f; p.gx = 0.2f * std::sin(t * 0.9f);
            float o = blink_open((fmodp(t, 4.5f) - 3.8f) * 1000); p.openL = p.openR = o;
        } break;
        case State::Thinking: {
            static constexpr float X[] = {0.15f, 0.f, -0.10f}, Y[] = {-0.08f, 0.f, 0.12f}, A[] = {-25.f, 0.f, 25.f};
            p.eye_dx = X[s_think_pose] * BODY_W; p.eye_dy = Y[s_think_pose] * BODY_H;
            p.tilt = A[s_think_pose] - CAP_TILT;
            if (s_composing) p.shape = Shape::Closed;
        } break;
        case State::Speaking: {
            float v = s_level;   // real speaker RMS drives the speaking motion
            p.hL = p.hR = 1 + 0.10f * v; p.dy = -3.5f * v; p.sx = 1 + 0.05f * v - 0.02f; p.sy = 1 - 0.05f * v + 0.02f;
            p.wob = 1 + v;
            float o = blink_open((fmodp(t, 3.6f) - 3.0f) * 1000); p.openL = p.openR = o;
        } break;
        case State::Petted: {
            float s = std::sin(TAU * t / 1.2f);
            p.shape = Shape::Closed; p.body_h = 0.85f; p.anchor = 1;
            p.dx = 3 * s; p.lean = 0.06f * BODY_W * head_touch::position() / 100.f;
        } break;
        case State::Sleepy: {
            float nod_t = t * 1000 - (s_nod_ms - s_state_t0) - s_nod_wait;
            float nod = nod_t < 0 ? 0.f : nod_t < 1200 ? ease_io(nod_t / 1200) : 1 - ease_io((nod_t - 1200) / 200);
            p.scale = 0.58f; p.gy = 0.4f; p.dy = 0.05f * BODY_H * ease_io(t / 4);
            p.lean = s_nod_side * 0.08f * BODY_W * nod; p.tilt = 8 * s_nod_side * nod;
        } break;
        case State::Sleep: {
            p.shape = Shape::Closed; p.body_h = 0.90f; p.anchor = 1; p.dy = 6; p.gy = 0.4f;
            p.body_scale = 0.925f + 0.075f * std::cos(TAU * t / 5.5f);
            p.lean = 0.04f * BODY_W;
        } break;
        case State::Idle:
        default: {   // Idle + stubbed states
            p.dy = bob; p.sx = p.sy = br;
            p.openL = p.openR = s_idle_open;
            p.gx = 0.15f * std::sin(global_t * 0.7f); p.gy = 0.1f * std::sin(global_t * 0.43f);
        } break;
    }
    return p;
}

P lerp(const P& a, const P& b, float k)
{
    auto L = [k](float x, float y) { return x + (y - x) * k; };
    P o;
    o.dy = L(a.dy, b.dy); o.dx = L(a.dx, b.dx); o.sx = L(a.sx, b.sx); o.sy = L(a.sy, b.sy); o.wob = L(a.wob, b.wob);
    o.gx = L(a.gx, b.gx); o.gy = L(a.gy, b.gy); o.scale = L(a.scale, b.scale); o.openL = L(a.openL, b.openL);
    o.openR = L(a.openR, b.openR); o.hL = L(a.hL, b.hL); o.hR = L(a.hR, b.hR); o.lidL = L(a.lidL, b.lidL);
    o.lidR = L(a.lidR, b.lidR); o.tilt = L(a.tilt, b.tilt);
    o.body_scale = L(a.body_scale, b.body_scale); o.body_h = L(a.body_h, b.body_h); o.anchor = L(a.anchor, b.anchor);
    o.lean = L(a.lean, b.lean); o.eye_dx = L(a.eye_dx, b.eye_dx); o.eye_dy = L(a.eye_dy, b.eye_dy);
    o.shape = k > 0.5f ? b.shape : a.shape;
    return o;
}

inline void rot(float px, float py, float x, float y, float a, float& ox, float& oy)
{
    float ca = std::cos(a), sa = std::sin(a);
    ox = x + px * ca - py * sa;
    oy = y + px * sa + py * ca;
}

void capsule_eye(float x, float y, float w, float h, float openk, Shape shape, float lid, float tilt_deg, uint16_t body)
{
    const uint16_t eye = c565(EYE);
    float a = tilt_deg * (float)M_PI / 180.f;
    if (shape == Shape::Closed) {
        float scale = w / CAP_W;
        float length = 0.24f * BODY_W * scale, thickness = 0.09f * BODY_W * scale;
        float x0, y0, x1, y1;
        a = 14 * (float)M_PI / 180.f;
        rot(-(length - thickness) / 2, 0, x, y, a, x0, y0);
        rot((length - thickness) / 2, 0, x, y, a, x1, y1);
        s_canvas->drawWideLine(x0, y0, x1, y1, thickness / 2, eye);
        return;
    }
    if (shape == Shape::Round) {
        s_canvas->fillSmoothCircle((int)x, (int)y, (int)(w / 2 + 0.5f), eye);
        return;
    }
    float hh = std::max(h * openk, w * 0.5f);
    float ww = w * (1 + 0.25f * (1 - openk));
    float L = std::max(hh - ww, 0.f) / 2;
    float x0, y0, x1, y1;
    if (hh < ww * 1.05f) {            // nearly closed: thin line along the minor axis
        rot(-ww * 0.6f, 0, x, y, a, x0, y0);
        rot(ww * 0.6f, 0, x, y, a, x1, y1);
        s_canvas->drawWideLine(x0, y0, x1, y1, std::max(hh * 0.6f, 3.f) / 2, eye);
        return;
    }
    rot(0, -L, x, y, a, x0, y0);
    rot(0, L, x, y, a, x1, y1);
    s_canvas->drawWideLine(x0, y0, x1, y1, ww / 2, eye);   // capsule = round-capped wide line
    if (lid > 0) {                    // upper lid: cut the top with body colour (slight droop)
        float hx = ww / 2 + std::fabs(std::sin(a)) * hh / 2 + 2;
        float top = -std::fabs(std::cos(a)) * hh / 2 - ww / 2 - 2;
        float ly = top + (-2 * top) * lid;
        s_canvas->fillTriangle(x - hx, y + top, x + hx, y + top, x + hx, y + ly - ww * 0.12f, body);
        s_canvas->fillTriangle(x - hx, y + top, x + hx, y + ly - ww * 0.12f, x - hx, y + ly + ww * 0.12f, body);
    }
}

}  // namespace

const char* name(State s)
{
    static const char* N[] = {"idle", "blink", "listening", "speaking", "petted", "sleepy", "sleep", "thinking",
                              "look_left", "look_right", "look_up", "wink", "surprised", "wakeup", "joy"};
    return N[(int)s];
}

bool init()
{
    s_canvas = new M5Canvas(&M5.Display);
    s_canvas->setPsram(true);
    s_canvas->setColorDepth(16);
    if (!s_canvas->createSprite(W, H)) {
        ESP_LOGE(TAG, "sprite alloc failed");
        return false;
    }
    ESP_LOGI(TAG, "sprite 320x240x16 in PSRAM ok");
    return true;
}

State state() { return s_state; }

void set_state(State s, uint32_t now)
{
    if (s == s_state) return;
    if (s_state == State::Sleep) hop(now);
    else if (s_state == State::Petted && (s_motion != Motion::Hop || now - s_motion_ms >= 350)) {
        s_motion = Motion::Rebound; s_motion_ms = now;
    }
    s_from = s_last_pose;
    s_state = s; s_state_t0 = now; s_change_ms = now;
    note_activity(now);
    if (s == State::Thinking) {
        s_think_ms = now; s_think_wait = 0; s_think_pose = 1; s_composing = false;
    }
    if (s == State::Sleepy) {
        s_nod_ms = now; s_nod_wait = (uint32_t)frand(4000, 6000); s_nod_side = 1;
    }
    ESP_LOGI(TAG, "state -> %s", name(s));
}

void note_activity(uint32_t now)
{
    s_shrinking = false;
    s_idle_ms = s_body_blink_ms = now;
}

void hop(uint32_t now)
{
    note_activity(now);
    s_motion = Motion::Hop; s_motion_ms = now;
}

void set_speech_level(float v)
{
    // fast attack, ~80 ms release
    s_level = v > s_level ? v : s_level + (v - s_level) * 0.35f;
}

void set_gaze_target(int x, int y)
{
    s_target_x = std::clamp(x, 0, W - 1);
    s_target_y = std::clamp(y, 0, H - 1);
    s_has_gaze = true;
    s_shrinking = false;
}

void clear_gaze_target() { s_has_gaze = false; }

void render(uint32_t now, const char (*ov)[52], int ov_n, const char* status)
{
    if (!s_canvas) return;
    update_animation(now);
    float gt = now / 1000.f;
    P cur = params(s_state, (now - s_state_t0) / 1000.f, gt);
    float tau = (now - s_change_ms) / 1000.f;
    P p = tau < 1.0f ? lerp(s_from, cur, spring(tau, 9)) : cur;
    s_last_pose = p;

    static float attention = 0.f, gaze_x = 0.f, gaze_y = 0.f;
    static uint32_t pose_ms = now;
    float dt = now - pose_ms;
    pose_ms = now;
    bool attentive = s_has_gaze || (cur.shape == Shape::Oval && (app::call_on.load() || s_state == State::Listening || s_state == State::Speaking));
    attention = std::clamp(attention + (attentive ? dt : -dt) / 250.f, 0.f, 1.f);
    float toward = ease_io(attention), away = 1 - toward;
    float target_x = s_has_gaze ? std::clamp(s_target_x - CX - p.dx, -0.20f * BODY_W, 0.20f * BODY_W) : p.eye_dx;
    float target_y = s_has_gaze ? std::clamp(s_target_y - CY - p.dy, -0.20f * BODY_H, 0.20f * BODY_H) : p.eye_dy;
    float follow = 1 - std::exp(-dt / 120.f);
    gaze_x += (target_x - gaze_x) * follow;
    gaze_y += (target_y - gaze_y) * follow;
    if (s_has_gaze) {
        if (s_state != State::Petted) p.shape = Shape::Oval;
        p.openL = p.openR = s_idle_open;
        p.lidL = p.lidR = 0;
    }
    apply_motion(p, now);

    const uint16_t bg = c565(BG), body = c565(BODY);
    s_canvas->fillScreen(bg);
    // shadow
    RGB sh{BG.r * 0.6f + 6, BG.g * 0.6f + 6, BG.b * 0.6f + 6};
    s_canvas->fillEllipse((int)CX, 227, 70, 5, c565(sh));
    // body: union of 7 circles (radius wobble +-1.2 %)
    float bx = CX + p.dx + p.lean - 0.015f * BODY_W * toward + 0.06f * gaze_x;
    float by = CY + p.dy + 0.015f * BODY_H * toward + 0.06f * gaze_y;
    float sx = p.sx * (1 + 0.07f * toward) * p.body_scale;
    float sy = p.sy * (1 + 0.04f * toward) * p.body_scale * p.body_h;
    float down = 1 + 0.04f * std::max(0.f, gaze_y) / (0.20f * BODY_H);
    float body_y = by + BODY_H * sy * (down - 1) / 2;
    struct Lobe { float x, y, rx, ry; } lobes[7];
    float bottom = 0, raw_bottom = 0;
    for (int i = 0; i < 7; i++) {
        float r = LOBES[i][2] * BODY_W * (1 + 0.012f * p.wob * std::sin(gt * 1.7f + i * 1.9f));
        float edge = std::min(1.f, std::fabs(LOBES[i][0]) / 0.26f);
        float X = bx + LOBES[i][0] * BODY_W * sx + 0.09f * gaze_x * edge - p.lean * LOBES[i][1];
        float Y = body_y + LOBES[i][1] * BODY_H * sy * down;
        lobes[i] = {X, Y, r * sx, r * sy * down};
        bottom = std::max(bottom, Y + lobes[i].ry);
        raw_bottom = std::max(raw_bottom, LOBES[i][1] * BODY_H + r);
    }
    // Keep the downward stretch on screen; eyes share the body translation.
    float anchor_y = p.anchor * (by + raw_bottom - bottom);
    float fit_y = std::min(0.f, H - 2.f - bottom - anchor_y);
    for (const auto& lobe : lobes) {
        float X = lobe.x, Y = lobe.y + anchor_y + fit_y, rx = lobe.rx, ry = lobe.ry;
        if (std::fabs(rx - ry) < 0.5f) s_canvas->fillSmoothCircle((int)X, (int)Y, (int)(rx + 0.5f), body);
        else s_canvas->fillEllipse((int)X, (int)Y, (int)(rx + 0.5f), (int)(ry + 0.5f), body);
    }
    // eyes
    float sc = p.scale * (p.shape == Shape::Round ? 1.f : p.body_scale), gx = p.gx * away;
    float ox = p.shape == Shape::Round ? 0.f : (EYE_OX + p.gx * GAZE_X) * away;
    float cx = bx + (ox + gaze_x) * p.body_scale;
    float cy = by + anchor_y + fit_y + ((EYE_OY + p.gy * GAZE_Y) * away + gaze_y) * sy;
    float gap = p.shape == Shape::Round ? 0.42f * BODY_W * p.body_scale :
                p.shape == Shape::Closed ? 0.32f * BODY_W * sc : CAP_GAP * sc;
    float tilt = (s_state == State::Thinking && !s_has_gaze) ? CAP_TILT + p.tilt : (CAP_TILT + p.tilt) * away;
    for (int side = -1; side <= 1; side += 2) {
        bool left = side < 0;
        float fs = 1 - 0.12f * std::max(0.f, -side * gx);
        float ex = cx + side * gap / 2 * (1 - 0.15f * std::fabs(gx));
        float ey = cy + (p.shape == Shape::Oval ? (side * CAP_DY / 2 * sc + side * p.tilt * 0.4f) * away * p.body_h : 0.f);
        capsule_eye(ex, ey, CAP_W * sc * fs, CAP_H * sc * (left ? p.hL : p.hR), left ? p.openL : p.openR, p.shape,
                    left ? p.lidL : p.lidR, tilt, body);
    }
    // overlay: status line + last log lines
    if (status || ov_n > 0) {
        s_canvas->setFont(&fonts::Font0);
        s_canvas->setTextSize(1);
        s_canvas->setTextDatum(textdatum_t::top_left);
        s_canvas->setTextColor(lgfx::color565(200, 200, 200), bg);
        if (status) s_canvas->drawString(status, 2, 2);
        for (int i = 0; i < ov_n; i++) s_canvas->drawString(ov[i], 2, H - 9 * (ov_n - i));
    }
    s_canvas->pushSprite(0, 0);
}

}  // namespace face
