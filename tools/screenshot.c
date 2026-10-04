/* Renders store screenshots of c2048. Not part of the app.
 * Usage: see tools/screenshots.sh */
#define main app_main
#include "../src/main.c"
#undef main
static Game *G; static const char *OUT; static int dark;
static gboolean grab(gpointer d) {
    (void)d;
    GtkWidget *w = GTK_WIDGET(G->win);
    int W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    GdkPaintable *p = gtk_widget_paintable_new(w);
    GtkSnapshot *s = gtk_snapshot_new();
    gtk_snapshot_scale(s, 2, 2);
    gdk_paintable_snapshot(p, s, W, H);
    GskRenderNode *n = gtk_snapshot_free_to_node(s);
    GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(G->win));
    GdkTexture *t = gsk_renderer_render_texture(r, n, &GRAPHENE_RECT_INIT(0, 0, W * 2, H * 2));
    gdk_texture_save_to_png(t, OUT);
    g_print("%s %dx%d\n", OUT, W * 2, H * 2);
    g_application_quit(g_application_get_default());
    return G_SOURCE_REMOVE;
}
static void on_act(GtkApplication *a, gpointer d) {
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
    gtk_window_set_default_size(GTK_WINDOW(((Game *)d)->win ? ((Game *)d)->win : NULL), 360, 720);
    (void)a;
}
int main(int argc, char **argv) {
    static Game g; G = &g; OUT = argv[1]; dark = argc > 2;
    AdwApplication *app = adw_application_new(NULL, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &g);
    g_signal_connect_after(app, "activate", G_CALLBACK(on_act), &g);
    g_timeout_add(3000, grab, NULL);
    return g_application_run(G_APPLICATION(app), 1, argv);
}
