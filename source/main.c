/* A7 Import - visionneuse JPEG 3DS (.3dsx)
 * Lit les photos copiees sur la carte SD de la 3DS :
 *   sdmc:/photos (JPEG) et sdmc:/DCIM (2 niveaux)
 * Les fichiers ne sont jamais modifies : l'original reste intact.
 * Mode "ajuste" : apercu rapide (decode reduit).
 * Mode "1:1"    : pixels reels, taille originale, avec deplacement.
 */
#define _GNU_SOURCE
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <jpeglib.h>

#define MAX_PHOTOS 1024
#define PATH_LEN   192
#define SCR_W      400
#define SCR_H      240
#define PAN_STEP   100

static char paths[MAX_PHOTOS][PATH_LEN];
static int  count = 0;
static char iobuf[64 * 1024];

typedef struct { u8 *px; int w, h; int iw, ih; } Img;

/* ---------- liste des photos ---------- */
static int is_jpeg(const char *n) {
    const char *e = strrchr(n, '.');
    return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg"));
}

static void scan_dir(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && count < MAX_PHOTOS) {
        if (e->d_name[0] == '.') continue;
        char full[PATH_LEN];
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full) continue;
        if (e->d_type == DT_DIR) {
            if (depth > 0) scan_dir(full, depth - 1);
        } else if (is_jpeg(e->d_name)) {
            strcpy(paths[count++], full);
        }
    }
    closedir(d);
}

static int cmp(const void *a, const void *b) { return strcmp(a, b); }

static void scan(void) {
    count = 0;
    scan_dir("sdmc:/photos", 0);
    scan_dir("sdmc:/DCIM", 2);
    qsort(paths, count, PATH_LEN, cmp);
}

/* ---------- decodage JPEG ---------- */
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void on_err(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

/* apercu : decode directement reduit (1/1, 1/2, 1/4 ou 1/8) */
static int load_fit(const char *path, Img *o) {
    memset(o, 0, sizeof *o);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    setvbuf(f, iobuf, _IOFBF, sizeof iobuf);

    struct jpeg_decompress_struct ci;
    struct jerr je;
    u8 *volatile buf = NULL;
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = on_err;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        fclose(f);
        free(buf);
        memset(o, 0, sizeof *o);
        return 0;
    }
    jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci, f);
    jpeg_read_header(&ci, TRUE);
    o->iw = ci.image_width;
    o->ih = ci.image_height;

    int d = 1;
    while (d < 8 && (int)ci.image_width / (d * 2) >= SCR_W &&
                    (int)ci.image_height / (d * 2) >= SCR_H)
        d *= 2;
    ci.scale_num = 1;
    ci.scale_denom = d;
    ci.out_color_space = JCS_RGB;
    ci.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&ci);

    o->w = ci.output_width;
    o->h = ci.output_height;
    buf = malloc((size_t)o->w * o->h * 3);
    if (!buf) longjmp(je.jb, 1);
    while (ci.output_scanline < ci.output_height) {
        u8 *row = buf + (size_t)ci.output_scanline * o->w * 3;
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    fclose(f);
    o->px = buf;
    return 1;
}

/* 1:1 : ne decode que la fenetre 400x240 a la position (ox,oy) */
static int load_crop(const char *path, int *ox, int *oy, int center, Img *o) {
    memset(o, 0, sizeof *o);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    setvbuf(f, iobuf, _IOFBF, sizeof iobuf);

    struct jpeg_decompress_struct ci;
    struct jerr je;
    u8 *volatile buf = NULL;
    u8 *volatile line = NULL;
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = on_err;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        fclose(f);
        free(buf);
        free(line);
        memset(o, 0, sizeof *o);
        return 0;
    }
    jpeg_create_decompress(&ci);
    jpeg_stdio_src(&ci, f);
    jpeg_read_header(&ci, TRUE);

    int iw = ci.image_width, ih = ci.image_height;
    int ww = iw < SCR_W ? iw : SCR_W;
    int wh = ih < SCR_H ? ih : SCR_H;
    if (center) { *ox = (iw - ww) / 2; *oy = (ih - wh) / 2; }
    if (*ox > iw - ww) *ox = iw - ww;
    if (*oy > ih - wh) *oy = ih - wh;
    if (*ox < 0) *ox = 0;
    if (*oy < 0) *oy = 0;

    ci.out_color_space = JCS_RGB;
    ci.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&ci);

    JDIMENSION xo = *ox, wid = ww;
    jpeg_crop_scanline(&ci, &xo, &wid);
    if (*oy > 0) jpeg_skip_scanlines(&ci, *oy);

    line = malloc((size_t)wid * 3);
    buf  = malloc((size_t)ww * wh * 3);
    if (!line || !buf) longjmp(je.jb, 1);

    for (int y = 0; y < wh; y++) {
        u8 *r = line;
        jpeg_read_scanlines(&ci, &r, 1);
        memcpy(buf + (size_t)y * ww * 3, line + (size_t)(*ox - (int)xo) * 3, (size_t)ww * 3);
    }
    jpeg_abort_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(line);
    fclose(f);
    o->px = buf; o->w = ww; o->h = wh; o->iw = iw; o->ih = ih;
    return 1;
}

/* ---------- affichage (ecran du haut 400x240, BGR8, par colonnes) ---------- */
static void draw(const Img *im, int fit) {
    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
    memset(fb, 0, SCR_W * SCR_H * 3);
    if (!im || !im->px) return;
    int w = im->w, h = im->h;
    float s = 1.0f;
    if (fit) {
        s = (float)SCR_W / w;
        if ((float)SCR_H / h < s) s = (float)SCR_H / h;
    }
    int dw = (int)(w * s), dh = (int)(h * s);
    if (dw > SCR_W) dw = SCR_W;
    if (dh > SCR_H) dh = SCR_H;
    int ox = (SCR_W - dw) / 2, oy = (SCR_H - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = fit ? (int)(y / s) : y;
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < dw; x++) {
            int sx = fit ? (int)(x / s) : x;
            if (sx >= w) sx = w - 1;
            const u8 *p = im->px + ((size_t)sy * w + sx) * 3;
            u8 *q = fb + ((size_t)(x + ox) * SCR_H + (SCR_H - 1 - (y + oy))) * 3;
            q[0] = p[2]; q[1] = p[1]; q[2] = p[0];
        }
    }
}

static void present(const Img *im, int fit) {
    for (int i = 0; i < 2; i++) {          /* double buffer */
        draw(im, fit);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

static void say_loading(void) {
    printf("\x1b[2J\x1b[1;1HChargement...\n");
    gfxFlushBuffers();
    gspWaitForVBlank();
}

/* ---------- main ---------- */
int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    mkdir("sdmc:/photos", 0777);
    scan();

    int idx = 0, full = 0, ox = 0, oy = 0, center = 1, dirty = 1;
    Img im; memset(&im, 0, sizeof im);

    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        if (kd & KEY_START) break;

        if (count > 0) {
            int move = 0;
            if (kd & KEY_R) move = 1;
            if (kd & KEY_L) move = -1;
            if (!full) {
                if (kd & KEY_RIGHT) move = 1;
                if (kd & KEY_LEFT)  move = -1;
            }
            if (move) {
                idx = (idx + count + move) % count;
                center = 1; dirty = 1;
            }
            if (kd & KEY_A) { full = !full; center = 1; dirty = 1; }
            if ((kd & KEY_B) && full) { full = 0; dirty = 1; }

            if (full) {
                circlePosition cp; hidCircleRead(&cp);
                int dx = ((kh & KEY_RIGHT) || cp.dx >  40) - ((kh & KEY_LEFT) || cp.dx < -40);
                int dy = ((kh & KEY_DOWN)  || cp.dy < -40) - ((kh & KEY_UP)   || cp.dy >  40);
                if (dx || dy) { ox += dx * PAN_STEP; oy += dy * PAN_STEP; dirty = 1; }
            }
        }

        if (dirty) {
            int ok = 0;
            if (count > 0) {
                say_loading();
                ok = full ? load_crop(paths[idx], &ox, &oy, center, &im)
                          : load_fit(paths[idx], &im);
                center = 0;
            }
            present(ok ? &im : NULL, !full);

            printf("\x1b[2J\x1b[1;1HA7 Import\n\n");
            if (count == 0) {
                printf("Aucune photo.\nCopiez vos JPEG dans\nsdmc:/photos/\n");
            } else {
                const char *n = strrchr(paths[idx], '/');
                printf("%d/%d  %s\n", idx + 1, count, n ? n + 1 : paths[idx]);
                if (ok) {
                    printf("Original : %dx%d\n", im.iw, im.ih);
                    if (full) printf("Vue 1:1  pos %d,%d\n", ox, oy);
                    else      printf("Vue ajustee\n");
                } else printf("Erreur de lecture\n");
                printf("\n<-/->, L/R : photo\n");
                if (full) printf("Pad/croix : deplacer\nA/B : retour\n");
                else      printf("A : taille reelle (1:1)\n");
            }
            printf("START : quitter\n");
            free(im.px); im.px = NULL;
            dirty = 0;
        }
        gspWaitForVBlank();
    }

    free(im.px);
    gfxExit();
    return 0;
}
