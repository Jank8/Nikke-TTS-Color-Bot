#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#include <timeapi.h>

// ─────────────────────────────────────────────
//  ADMIN CHECK & RESTART
// ─────────────────────────────────────────────
static bool is_admin() {
    BOOL is_admin = FALSE;
    PSID admin_group = NULL;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(NULL, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    return is_admin != FALSE;
}

static void restart_as_admin() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    
    SHELLEXECUTEINFOA sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "runas";
    sei.lpFile = path;
    sei.nShow = SW_NORMAL;
    
    if (ShellExecuteExA(&sei)) {
        exit(0);
    } else {
        printf("[ERR] Cannot start as administrator\n");
        exit(1);
    }
}

// ─────────────────────────────────────────────
//  CONFIG (loaded from bot_config.json - simple parser)
// ─────────────────────────────────────────────
static int   cfg_hit_zone_y       = 0;
static int   cfg_lane_centers[4]  = {0,0,0,0};
static int   cfg_lane_hold[4]     = {0,0,0,0};
static int   cfg_green_x          = 0;
static int   cfg_purple_x         = 0;
static int   cfg_gold_x           = 0;
static int   cfg_green_tap_x      = 0;
static int   cfg_purple_tap_x     = 0;
static int   cfg_scan_half        = 6;

// ─────────────────────────────────────────────
//  PARAMETERS
// ─────────────────────────────────────────────
#define SCAN_MARGIN_X      8
#define SCAN_Y_A_OFFSET    18
#define SCAN_Y_B_OFFSET    51
#define SCAN_Y_GOLD_OFFSET 80
#define TARGET_FPS         300.0
#define GREEN_HOLD_GRACE   0.02
#define PURPLE_HOLD_GRACE  0.02
#define KEY_REPRESS_COOLDOWN 0.01

// Keys: d f j k b a l
static const WORD VK_KEYS[7]   = { 'D','F','J','K','B','A','L' };
// Scan codes (hardware, independent of keyboard layout)
static const WORD SC_KEYS[7]   = { 0x20, 0x21, 0x24, 0x25, 0x30, 0x1E, 0x26 };
static const char KEY_NAMES[7] = { 'd','f','j','k','b','a','l' };
#define KEY_D 0
#define KEY_F 1
#define KEY_J 2
#define KEY_K 3
#define KEY_B 4
#define KEY_A 5
#define KEY_L 6
#define NUM_KEYS 7

static bool  held[NUM_KEYS]        = {};
static double released_at[NUM_KEYS] = {};

// ─────────────────────────────────────────────
//  TIMER
// ─────────────────────────────────────────────
static LARGE_INTEGER g_freq;
static double now_sec() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_freq.QuadPart;
}

// ─────────────────────────────────────────────
//  INPUT
// ─────────────────────────────────────────────
static void send_key(int idx, bool keyup) {
    INPUT inp = {};
    inp.type           = INPUT_KEYBOARD;
    inp.ki.wVk         = 0;
    inp.ki.wScan       = SC_KEYS[idx];
    inp.ki.dwFlags     = KEYEVENTF_SCANCODE | (keyup ? KEYEVENTF_KEYUP : 0);
    inp.ki.dwExtraInfo = 0;
    SendInput(1, &inp, sizeof(INPUT));
}

static void press_key(int idx) {
    if (held[idx]) return;
    if (now_sec() - released_at[idx] < KEY_REPRESS_COOLDOWN) return;
    send_key(idx, false);
    held[idx] = true;
}

static void release_key(int idx) {
    if (!held[idx]) return;
    send_key(idx, true);
    held[idx] = false;
    released_at[idx] = now_sec();
}

static void release_all() {
    for (int i = 0; i < NUM_KEYS; i++) release_key(i);
}

// ─────────────────────────────────────────────
//  COLOR CLASSIFICATION (BGRA)
// ─────────────────────────────────────────────
typedef struct { bool white, dark, green, purple, gold; } PixelClass;

static inline PixelClass classify(const uint8_t* p) {
    // p = [B, G, R, A]
    int B = p[0], G = p[1], R = p[2];
    PixelClass c = {};
    c.white  = R > 185 && G > 185 && B > 185;
    c.dark   = R < 60  && G < 60  && B < 60;
    c.green  = G > 130 && G > R + 30 && G > B + 15;
    c.purple = R > 130 && B > 120 && G < 120;
    c.gold   = R > 240 && G > 215 && B > 110 && B < 145;
    return c;
}

// Check if column x (relative to buffer) contains any matching pixel
static bool col_any_white (const uint8_t* buf, int w, int h, int x) {
    if (x < 0 || x >= w) return false;
    for (int y = 0; y < h; y++) {
        const uint8_t* p = buf + (y * w + x) * 4;
        if (classify(p).white) return true;
    }
    return false;
}
static bool col_any_dark  (const uint8_t* buf, int w, int h, int x) {
    if (x < 0 || x >= w) return false;
    for (int y = 0; y < h; y++) {
        const uint8_t* p = buf + (y * w + x) * 4;
        if (classify(p).dark) return true;
    }
    return false;
}
static bool col_any_gold  (const uint8_t* buf, int w, int h, int x) {
    if (x < 0 || x >= w) return false;
    for (int y = 0; y < h; y++) {
        const uint8_t* p = buf + (y * w + x) * 4;
        if (classify(p).gold) return true;
    }
    return false;
}
static bool zone_any_green(const uint8_t* buf, int w, int h, int x, int half) {
    int x0 = x - half < 0 ? 0 : x - half;
    int x1 = x + half + 1 > w ? w : x + half + 1;
    int count = 0;
    for (int y = 0; y < h; y++)
        for (int cx = x0; cx < x1; cx++) {
            const uint8_t* p = buf + (y * w + cx) * 4;
            if (classify(p).green && ++count >= 2) return true;
        }
    return false;
}
static bool zone_any_purple(const uint8_t* buf, int w, int h, int x, int half) {
    int x0 = x - half < 0 ? 0 : x - half;
    int x1 = x + half + 1 > w ? w : x + half + 1;
    int count = 0;
    for (int y = 0; y < h; y++)
        for (int cx = x0; cx < x1; cx++) {
            const uint8_t* p = buf + (y * w + cx) * 4;
            if (classify(p).purple && ++count >= 2) return true;
        }
    return false;
}

// ─────────────────────────────────────────────
//  DXGI DESKTOP DUPLICATION
// ─────────────────────────────────────────────
static ID3D11Device*            g_device    = NULL;
static ID3D11DeviceContext*     g_ctx       = NULL;
static IDXGIOutputDuplication*  g_dupl      = NULL;
static ID3D11Texture2D*         g_staging   = NULL;
static int g_screen_w = 0, g_screen_h = 0;

static bool dxgi_init() {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &g_device, &fl, &g_ctx);
    if (FAILED(hr)) { printf("[ERR] D3D11CreateDevice: %08X\n", hr); return false; }

    IDXGIDevice* dxgi_dev = NULL;
    g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev);
    IDXGIAdapter* adapter = NULL;
    dxgi_dev->GetAdapter(&adapter);
    IDXGIOutput* output = NULL;
    adapter->EnumOutputs(0, &output);
    IDXGIOutput1* output1 = NULL;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

    DXGI_OUTPUT_DESC desc = {};
    output->GetDesc(&desc);
    g_screen_w = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
    g_screen_h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

    hr = output1->DuplicateOutput(g_device, &g_dupl);
    if (FAILED(hr)) { printf("[ERR] DuplicateOutput: %08X\n", hr); return false; }

    output1->Release(); output->Release(); adapter->Release(); dxgi_dev->Release();
    printf("[DXGI] Screen: %dx%d\n", g_screen_w, g_screen_h);
    return true;
}

// Capture screen region to BGRA buffer. Returns true if new frame.
// buf must have size w*h*4 bytes.
static bool dxgi_grab_region(int left, int top, int w, int h, uint8_t* buf) {
    IDXGIResource* res = NULL;
    DXGI_OUTDUPL_FRAME_INFO fi = {};
    HRESULT hr = g_dupl->AcquireNextFrame(0, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        // Attempt reinitialization after loss
        g_dupl->Release(); g_dupl = NULL;
        dxgi_init();
        return false;
    }

    ID3D11Texture2D* tex = NULL;
    res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);

    // Create/recreate staging texture if needed
    if (!g_staging) {
        D3D11_TEXTURE2D_DESC td = {};
        tex->GetDesc(&td);
        td.Usage          = D3D11_USAGE_STAGING;
        td.BindFlags      = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags      = 0;
        g_device->CreateTexture2D(&td, NULL, &g_staging);
    }

    g_ctx->CopyResource(g_staging, tex);
    tex->Release(); res->Release();
    g_dupl->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    // Copy only requested region
    const uint8_t* src = (const uint8_t*)mapped.pData;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = src + (top + y) * mapped.RowPitch + left * 4;
        memcpy(buf + y * w * 4, row, w * 4);
    }

    g_ctx->Unmap(g_staging, 0);
    return true;
}

// ─────────────────────────────────────────────
//  JSON PARSER (minimal, numbers only)
// ─────────────────────────────────────────────
static int json_int(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}
static void json_int_array(const char* json, const char* key, int* out, int n) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    for (int i = 0; i < n; i++) {
        while (*p == ' ' || *p == ',') p++;
        out[i] = atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
    }
}

static bool load_config(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { printf("[ERR] Missing %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char* buf = (char*)malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);

    cfg_hit_zone_y  = json_int(buf, "hit_zone_y");
    cfg_green_x     = json_int(buf, "green_x");
    cfg_purple_x    = json_int(buf, "purple_x");
    cfg_gold_x      = json_int(buf, "gold_x");
    cfg_green_tap_x = json_int(buf, "green_tap_x");
    cfg_purple_tap_x= json_int(buf, "purple_tap_x");
    cfg_scan_half   = json_int(buf, "special_scan_half");
    if (cfg_scan_half == 0) cfg_scan_half = 6;
    json_int_array(buf, "lane_centers_x", cfg_lane_centers, 4);
    json_int_array(buf, "lane_hold_x",    cfg_lane_hold,    4);

    free(buf);
    printf("[CFG] hit_y=%d centers=%d,%d,%d,%d\n",
        cfg_hit_zone_y,
        cfg_lane_centers[0], cfg_lane_centers[1],
        cfg_lane_centers[2], cfg_lane_centers[3]);
    return cfg_hit_zone_y > 0;
}

// ─────────────────────────────────────────────
//  MAIN BOT LOOP
// ─────────────────────────────────────────────
static volatile bool g_running = false;

static void bot_loop() {
    int hit_y   = cfg_hit_zone_y;
    int scan_y_a = hit_y - SCAN_Y_A_OFFSET;
    int scan_y_b = hit_y - SCAN_Y_B_OFFSET;
    int scan_top = scan_y_b < 0 ? 0 : scan_y_b;
    int scan_h   = (scan_y_a - scan_y_b) + 1;
    if (scan_h < 3) scan_h = 3;

    int gold_scan_top = hit_y - SCAN_Y_GOLD_OFFSET;
    if (gold_scan_top < 0) gold_scan_top = 0;
    int gold_scan_h = (hit_y - SCAN_Y_A_OFFSET) - gold_scan_top + 1;
    if (gold_scan_h < 3) gold_scan_h = 3;

    // Determine scan_left and scan_w
    int all_x[9] = {
        cfg_lane_centers[0], cfg_lane_centers[1], cfg_lane_centers[2], cfg_lane_centers[3],
        cfg_green_x, cfg_purple_x, cfg_gold_x, cfg_green_tap_x, cfg_purple_tap_x
    };
    int min_x = all_x[0], max_x = all_x[0];
    for (int i = 1; i < 9; i++) {
        if (all_x[i] < min_x) min_x = all_x[i];
        if (all_x[i] > max_x) max_x = all_x[i];
    }
    int scan_left = min_x - SCAN_MARGIN_X;
    if (scan_left < 0) scan_left = 0;
    int scan_w = max_x - min_x + 1 + 2 * SCAN_MARGIN_X;
    if (scan_w < 1) scan_w = 1;

    // Local coordinates (relative to scan_left)
    int lc[4], lh[4];
    for (int i = 0; i < 4; i++) {
        lc[i] = cfg_lane_centers[i] - scan_left;
        lh[i] = cfg_lane_hold[i]    - scan_left;
    }
    int lg  = cfg_green_x     - scan_left;
    int lp  = cfg_purple_x    - scan_left;
    int lgo = cfg_gold_x      - scan_left;
    int gh  = cfg_scan_half;
    int ph  = cfg_scan_half;

    printf("[BOT] Region: top=%d h=%d w=%d left=%d\n", scan_top, scan_h, scan_w, scan_left);
    printf("[BOT] Gold region: top=%d h=%d\n", gold_scan_top, gold_scan_h);

    // Pixel buffers — one large buffer covering both regions
    // From gold_scan_top (top) to scan_top + scan_h (bottom)
    int combined_top  = gold_scan_top;
    int combined_bottom = scan_top + scan_h;
    int combined_h    = combined_bottom - combined_top;
    // Offset inside combined buffer for normal region
    int normal_off = scan_top - combined_top;  // starting row of normal region
    int gold_off   = 0; // gold starts from top of combined

    uint8_t* combined_buf = (uint8_t*)malloc(scan_w * combined_h * 4);
    // Pointers to subregions
    uint8_t* buf      = combined_buf + normal_off * scan_w * 4;
    uint8_t* gold_buf = combined_buf + gold_off   * scan_w * 4;

    // State
    bool  in_hold[NUM_KEYS]       = {};
    double white_last_seen[4]     = {-1,-1,-1,-1};
    double green_last_seen        = -1;
    double purple_last_seen       = -1;

    const double MIN_LOOP = 1.0 / TARGET_FPS;
    bool last_frame_valid = false;

    while (g_running) {
        double t0 = now_sec();

        // Capture frame — one grab covering both regions
        bool new_frame = dxgi_grab_region(scan_left, combined_top, scan_w, combined_h, combined_buf);
        if (!new_frame && !last_frame_valid) {
            Sleep(1);
            continue;
        }
        if (new_frame) last_frame_valid = true;

        double now = now_sec();

        // Collect press/release
        int to_press[NUM_KEYS],   np = 0;
        int to_release[NUM_KEYS], nr = 0;

        // ── WHITE (d, f, j, k) ───────────────────────────────────────
        // lane_keys = d,f,j,k = indices 0,1,2,3
        for (int i = 0; i < 4; i++) {
            int key = i; // KEY_D=0 KEY_F=1 KEY_J=2 KEY_K=3
            bool white_present = col_any_white(buf, scan_w, scan_h, lc[i]);
            bool white_hold    = white_present && col_any_white(buf, scan_w, scan_h, lh[i]);
            bool dark_present  = col_any_dark (buf, scan_w, scan_h, lc[i]);

            if (white_present) {
                if (!held[key]) {
                    to_press[np++] = key;
                    in_hold[key]   = white_hold;
                } else if (white_hold) {
                    in_hold[key] = true;
                }
                white_last_seen[i] = now;
            } else if (dark_present && in_hold[key]) {
                if (held[key]) {
                    to_release[nr++] = key;
                    in_hold[key]     = false;
                    white_last_seen[i] = -1;
                }
            } else {
                if (held[key]) {
                    if (in_hold[key]) {
                        // Hold: refresh last_seen ONLY when we see white (glow)
                        // If no white or dark = empty background, let grace timeout trigger
                        if (white_present) {
                            white_last_seen[i] = now;
                        } else if (white_last_seen[i] >= 0 && now - white_last_seen[i] > 0.04) {
                            // Grace timeout for hold
                            to_release[nr++] = key;
                            in_hold[key]     = false;
                            white_last_seen[i] = -1;
                        }
                    } else {
                        if (white_last_seen[i] >= 0 && now - white_last_seen[i] > 0.04) {
                            to_release[nr++] = key;
                            in_hold[key]     = false;
                            white_last_seen[i] = -1;
                        }
                    }
                }
            }
        }

        // ── GOLD (b = KEY_B = 4) ────────────────────────────────────
        {
            bool gold_present = col_any_gold(gold_buf, scan_w, gold_scan_h, lgo);
            if (gold_present) {
                if (!held[KEY_B]) {
                    to_press[np++] = KEY_B;
                    in_hold[KEY_B] = true;
                }
            } else {
                if (held[KEY_B]) {
                    to_release[nr++] = KEY_B;
                    in_hold[KEY_B]   = false;
                }
            }
        }

        // ── GREEN (a = KEY_A = 5) ──────────────────────────────────
        {
            bool green_any = false;
            for (int i = 0; i < 4 && !green_any; i++)
                green_any = zone_any_green(buf, scan_w, scan_h, lc[i], gh);
            bool green_hold_pt = zone_any_green(buf, scan_w, scan_h, lg, gh);
            bool green_active  = green_any || green_hold_pt;

            if (green_active) {
                if (!held[KEY_A]) {
                    to_press[np++] = KEY_A;
                    in_hold[KEY_A] = true;
                }
                green_last_seen = now;
            } else {
                if (held[KEY_A]) {
                    if (in_hold[KEY_A]) {
                        if (green_last_seen >= 0 && now - green_last_seen > GREEN_HOLD_GRACE) {
                            to_release[nr++] = KEY_A;
                            in_hold[KEY_A]   = false;
                            green_last_seen  = -1;
                        }
                    } else {
                        to_release[nr++] = KEY_A;
                        in_hold[KEY_A]   = false;
                        green_last_seen  = -1;
                    }
                }
            }
        }

        // ── PURPLE (l = KEY_L = 6) ────────────────────────────────
        {
            bool purple_any = false;
            for (int i = 0; i < 4 && !purple_any; i++)
                purple_any = zone_any_purple(buf, scan_w, scan_h, lc[i], ph);
            bool purple_hold_pt = zone_any_purple(buf, scan_w, scan_h, lp, ph);
            bool purple_active  = purple_any || purple_hold_pt;

            if (purple_active) {
                if (!held[KEY_L]) {
                    to_press[np++] = KEY_L;
                    in_hold[KEY_L] = true;
                }
                purple_last_seen = now;
            } else {
                if (held[KEY_L]) {
                    if (in_hold[KEY_L]) {
                        if (purple_last_seen >= 0 && now - purple_last_seen > PURPLE_HOLD_GRACE) {
                            to_release[nr++] = KEY_L;
                            in_hold[KEY_L]   = false;
                            purple_last_seen = -1;
                        }
                    } else {
                        to_release[nr++] = KEY_L;
                        in_hold[KEY_L]   = false;
                        purple_last_seen = -1;
                    }
                }
            }
        }

        // ── SEND INPUTS ────────────────────────────────────────────
        for (int i = 0; i < nr; i++) release_key(to_release[i]);
        for (int i = 0; i < np; i++) press_key(to_press[i]);

        // ── STOP: F8 ─────────────────────────────────────────────────
        if (GetAsyncKeyState(VK_F8) & 0x0001) {  // 0x0001 = new press
            g_running = false;
            break;
        }

        double elapsed = now_sec() - t0;
        if (elapsed < MIN_LOOP) {
            DWORD ms = (DWORD)((MIN_LOOP - elapsed) * 1000.0);
            if (ms > 0) Sleep(ms);
        }
    }

    release_all();
    free(combined_buf);
    printf("[BOT] Stopped.\n");
}

// ─────────────────────────────────────────────
//  CALIBRATION
// ─────────────────────────────────────────────
static POINT wait_for_click() {
    // Wait for button release
    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) Sleep(10);
    // Wait for press
    while (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) Sleep(10);
    POINT pt; GetCursorPos(&pt);
    return pt;
}

static void save_config(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n");
    fprintf(f, "    \"hit_zone_y\": %d,\n", cfg_hit_zone_y);
    fprintf(f, "    \"lane_centers_x\": [%d, %d, %d, %d],\n",
        cfg_lane_centers[0], cfg_lane_centers[1], cfg_lane_centers[2], cfg_lane_centers[3]);
    fprintf(f, "    \"lane_hold_x\": [%d, %d, %d, %d],\n",
        cfg_lane_hold[0], cfg_lane_hold[1], cfg_lane_hold[2], cfg_lane_hold[3]);
    fprintf(f, "    \"lane_keys\": [\"d\", \"f\", \"j\", \"k\"],\n");
    fprintf(f, "    \"green_x\": %d,\n",      cfg_green_x);
    fprintf(f, "    \"purple_x\": %d,\n",     cfg_purple_x);
    fprintf(f, "    \"gold_x\": %d,\n",       cfg_gold_x);
    fprintf(f, "    \"green_tap_x\": %d,\n",  cfg_green_tap_x);
    fprintf(f, "    \"purple_tap_x\": %d,\n", cfg_purple_tap_x);
    fprintf(f, "    \"special_scan_half\": %d\n", cfg_scan_half);
    fprintf(f, "}\n");
    fclose(f);
}

static void calibrate() {
    printf("\n[CALIBRATION] Click 5 points on the HIT LINE (left to right)...\n");
    POINT pts[5];
    for (int i = 0; i < 5; i++) {
        printf("Point %d/5...\n", i+1);
        pts[i] = wait_for_click();
        Sleep(250);
    }

    int edges[5];
    int sum_y = 0;
    for (int i = 0; i < 5; i++) { edges[i] = pts[i].x; sum_y += pts[i].y; }
    cfg_hit_zone_y = sum_y / 5;

    // Margins 2%
    double e[8];
    for (int i = 0; i < 4; i++) {
        double w = (double)(edges[i+1] - edges[i]);
        double m = w * 0.02;
        e[i*2]   = edges[i]   + m;
        e[i*2+1] = edges[i+1] - m;
    }

    for (int i = 0; i < 4; i++)
        cfg_lane_centers[i] = (int)((e[i*2] + e[i*2+1]) / 2.0);

    for (int i = 0; i < 4; i++)
        cfg_lane_hold[i] = (int)(e[i*2] + (e[i*2+1] - e[i*2]) * 0.15);

    cfg_green_x      = (int)(e[0] + (e[1] - e[0]) * 0.2);
    cfg_purple_x     = (int)(e[6] + (e[7] - e[6]) * 0.8);
    cfg_green_tap_x  = (int)((e[2] + e[3]) / 2.0);
    cfg_purple_tap_x = (int)((e[4] + e[5]) / 2.0);
    cfg_gold_x       = (int)((e[6] + e[7]) / 2.0);

    double total_w = 0;
    for (int i = 0; i < 4; i++) total_w += edges[i+1] - edges[i];
    int lane_w = (int)(total_w / 4.0);
    cfg_scan_half = lane_w / 5;
    if (cfg_scan_half < 4) cfg_scan_half = 4;

    save_config("bot_config.json");
    printf("Calibration complete! hit_zone_y=%d\n", cfg_hit_zone_y);
    printf("  Centers=%d,%d,%d,%d\n",
        cfg_lane_centers[0], cfg_lane_centers[1],
        cfg_lane_centers[2], cfg_lane_centers[3]);
}

// ─────────────────────────────────────────────
//  MAIN
// ─────────────────────────────────────────────
int main() {
    // Check administrator privileges
    if (!is_admin()) {
        printf("Administrator privileges required. Restarting...\n");
        restart_as_admin();
        return 1;
    }

    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);

    printf("===========================================\n");
    printf("  Nikke TTS Color Bot (C++ DXGI)\n");
    printf("===========================================\n");
    printf("\n");
    printf("REQUIREMENTS:\n");
    printf("  - Game resolution: 1920x1080 (1080p)\n");
    printf("  - Windows scaling: 100%%\n");
    printf("  - Primary monitor only\n");
    printf("\n");
    printf("IMPORTANT: Configure your game keybinds:\n");
    printf("  White notes:  D F J K\n");
    printf("  Gold note:    B\n");
    printf("  Green note:   A\n");
    printf("  Purple note:  L\n");
    printf("\n");
    printf("Controls:\n");
    printf("  F8  - Start/Stop bot\n");
    printf("  F9  - Calibration (click 5 points on hit line)\n");
    printf("  F10 - Exit\n");
    printf("\n");

    if (!dxgi_init()) {
        printf("[ERR] Cannot initialize DXGI\n");
        return 1;
    }

    load_config("bot_config.json");

    while (true) {
        // F8 - start/stop
        if (GetAsyncKeyState(VK_F8) & 0x8000) {
            if (!g_running) {
                if (cfg_hit_zone_y > 0) {
                    // Wait for F8 release before entering loop
                    while (GetAsyncKeyState(VK_F8) & 0x8000) Sleep(10);
                    g_running = true;
                    printf("[BOT] Start\n");
                    bot_loop();
                } else {
                    printf("[ERR] Calibrate first! (F9)\n");
                }
            }
            while (GetAsyncKeyState(VK_F8) & 0x8000) Sleep(10);
        }
        // F9 - calibration
        if (GetAsyncKeyState(VK_F9) & 0x8000) {
            calibrate();
            while (GetAsyncKeyState(VK_F9) & 0x8000) Sleep(10);
        }
        // F10 - exit
        if (GetAsyncKeyState(VK_F10) & 0x8000) break;

        Sleep(10);
    }

    release_all();
    timeEndPeriod(1);
    return 0;
}
