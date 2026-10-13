/* ExampleMod: a small mod for the Urban Reign PC port, as a starting point.
 *
 * - Two patch groups built into the DLL (PCSX2 .pnach lines): the F2 menu shows them as checkboxes.
 * - Its own tab: reads P1's and P2's health live and has a button that refills it once.
 * - A frame callback that counts frames (runs on the game thread at every vblank).
 *
 * Build: see modsdk/README.md. Install: game\mods\ExampleMod\ExampleMod.dll + mod.json, then switch it on in the
 * launcher's Mods tab.
 */
#include "urban_mod_api.h"

#include <stdio.h>

/* Each fighter's block is 0x9750 bytes; the current health is the first 16-bit value (P1 at 0x5A5FC8). */
#define P1_HEALTH 0x005A5FC8u
#define FIGHTER_STRIDE 0x9750u
#define FULL_HEALTH 1500

static const UrbanModHost *g_host;
static unsigned long g_frames;

static const char kInfiniteP1[] = "patch=1,EE,105A5FC8,extended,000005DC // P1 health = 1500\n";
static const char kInfiniteP2[] = "patch=1,EE,105AF718,extended,000005DC // P2 health = 1500\n";

static const char *tr(const char *pt, const char *en)
{
    return g_host->language[0] == 'p' ? pt : en;
}

static void on_frame(void *user)
{
    (void)user;
    ++g_frames;
}

static void draw_tab(const UrbanModUi *ui, void *user)
{
    char line[128];
    unsigned player;
    (void)user;
    ui->heading(tr("Ao vivo", "Live"));
    for (player = 0; player < 2; ++player)
    {
        unsigned short health = 0;
        g_host->read(P1_HEALTH + player * FIGHTER_STRIDE, &health, sizeof(health));
        snprintf(line, sizeof(line), tr("P%u: vida %u", "P%u: health %u"), player + 1, (unsigned)health);
        ui->text(line);
    }
    snprintf(line, sizeof(line), tr("Quadros desde que o mod abriu: %lu", "Frames since the mod started: %lu"), g_frames);
    ui->text_disabled(line);
    ui->spacing();
    if (ui->button(tr("Encher a vida do P1 e do P2 agora", "Refill P1's and P2's health now")))
    {
        const unsigned short full = FULL_HEALTH;
        g_host->write(P1_HEALTH, &full, sizeof(full));
        g_host->write(P1_HEALTH + FIGHTER_STRIDE, &full, sizeof(full));
        g_host->log("health refilled");
    }
}

URBAN_MOD_EXPORT int urban_mod_init(const UrbanModHost *host)
{
    if (host->version < 1u)
        return 0;
    g_host = host;
    host->add_patches(host->language[0] == 'p' ? "Vida infinita (P1)" : "Infinite health (P1)",
                      host->language[0] == 'p' ? "Mantém a vida do P1 cheia." : "Keeps P1's health full.", kInfiniteP1, 0, 1);
    host->add_patches(host->language[0] == 'p' ? "Vida infinita (P2)" : "Infinite health (P2)",
                      host->language[0] == 'p' ? "Mantém a vida do P2 cheia." : "Keeps P2's health full.", kInfiniteP2, 0, 1);
    host->set_tab(draw_tab, 0);
    host->on_frame(on_frame, 0);
    host->log("ExampleMod ready");
    return 1;
}
