// A hosted graphical program exercising the juampiOS platform layer end to end:
// framebuffer present, raw key input (press/release), and the ms clock. Bounces
// a ball; ESC or ~4 seconds quits. Proves the fullscreen lifecycle before Doom.

#include <juampi.h>
#include <stdio.h>

#define W 640
#define H 400
static unsigned fb[W * H]; // frame buffer (0x00RRGGBB), in the program's .bss

int main(void)
{
    fprintf(stderr, "GFXDEMO_START\n");
    int bx = W / 2, by = H / 2, vx = 4, vy = 3, r = 28;
    unsigned long start = juampi_ticks_ms();

    for (;;) {
        int pressed;
        int k = juampi_getkey(&pressed);
        if (k == JK_ESC && pressed) {
            break;
        }

        for (int i = 0; i < W * H; i++) {
            fb[i] = 0x101018; // dark background
        }
        bx += vx;
        by += vy;
        if (bx < r || bx > W - r) {
            vx = -vx;
        }
        if (by < r || by > H - r) {
            vy = -vy;
        }
        for (int y = -r; y <= r; y++) {
            for (int x = -r; x <= r; x++) {
                if (x * x + y * y > r * r) {
                    continue;
                }
                int px = bx + x, py = by + y;
                if (px >= 0 && px < W && py >= 0 && py < H) {
                    fb[py * W + px] = 0xff6040; // orange ball
                }
            }
        }
        juampi_fb_present(fb, W, H);

        unsigned long t = juampi_ticks_ms(); // ~30 fps
        while (juampi_ticks_ms() - t < 33) {
        }
        if (juampi_ticks_ms() - start > 4000) {
            break; // auto-quit so the smoke test terminates
        }
    }

    printf("GFXDEMO_OK\n");
    fflush(stdout);
    return 0;
}
