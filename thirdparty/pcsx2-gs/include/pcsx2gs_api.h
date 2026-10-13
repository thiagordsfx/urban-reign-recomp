// C interface of pcsx2gs.dll: PCSX2's GS (hardware renderer) as the port's GS back end (docs/GS_PCSX2.md, phase 2).
// urban.exe loads the DLL at run time (LoadLibrary + pcsx2gs_get_api), so it starts without it and its own ImGui
// never meets the copy inside the DLL. Every call except pcsx2gs_get_api must come from one thread (the GS thread).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCSX2GS_API_VERSION 4u

typedef struct Pcsx2GsOpenParams
{
    void *hwnd;             // window to render into (a child of the game window)
    uint32_t width;         // its client size
    uint32_t height;
    float scale;            // internal resolution multiplier (1 = native)
    const char *resources;  // folder holding PCSX2's "shaders/" (UTF-8)
    const char *cache;      // shader cache folder (UTF-8)
    uint32_t vsync;         // 0 = present without waiting, 1 = wait for the monitor (FIFO), 2 = mailbox
    uint32_t stretch;       // 1 = fill the window, 0 = keep the 4:3 aspect (black bars)
    // Image options, "key=value" separated by ';' (unknown keys are ignored; missing ones keep PCSX2's defaults):
    //   fxaa=0/1, texture_filtering=0 nearest / 1 forced bilinear / 2 as the PS2 / 3 forced except sprites,
    //   trilinear=-1 auto / 0 off / 1 as the PS2 / 2 forced, anisotropy=0/2/4/8/16,
    //   cas=0 off / 1 sharpen / 2 sharpen and resize, cas_sharpness=0..100,
    //   shadeboost=0/1, brightness/contrast/saturation/gamma=0..100 (50 = unchanged),
    //   tv_shader=0 none / 1 scanlines / 2 diagonal / 3 triangular / 4 wave / 5 Lottes CRT / 6 4xRGSS / 7 NxAGSS,
    //   present_filter=0 off / 1 smooth / 2 sharp, dithering=0 off / 1 scaled / 2 unscaled / 3 force 32 bit,
    //   blending=0..5 (minimum ... maximum),
    //   texture_dir=<folder holding replacements/> (HD textures, PCSX2's names and formats: PNG, DDS),
    //   texture_precache=0/1 (load all of them at start instead of as the game uses them)
    const char *options;
} Pcsx2GsOpenParams;

// Privileged registers at a vsync, in the order of the PS2 map (0x12000000 + 0x10 * i): PMODE, SMODE1, SMODE2, SRFSH,
// SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR; then CSR, IMR,
// BUSDIR, SIGLBLID (0x12001000, 1010, 1040, 1080).
typedef struct Pcsx2GsPrivRegs
{
    uint64_t regset1[15];
    uint64_t csr;
    uint64_t imr;
    uint64_t busdir;
    uint64_t siglblid;
} Pcsx2GsPrivRegs;

// In-game menu (version 4): a small immediate-mode UI on top of the image, drawn with the DLL's own ImGui. The draw
// callback set with set_menu runs on the GS thread at every present and builds the menu with these calls (UTF-8
// strings; the bool results say "clicked" / "changed" / "open"). Mods will draw their panels with the same table.
typedef struct Pcsx2GsUi
{
    uint32_t version;
    int (*begin_window)(const char *title, int *open, float width, float height); // centered the first time
    void (*end_window)(void);
    int (*begin_tab_bar)(const char *id);
    void (*end_tab_bar)(void);
    int (*begin_tab)(const char *label);
    void (*end_tab)(void);
    void (*text)(const char *text);
    void (*text_wrapped)(const char *text);
    void (*text_disabled)(const char *text);
    void (*text_colored)(uint32_t rgba, const char *text); // R in the low byte
    void (*heading)(const char *text);                     // larger text for titles
    int (*checkbox)(const char *label, int *value);
    int (*button)(const char *label);
    void (*same_line)(void);
    void (*separator)(void);
    void (*spacing)(void);
    int (*begin_child)(const char *id, float width, float height); // scrolling area (0 = fill)
    void (*end_child)(void);
    void (*push_id)(int id);
    void (*pop_id)(void);
    // version 2
    int (*combo)(const char *label, int *current, const char *const *items, int count); // "##id" hides the label
    int (*radio)(const char *label, int active);
    int (*small_button)(const char *label);
    void (*separator_text)(const char *text);
    void (*indent)(void);
    void (*unindent)(void);
    void (*set_next_width)(float width); // width of the next item (0 = default)
    void (*begin_disabled)(int disabled);
    void (*end_disabled)(void);
} Pcsx2GsUi;

typedef void (*Pcsx2GsMenuFn)(const Pcsx2GsUi *ui, void *user);

// Input for the menu, sent by the game window while it is open (any thread; the latest one is used at the next
// frame). Mouse in client pixels; keys and buttons are "held" bits.
typedef struct Pcsx2GsMenuInput
{
    float mouse_x;
    float mouse_y;
    uint32_t mouse_buttons; // bit 0 left, 1 right, 2 middle
    float wheel;            // notches since the last input (positive = up)
    uint32_t keys;          // PCSX2GS_KEY_* bits held
    uint32_t pad;           // PCSX2GS_PAD_* bits held (any controller)
} Pcsx2GsMenuInput;

enum
{
    PCSX2GS_KEY_UP = 1u << 0, PCSX2GS_KEY_DOWN = 1u << 1, PCSX2GS_KEY_LEFT = 1u << 2, PCSX2GS_KEY_RIGHT = 1u << 3,
    PCSX2GS_KEY_ENTER = 1u << 4, PCSX2GS_KEY_SPACE = 1u << 5, PCSX2GS_KEY_ESCAPE = 1u << 6, PCSX2GS_KEY_TAB = 1u << 7,
    PCSX2GS_KEY_SHIFT = 1u << 8, PCSX2GS_KEY_CTRL = 1u << 9,
    PCSX2GS_PAD_UP = 1u << 0, PCSX2GS_PAD_DOWN = 1u << 1, PCSX2GS_PAD_LEFT = 1u << 2, PCSX2GS_PAD_RIGHT = 1u << 3,
    PCSX2GS_PAD_ACCEPT = 1u << 4, PCSX2GS_PAD_BACK = 1u << 5, PCSX2GS_PAD_L1 = 1u << 6, PCSX2GS_PAD_R1 = 1u << 7,
};

typedef struct Pcsx2GsApi
{
    uint32_t version;
    // Creates the device and the renderer (DX11). Returns 0 on failure.
    int (*open)(const Pcsx2GsOpenParams *params);
    void (*close)(void);
    // GIF data in stream order (all paths merged), whole qwords; a tag may continue in the next call.
    void (*gif)(const uint8_t *data, uint32_t qwc);
    // End of a field: copies the registers into the GS's register block, composes the output and presents.
    void (*vsync)(const Pcsx2GsPrivRegs *regs);
    // GS reset (CSR.RESET).
    void (*reset)(void);
    // Local -> host transfer started by TRXDIR = 1 (already sent through gif): reads the next qwc qwords.
    void (*read_fifo)(uint8_t *dst, uint32_t qwc);
    // The window's client size changed.
    void (*resize)(uint32_t width, uint32_t height);
    // Internal resolution multiplier.
    void (*set_scale)(float scale);
    // Replaces the GS local memory (4 MB, the GS's own layout) and drops what the renderer derived from it (a save
    // state was loaded). The GS registers are reset too; games send them again with their next draws.
    void (*load_vram)(const uint8_t *data, uint32_t size);
    // Copies the GS local memory (4 MB) into dst, after writing back what only the GPU has (render targets): for a
    // save state. Returns the bytes copied.
    uint32_t (*save_vram)(uint8_t *dst, uint32_t size);
    // Reads a local -> host transfer's area as it is now (draws flushed, render targets written back), without the
    // GS's own transfer state: for reads the port serves from a cache. Returns the bytes written to dst.
    uint32_t (*read_local)(uint8_t *dst, uint32_t size, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
    // On-screen text drawn over the image at each present (UTF-8; NULL or "" = none): top = a message box at the top
    // left (top_rgba = its background color, R in the low byte; top_alpha fades it), bottom = a line at the bottom left
    // (the FPS counter). Unlike the other calls, any thread may call it.
    void (*set_osd)(const char *top, uint32_t top_rgba, float top_alpha, const char *bottom);
    // Debug: writes the image the GS shows now (internal resolution, before window scaling) as a 24-bit .bmp.
    int (*snapshot)(const char *path);
    // In-game menu: draw = NULL hides it. Any thread.
    void (*set_menu)(Pcsx2GsMenuFn draw, void *user);
    void (*menu_input)(const Pcsx2GsMenuInput *input);
} Pcsx2GsApi;

typedef const Pcsx2GsApi *(*Pcsx2GsGetApiFn)(uint32_t version);

#ifdef __cplusplus
}
#endif
