/* c2048 – 2048 for Linux phones and desktops (GTK4 + libadwaita)
 * SPDX-License-Identifier: MIT */
#include <adwaita.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#define N 4
#define SLIDE_US 110000
#define POP_US   160000
#define MAX_UNDO 100

typedef enum { EASY, MEDIUM, EVIL, NDIFF } Diff;

static const struct {
    const char *id, *name, *desc;
    double p4;      /* probability of a 4; < 0 = adversarial placement */
    int undo;       /* undo depth */
} DIFFS[NDIFF] = {
    { "easy",   "Leicht",          "Es erscheinen nur Zweier.\nBis zu 100 Züge rückgängig.",     0.0, MAX_UNDO },
    { "hard",   "Mittel",          "Jede zweite neue Kachel ist eine Vier.\nKein Rückgängig.",   0.5, 0 },
    { "evil",   "Schwerstmöglich", "Ein Gegner setzt jede neue Kachel auf das\n"
                                   "für dich schlechteste Feld. Kein Rückgängig.",              -1.0, 0 },
};

typedef struct { int fr, fc, tr, tc, val; } Move;
typedef struct { int grid[N][N], score; } Snapshot;

typedef struct {
    AdwApplicationWindow *win;
    GtkWidget *area, *score_lbl, *best_lbl, *undo_btn, *hint;
    AdwWindowTitle *wtitle;
    GSimpleAction *undo_action, *diff_action;
    Diff diff;
    int grid[N][N], score, best;
    Snapshot hist[MAX_UNDO];
    int nhist;
    gboolean won, keep_going, over;
    /* animation */
    Move moves[N * N];
    int nmoves;
    gboolean merged[N][N];
    int new_r, new_c;
    gint64 anim_start;
    guint tick_id;
} Game;

/* ---------- pure board logic ---------- */

/* Slide `in` toward dir (0=left 1=right 2=up 3=down) into `out`.
 * moves/nmoves/merged may be NULL. Returns TRUE if anything changed. */
static gboolean slide(int in[N][N], int out[N][N], int dir, int *gain,
                      Move *moves, int *nmoves, gboolean merged[N][N]) {
    gboolean changed = FALSE;
    int g = 0, nm = 0;
    for (int line = 0; line < N; line++) {
        int rs[N], cs[N];               /* cells of this line, ordered toward the wall */
        for (int i = 0; i < N; i++) {
            switch (dir) {
            case 0: rs[i] = line; cs[i] = i;         break;
            case 1: rs[i] = line; cs[i] = N - 1 - i; break;
            case 2: rs[i] = i;    cs[i] = line;      break;
            default:rs[i] = N - 1 - i; cs[i] = line; break;
            }
        }
        int res[N] = {0}, pos = -1;
        gboolean can_merge = FALSE;
        for (int i = 0; i < N; i++) {
            int v = in[rs[i]][cs[i]];
            if (!v) continue;
            if (can_merge && res[pos] == v) {
                res[pos] *= 2;
                g += res[pos];
                if (merged) merged[rs[pos]][cs[pos]] = TRUE;
                can_merge = FALSE;
            } else {
                res[++pos] = v;
                can_merge = TRUE;
            }
            if (moves) moves[nm++] = (Move){ rs[i], cs[i], rs[pos], cs[pos], v };
        }
        for (int i = 0; i < N; i++) {
            if (in[rs[i]][cs[i]] != res[i]) changed = TRUE;
            out[rs[i]][cs[i]] = res[i];
        }
    }
    if (gain) *gain = g;
    if (nmoves) *nmoves = nm;
    return changed;
}

static gboolean can_move(int b[N][N]) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!b[r][c]) return TRUE;
            if (c + 1 < N && b[r][c] == b[r][c + 1]) return TRUE;
            if (r + 1 < N && b[r][c] == b[r + 1][c]) return TRUE;
        }
    return FALSE;
}

/* ---------- adversary ("Schwerstmöglich") ---------- */

/* How good a board is for the player: free cells, merge chances, monotonic rows/cols. */
static double heuristic(int b[N][N]) {
    double empty = 0, merges = 0, mono = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!b[r][c]) { empty++; continue; }
            if (c + 1 < N && b[r][c] == b[r][c + 1]) merges++;
            if (r + 1 < N && b[r][c] == b[r + 1][c]) merges++;
        }
    for (int i = 0; i < N; i++) {
        double inc_r = 0, dec_r = 0, inc_c = 0, dec_c = 0;
        for (int j = 0; j + 1 < N; j++) {
            double a = b[i][j] ? log2(b[i][j]) : 0, n = b[i][j + 1] ? log2(b[i][j + 1]) : 0;
            if (a > n) dec_r += a - n; else inc_r += n - a;
            a = b[j][i] ? log2(b[j][i]) : 0; n = b[j + 1][i] ? log2(b[j + 1][i]) : 0;
            if (a > n) dec_c += a - n; else inc_c += n - a;
        }
        mono -= MIN(inc_r, dec_r) + MIN(inc_c, dec_c);
    }
    return empty * 3.0 + merges * 1.5 + mono * 0.5;
}

static double adversary_value(int b[N][N], int depth);

/* Best the player can reach from b (player to move). */
static double player_value(int b[N][N], int depth) {
    double best = -1e9;
    gboolean any = FALSE;
    for (int dir = 0; dir < 4; dir++) {
        int out[N][N];
        if (!slide(b, out, dir, NULL, NULL, NULL, NULL)) continue;
        any = TRUE;
        double v = depth > 1 ? adversary_value(out, depth - 1) : heuristic(out);
        if (v > best) best = v;
    }
    return any ? best : -1e6;   /* no move left = game over */
}

/* Worst tile placement for the player. */
static double adversary_value(int b[N][N], int depth) {
    double worst = 1e9;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (b[r][c]) continue;
            for (int v = 2; v <= 4; v += 2) {
                b[r][c] = v;
                double val = player_value(b, depth);
                b[r][c] = 0;
                if (val < worst) worst = val;
            }
        }
    return worst;
}

static void adversary_place(int b[N][N], int *rr, int *cc, int *vv) {
    double worst = 1e18;
    int cand[2 * N * N][3], ncand = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (b[r][c]) continue;
            for (int v = 2; v <= 4; v += 2) {
                b[r][c] = v;
                double val = player_value(b, 2);
                b[r][c] = 0;
                if (val < worst - 1e-9) { worst = val; ncand = 0; }
                if (val < worst + 1e-9) {
                    cand[ncand][0] = r; cand[ncand][1] = c; cand[ncand][2] = v; ncand++;
                }
            }
        }
    int i = g_random_int_range(0, ncand);   /* random among equally bad choices */
    *rr = cand[i][0]; *cc = cand[i][1]; *vv = cand[i][2];
}

/* ---------- persistence: one group per difficulty ---------- */

static char *state_path(void) {
    char *path = g_build_filename(g_get_user_data_dir(), "c2048", "state.ini", NULL);
    /* take over saves from the pre-release name "game2048" */
    g_autofree char *old = g_build_filename(g_get_user_data_dir(), "game2048", NULL);
    g_autofree char *dir = g_path_get_dirname(path);
    if (!g_file_test(dir, G_FILE_TEST_EXISTS) && g_file_test(old, G_FILE_TEST_IS_DIR))
        g_rename(old, dir);
    return path;
}

static void save_state(Game *g) {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_autofree char *path = state_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_remove_group(kf, "game", NULL);      /* obsolete levels */
    g_key_file_remove_group(kf, "medium", NULL);
    const char *grp = DIFFS[g->diff].id;
    int flat[N * N];
    memcpy(flat, g->grid, sizeof flat);
    g_key_file_set_integer_list(kf, grp, "grid", flat, N * N);
    g_key_file_set_integer(kf, grp, "score", g->score);
    g_key_file_set_integer(kf, grp, "best", g->best);
    g_key_file_set_boolean(kf, grp, "keep_going", g->keep_going);
    g_key_file_set_string(kf, "settings", "difficulty", grp);
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_key_file_save_to_file(kf, path, NULL);
}

static Diff load_difficulty(void) {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_autofree char *path = state_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_autofree char *id = g_key_file_get_string(kf, "settings", "difficulty", NULL);
        for (int d = 0; d < NDIFF; d++)
            if (!g_strcmp0(id, DIFFS[d].id)) return d;
    }
    return MEDIUM;
}

/* Loads the saved game of g->diff. Returns FALSE if there is none worth resuming. */
static gboolean load_state(Game *g) {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_autofree char *path = state_path();
    const char *grp = DIFFS[g->diff].id;
    g->best = 0;
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        return FALSE;
    g->best = g_key_file_get_integer(kf, grp, "best", NULL);
    gsize len = 0;
    g_autofree int *flat = g_key_file_get_integer_list(kf, grp, "grid", &len, NULL);
    if (!flat || len != N * N)
        return FALSE;
    memcpy(g->grid, flat, sizeof g->grid);
    g->score = g_key_file_get_integer(kf, grp, "score", NULL);
    g->keep_going = g_key_file_get_boolean(kf, grp, "keep_going", NULL);
    g->won = g->over = FALSE;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (g->grid[r][c] >= 2048) g->won = TRUE;
    return can_move(g->grid);
}

/* ---------- game ---------- */

static void spawn_tile(Game *g) {
    int r, c, v;
    if (DIFFS[g->diff].p4 < 0) {
        adversary_place(g->grid, &r, &c, &v);
    } else {
        int free_cells[N * N], n = 0;
        for (int i = 0; i < N * N; i++)
            if (g->grid[i / N][i % N] == 0) free_cells[n++] = i;
        if (!n) return;
        int i = free_cells[g_random_int_range(0, n)];
        r = i / N; c = i % N;
        v = g_random_double() < DIFFS[g->diff].p4 ? 4 : 2;
    }
    g->grid[r][c] = v;
    g->new_r = r; g->new_c = c;
}

static void update_ui(Game *g) {
    char buf[32];
    g_snprintf(buf, sizeof buf, "%d", g->score);
    gtk_label_set_text(GTK_LABEL(g->score_lbl), buf);
    g_snprintf(buf, sizeof buf, "%d", g->best);
    gtk_label_set_text(GTK_LABEL(g->best_lbl), buf);
    g_simple_action_set_enabled(g->undo_action, g->nhist > 0);
    gtk_widget_set_visible(g->undo_btn, DIFFS[g->diff].undo > 0);
    adw_window_title_set_subtitle(g->wtitle, DIFFS[g->diff].name);
    gtk_label_set_text(GTK_LABEL(g->hint), DIFFS[g->diff].desc);
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
    Game *g = data;
    gtk_widget_queue_draw(w);
    if (gdk_frame_clock_get_frame_time(clock) - g->anim_start > SLIDE_US + POP_US) {
        g->tick_id = 0;
        g->nmoves = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_anim(Game *g) {
    g->anim_start = g_get_monotonic_time();
    if (!g->tick_id)
        g->tick_id = gtk_widget_add_tick_callback(g->area, on_tick, g, NULL);
}

static void reset_anim(Game *g) {
    g->nmoves = 0;
    g->new_r = -1;
    memset(g->merged, 0, sizeof g->merged);
}

static void new_game(Game *g) {
    memset(g->grid, 0, sizeof g->grid);
    reset_anim(g);
    g->score = g->nhist = 0;
    g->won = g->keep_going = g->over = FALSE;
    spawn_tile(g);
    spawn_tile(g);
    g->new_r = -1;
    update_ui(g);
    save_state(g);
    start_anim(g);
}

static void show_dialog(Game *g, gboolean win);

static void do_move(Game *g, int dir) {
    if (g->over) return;
    int out[N][N], gain, nm;
    memset(g->merged, 0, sizeof g->merged);
    if (!slide(g->grid, out, dir, &gain, g->moves, &nm, g->merged)) return;

    int depth = DIFFS[g->diff].undo;
    if (depth > 0) {
        if (g->nhist == depth) {
            memmove(g->hist, g->hist + 1, sizeof(Snapshot) * (depth - 1));
            g->nhist--;
        }
        memcpy(g->hist[g->nhist].grid, g->grid, sizeof g->grid);
        g->hist[g->nhist++].score = g->score;
    }

    memcpy(g->grid, out, sizeof out);
    g->nmoves = nm;
    g->score += gain;
    spawn_tile(g);
    if (g->score > g->best) g->best = g->score;
    update_ui(g);
    save_state(g);
    start_anim(g);

    if (!g->won)
        for (int r = 0; r < N; r++)
            for (int c = 0; c < N; c++)
                if (g->grid[r][c] >= 2048) g->won = TRUE;
    if (g->won && !g->keep_going) { show_dialog(g, TRUE); return; }
    if (!can_move(g->grid)) { g->over = TRUE; show_dialog(g, FALSE); }
}

static void undo(Game *g) {
    if (!g->nhist) return;
    Snapshot *s = &g->hist[--g->nhist];
    memcpy(g->grid, s->grid, sizeof g->grid);
    g->score = s->score;
    g->over = FALSE;
    reset_anim(g);
    update_ui(g);
    save_state(g);
    gtk_widget_queue_draw(g->area);
}

static void set_difficulty(Game *g, Diff d) {
    if (g->diff == d) return;
    save_state(g);          /* keep the game of the old level */
    g->diff = d;
    g->nhist = 0;
    reset_anim(g);
    if (load_state(g)) {
        update_ui(g);
        save_state(g);      /* remember chosen difficulty */
        start_anim(g);
    } else {
        new_game(g);
    }
}

/* ---------- dialogs ---------- */

static void on_dialog_response(AdwAlertDialog *d, const char *resp, gpointer data) {
    Game *g = data;
    (void)d;
    if (!g_strcmp0(resp, "new")) new_game(g);
    else if (!g_strcmp0(resp, "continue")) { g->keep_going = TRUE; save_state(g); }
    else if (!g_strcmp0(resp, "undo")) undo(g);
}

static void show_dialog(Game *g, gboolean win) {
    AdwDialog *d = adw_alert_dialog_new(win ? "Gewonnen! 🎉" : "Keine Züge mehr", NULL);
    AdwAlertDialog *a = ADW_ALERT_DIALOG(d);
    adw_alert_dialog_format_body(a, "%s – %d Punkte", DIFFS[g->diff].name, g->score);
    if (win) {
        adw_alert_dialog_add_responses(a, "new", "Neues Spiel", "continue", "Weiterspielen", NULL);
        adw_alert_dialog_set_default_response(a, "continue");
        adw_alert_dialog_set_close_response(a, "continue");
    } else if (g->nhist > 0) {
        adw_alert_dialog_add_responses(a, "undo", "Rückgängig", "new", "Neues Spiel", NULL);
        adw_alert_dialog_set_default_response(a, "new");
        adw_alert_dialog_set_close_response(a, "undo");
    } else {
        adw_alert_dialog_add_responses(a, "close", "Schließen", "new", "Neues Spiel", NULL);
        adw_alert_dialog_set_default_response(a, "new");
        adw_alert_dialog_set_close_response(a, "close");
    }
    adw_alert_dialog_set_response_appearance(a, "new", ADW_RESPONSE_SUGGESTED);
    g_signal_connect(d, "response", G_CALLBACK(on_dialog_response), g);
    adw_dialog_present(d, GTK_WIDGET(g->win));
}

/* ---------- drawing ---------- */

static void tile_colors(int v, double bg[3], double fg[3]) {
    static const guint32 cols[] = {
        0xeee4da, 0xede0c8, 0xf2b179, 0xf59563, 0xf67c5f, 0xf65e3b,
        0xedcf72, 0xedcc61, 0xedc850, 0xedc53f, 0xedc22e };
    int e = 0;
    while ((1 << (e + 1)) <= v) e++;          /* 2 -> 1 */
    guint32 c = e - 1 < (int)G_N_ELEMENTS(cols) ? cols[e - 1] : 0x3c3a32;
    bg[0] = ((c >> 16) & 0xff) / 255.0;
    bg[1] = ((c >> 8) & 0xff) / 255.0;
    bg[2] = (c & 0xff) / 255.0;
    guint32 f = v <= 4 ? 0x776e65 : 0xf9f6f2;
    fg[0] = ((f >> 16) & 0xff) / 255.0;
    fg[1] = ((f >> 8) & 0xff) / 255.0;
    fg[2] = (f & 0xff) / 255.0;
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
    cairo_close_path(cr);
}

static void draw_tile(cairo_t *cr, GtkWidget *w, double x, double y, double s, double scale, int v) {
    double bg[3], fg[3];
    tile_colors(v, bg, fg);
    double ss = s * scale, ox = x + (s - ss) / 2, oy = y + (s - ss) / 2;
    cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
    rounded_rect(cr, ox, oy, ss, ss, ss * 0.08);
    cairo_fill(cr);

    char txt[16];
    g_snprintf(txt, sizeof txt, "%d", v);
    int digits = strlen(txt);
    double fs = ss * (digits <= 2 ? 0.45 : digits == 3 ? 0.36 : digits == 4 ? 0.28 : 0.22);
    PangoLayout *pl = gtk_widget_create_pango_layout(w, txt);
    PangoFontDescription *fd = pango_font_description_from_string("Sans Bold");
    pango_font_description_set_absolute_size(fd, fs * PANGO_SCALE);
    pango_layout_set_font_description(pl, fd);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    cairo_set_source_rgb(cr, fg[0], fg[1], fg[2]);
    cairo_move_to(cr, ox + (ss - tw) / 2.0, oy + (ss - th) / 2.0);
    pango_cairo_show_layout(cr, pl);
    pango_font_description_free(fd);
    g_object_unref(pl);
}

static void draw_board(GtkDrawingArea *a, cairo_t *cr, int W, int H, gpointer data) {
    Game *g = data;
    GtkWidget *w = GTK_WIDGET(a);
    double size = MIN(W, H);
    double bx = (W - size) / 2, by = (H - size) / 2;
    double gap = size * 0.03, cell = (size - gap * (N + 1)) / N;

    cairo_set_source_rgb(cr, 0xbb / 255.0, 0xad / 255.0, 0xa0 / 255.0);
    rounded_rect(cr, bx, by, size, size, size * 0.03);
    cairo_fill(cr);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            cairo_set_source_rgba(cr, 0.93, 0.89, 0.85, 0.35);
            rounded_rect(cr, bx + gap + c * (cell + gap), by + gap + r * (cell + gap),
                         cell, cell, cell * 0.08);
            cairo_fill(cr);
        }

    gint64 t = g->tick_id ? g_get_monotonic_time() - g->anim_start : SLIDE_US + POP_US;

    if (g->nmoves && t < SLIDE_US) {               /* sliding phase */
        double p = (double)t / SLIDE_US;
        p = 1 - (1 - p) * (1 - p);                 /* ease-out */
        for (int i = 0; i < g->nmoves; i++) {
            Move *m = &g->moves[i];
            double r = m->fr + (m->tr - m->fr) * p, c = m->fc + (m->tc - m->fc) * p;
            draw_tile(cr, w, bx + gap + c * (cell + gap), by + gap + r * (cell + gap),
                      cell, 1.0, m->val);
        }
        return;
    }

    double pt = g->nmoves ? (double)(t - SLIDE_US) / POP_US : (double)t / (SLIDE_US + POP_US);
    pt = CLAMP(pt, 0, 1);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            int v = g->grid[r][c];
            if (!v) continue;
            double scale = 1.0;
            if (r == g->new_r && c == g->new_c) scale = pt;
            else if (g->merged[r][c]) scale = 1.0 + 0.15 * sin(pt * G_PI);
            if (scale <= 0.01) continue;
            draw_tile(cr, w, bx + gap + c * (cell + gap), by + gap + r * (cell + gap),
                      cell, scale, v);
        }
}

/* ---------- input & actions ---------- */

static gboolean on_key(GtkEventControllerKey *k, guint key, guint code,
                       GdkModifierType mods, gpointer data) {
    Game *g = data;
    (void)k; (void)code;
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK)) return FALSE;
    switch (key) {
    case GDK_KEY_Left:  case GDK_KEY_a: case GDK_KEY_h: do_move(g, 0); return TRUE;
    case GDK_KEY_Right: case GDK_KEY_d: case GDK_KEY_l: do_move(g, 1); return TRUE;
    case GDK_KEY_Up:    case GDK_KEY_w: case GDK_KEY_k: do_move(g, 2); return TRUE;
    case GDK_KEY_Down:  case GDK_KEY_s: case GDK_KEY_j: do_move(g, 3); return TRUE;
    }
    return FALSE;
}

static void on_drag_end(GtkGestureDrag *d, double dx, double dy, gpointer data) {
    Game *g = data;
    (void)d;
    if (MAX(fabs(dx), fabs(dy)) < 30) return;
    if (fabs(dx) > fabs(dy)) do_move(g, dx < 0 ? 0 : 1);
    else                     do_move(g, dy < 0 ? 2 : 3);
}

static void act_new(GSimpleAction *a, GVariant *p, gpointer g)  { (void)a; (void)p; new_game(g); }
static void act_undo(GSimpleAction *a, GVariant *p, gpointer g) { (void)a; (void)p; undo(g); }

static void act_difficulty(GSimpleAction *a, GVariant *value, gpointer data) {
    Game *g = data;
    const char *id = g_variant_get_string(value, NULL);
    for (int d = 0; d < NDIFF; d++)
        if (!g_strcmp0(id, DIFFS[d].id)) {
            g_simple_action_set_state(a, value);
            set_difficulty(g, d);
        }
}

/* ---------- UI ---------- */

static GtkWidget *score_box(const char *caption, GtkWidget **value) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "scorebox");
    GtkWidget *cap = gtk_label_new(caption);
    gtk_widget_add_css_class(cap, "caption");
    *value = gtk_label_new("0");
    gtk_widget_add_css_class(*value, "value");
    gtk_box_append(GTK_BOX(box), cap);
    gtk_box_append(GTK_BOX(box), *value);
    return box;
}

static GMenuModel *build_menu(void) {
    GMenu *menu = g_menu_new(), *levels = g_menu_new(), *game = g_menu_new();
    for (int d = 0; d < NDIFF; d++) {
        GMenuItem *it = g_menu_item_new(DIFFS[d].name, NULL);
        g_menu_item_set_action_and_target_value(it, "win.difficulty",
                                                g_variant_new_string(DIFFS[d].id));
        g_menu_append_item(levels, it);
        g_object_unref(it);
    }
    g_menu_append(game, "Neues Spiel", "win.new-game");
    g_menu_append_section(menu, "Schwierigkeit", G_MENU_MODEL(levels));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(game));
    g_object_unref(levels);
    g_object_unref(game);
    return G_MENU_MODEL(menu);
}

static void activate(GtkApplication *app, gpointer data) {
    Game *g = data;
    if (g->win) { gtk_window_present(GTK_WINDOW(g->win)); return; }

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/io/github/ElTentakel/c2048/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g->diff = load_difficulty();
    g->win = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(g->win), "c2048");
    gtk_window_set_default_size(GTK_WINDOW(g->win), 360, 640);

    /* actions */
    GSimpleAction *newa = g_simple_action_new("new-game", NULL);
    g_signal_connect(newa, "activate", G_CALLBACK(act_new), g);
    g->undo_action = g_simple_action_new("undo", NULL);
    g_signal_connect(g->undo_action, "activate", G_CALLBACK(act_undo), g);
    g->diff_action = g_simple_action_new_stateful("difficulty", G_VARIANT_TYPE_STRING,
                                                  g_variant_new_string(DIFFS[g->diff].id));
    g_signal_connect(g->diff_action, "change-state", G_CALLBACK(act_difficulty), g);
    g_action_map_add_action(G_ACTION_MAP(g->win), G_ACTION(newa));
    g_action_map_add_action(G_ACTION_MAP(g->win), G_ACTION(g->undo_action));
    g_action_map_add_action(G_ACTION_MAP(g->win), G_ACTION(g->diff_action));
    gtk_application_set_accels_for_action(app, "win.new-game", (const char *[]){ "<Control>n", NULL });
    gtk_application_set_accels_for_action(app, "win.undo", (const char *[]){ "<Control>z", NULL });

    /* header */
    GtkWidget *header = adw_header_bar_new();
    g->wtitle = ADW_WINDOW_TITLE(adw_window_title_new("c2048", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(g->wtitle));
    GtkWidget *newb = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(newb, "Neues Spiel (Strg+N)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(newb), "win.new-game");
    g->undo_btn = gtk_button_new_from_icon_name("edit-undo-symbolic");
    gtk_widget_set_tooltip_text(g->undo_btn, "Rückgängig (Strg+Z)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(g->undo_btn), "win.undo");
    GtkWidget *menub = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menub), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menub, "Schwierigkeit");
    GMenuModel *menu = build_menu();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menub), menu);
    g_object_unref(menu);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), newb);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), g->undo_btn);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menub);

    /* content */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title = gtk_label_new("2048");
    gtk_widget_add_css_class(title, "gametitle");
    gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(top), title);
    gtk_box_append(GTK_BOX(top), spacer);
    gtk_box_append(GTK_BOX(top), score_box("PUNKTE", &g->score_lbl));
    gtk_box_append(GTK_BOX(top), score_box("REKORD", &g->best_lbl));

    g->area = gtk_drawing_area_new();
    gtk_widget_set_vexpand(g->area, TRUE);
    gtk_widget_set_hexpand(g->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g->area), draw_board, g, NULL);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), g);
    gtk_widget_add_controller(g->area, GTK_EVENT_CONTROLLER(drag));

    g->hint = gtk_label_new("");
    gtk_widget_add_css_class(g->hint, "dim-label");
    gtk_label_set_justify(GTK_LABEL(g->hint), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(g->hint), TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), g->area);
    gtk_box_append(GTK_BOX(box), g->hint);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 560);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);

    GtkWidget *view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), clamp);
    adw_application_window_set_content(g->win, view);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), g);
    gtk_widget_add_controller(GTK_WIDGET(g->win), keys);

    reset_anim(g);
    if (load_state(g)) {
        update_ui(g);
        start_anim(g);
    } else {
        new_game(g);
    }
    gtk_window_present(GTK_WINDOW(g->win));
}

int main(int argc, char **argv) {
    static Game g;
    AdwApplication *app = adw_application_new("io.github.ElTentakel.c2048", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &g);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
