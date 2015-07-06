#if 0
# run as a shell script to compile
exec gcc $0 -o termi \
  -O3 -Wall -Werror -Wextra -Wno-unused-parameter \
  `pkg-config --cflags --libs vte` \
  `pkg-config --cflags --libs gdk-pixbuf-2.0`
#endif

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <vte/vte.h>
#include <vte/pty.h>
#include <gdk/gdkkeysyms.h>

#define VERSION  "1.0"
/// Name of the application (used for config file name, etc.).
#define PROGRAM_NAME  "termi"
#define TERMI_ICON_NAME  "terminal"
#define TERMI_QUARK_STR  PROGRAM_NAME
#define TERMI_CFGGRP_GENERAL  "General"
#define TERMI_CFGGRP_KEYS     "Keys"


/// Data for a single termi's tab.
typedef struct {
  VteTerminal *vte;   ///< Terminal widget.
  GtkLabel *lbl;      ///< Tabl label
  GPid pid;           ///< Child PID.
  int uri_regex_tag;

} TermiTab;

/// Key binding.
typedef struct {
  GdkModifierType mod;
  guint key;
} TermiKeyBinding;


#if VTE_CHECK_VERSION(0,26,0)
#define TERMI_KEY_BINDINGS_FIND_APPLY(expr) \
  expr(find,      "Find",        GDK_CONTROL_MASK|GDK_SHIFT_MASK, 'f') \
  expr(find_next, "FindNext",    GDK_CONTROL_MASK|GDK_SHIFT_MASK, 'n') \
  expr(find_prev, "FindPrev",    GDK_CONTROL_MASK|GDK_SHIFT_MASK, 'p')
#endif

/// Apply code on all configurable key bindings.
#define TERMI_KEY_BINDINGS_APPLY(expr) \
  expr(new_tab,   "NewTab",      GDK_CONTROL_MASK|GDK_SHIFT_MASK, 't') \
  expr(left_tab,  "LeftTab",     GDK_CONTROL_MASK, GDK_Page_Up) \
  expr(right_tab, "RightTab",    GDK_CONTROL_MASK, GDK_Page_Down) \
  expr(prev_tab,  "PreviousTab", GDK_CONTROL_MASK, GDK_Tab) \
  expr(copy,      "Copy",        GDK_CONTROL_MASK|GDK_SHIFT_MASK, 'c') \
  expr(paste,     "Paste",       GDK_CONTROL_MASK|GDK_SHIFT_MASK, 'v') \
  TERMI_KEY_BINDINGS_FIND_APPLY(expr)


/// Global data of the termi instance.
typedef struct {
  GQuark quark;              ///< Application quark.
  GKeyFile *cfg;             ///< Current configuration.
  gchar *cfg_file;           ///< Configuration file.
  GtkWindow *winmain;        ///< Main window.
  GtkNotebook *notebook;     ///< Notebook (with tabs).
  TermiTab *prev_tab;        ///< Previously selected tab.
  TermiTab *cur_tab;         ///< Currently selected tab.
  gboolean quitting;         ///< True when quitting.
  guint label_nb;            ///< Tab label number (starting at 1).
  GRegex *uri_regex;         ///< Regex object for underlined URIs.
  gchar *menu_uri;           ///< Allocated URI for the current popup menu.
#if VTE_CHECK_VERSION(0,26,0)
  GRegex *search_regex;        ///< Current search regex.
#endif

  // configuration
  gboolean save_conf_at_exit;
  gboolean show_single_tab;  ///< Show the tabbar even when there is only one tab.
  gboolean force_tab_title;  ///< Terminal is not allowed to change tab title.
  gboolean audible_bell;
  gboolean visible_bell;
  gboolean blink_mode;  // note: don't support the 3-state mode, on purpose
  guint buffer_lines;
  gchar *word_chars;
#if VTE_CHECK_VERSION(0,26,0)
  gboolean search_wrap;
#endif
  PangoFontDescription *vte_font;  ///< Font for terminals.
  GdkColor vte_fg_color;
  GdkColor vte_bg_color;
  GdkColor vte_cursor_color;
  gboolean vte_cursor_color_default;

  // key bindings
#define TERMI_DEFINE_KB(n,k,dm,dk)   TermiKeyBinding kb_##n;
  TERMI_KEY_BINDINGS_APPLY(TERMI_DEFINE_KB)
#undef TERMI_DEFINE_KB

} TermiInstance;


static TermiInstance termi = {
  .quark     = 0,
  .cfg       = NULL,
  .cfg_file  = NULL,
  .winmain   = NULL,
  .notebook  = NULL,
  .prev_tab  = NULL,
  .cur_tab   = NULL,
  .quitting  = FALSE,
  .label_nb  = 1,
  .uri_regex = NULL,
  .menu_uri  = NULL,
#if VTE_CHECK_VERSION(0,26,0)
  .search_regex = NULL,
#endif

   // binding and conf values initialized in termi_conf_load()
};



/** @brief Initialize main window.
 * @note The main window is not shown.
 */
static void termi_winmain_init(void);
/// Close and exit.
static void termi_quit(void);

/// Load or reload configuration file.
static void termi_conf_load(void);
/// Save configuration file.
static void termi_conf_save(void);
/** @brief Helper method to loading key bindings.
 *
 * \e kb must be filled with the default value.
 */
static void termi_conf_load_keys(const char *name, TermiKeyBinding *kb);
/// Helper method to load boolean values.
static gboolean termi_conf_load_bool(const char *grp, const char *name, gboolean def);
/** @brief Helper method to load colors.
 * @return \e true if a valid color has been provided and \e color updated.
 */
static gboolean termi_conf_load_color(const char *grp, const char *name, GdkColor *color);
/** @brief Helper to save colors.
 *
 * If \e color is NULL, set value to an empty string.
 * @todo Don't replace values if color did not changed, to preserve color names.
 */
static void termi_conf_save_color(const char *grp, const char *name, const GdkColor *color);

/** @brief Display the popup menu.
 *
 * If \e full is FALSE, don't display options the first group of items (URI,
 * copy/paste).
 */
static void termi_menu_popup(TermiTab *tab, const GdkEvent *ev, gboolean full);
/** @brief Update terminal font on all tabs and resize them.
 * @note It is safe to set \e font to \e termi.vte_font.
 */
static void termi_set_vte_font(PangoFontDescription *font);
/** @brief Change terminal colors on all tabs.
 *
 * If \e cursor is NULL, \e vte_cursor_color_default i set to TRUE.
 * Other parameters must not be NULL.
 */
static void termi_set_vte_colors(const GdkColor *fg, const GdkColor *bg, const GdkColor *cursor);
/** @brief Resize the window.
 *
 * This should be called after changing the font.
 * If \e row or \e col is -1, current value is used.
 */
static void termi_resize(gint col, gint row);

/** @brief Add a new tab.
 *
 * If \e cmd is NULL, run a shell.
 * If \e cwd is NULL, use current tab's directory or the current directory.
 * The new tab will receive focus.
 *
 * @return the created tab or \e NULL.
 */
static TermiTab *termi_tab_new(gchar *cmd, const gchar *cwd);
/** @brief Remove a tab.
 *
 * If removed tab is the current tab, focus will be transferred.
 */
static void termi_tab_del(TermiTab *tab);
/// Transfer focus to a given tab.
static void termi_tab_focus(TermiTab *tab);
/// Transfer focus to another tab by relative index.
static void termi_tab_focus_rel(int n);
/// Check if tab as running processes.
static gboolean termi_tab_has_running_processes(TermiTab *tab);
/// Set tab title
static void termi_tab_set_title(TermiTab *tab, const gchar *title);

/** @brief Get URI under the cursor, if any.
 * @return an allocated string, or NULL.
 */
static gchar *termi_get_cursor_uri(const TermiTab *tab, const GdkEventButton *ev);
/// Open a given URI.
static void termi_open_uri(gchar *uri);

#if VTE_CHECK_VERSION(0,26,0)
/** @brief Show dialog to change search regex.
 * @return TRUE if modified, FALSE otherwise.
 */
static gboolean termi_search_modify(void);
/// Find next/previous string.
static void termi_search_find(const TermiTab *tab, int way);
#endif


/** @name Signal callbacks.
 */
//@{
static void termi_winmain_destroy_cb(GtkWindow *, void *);
static gboolean termi_winmain_delete_event_cb(GtkWindow *, GdkEvent *, void *);
static gboolean termi_winmain_key_press_event_cb(GtkWindow *, GdkEventKey *, void *);
static gboolean termi_winmain_focus_in_event_cb(GtkWindow *, GdkEvent *, void *);
static void termi_notebook_switch_page_cb(GtkNotebook *, gpointer, gint index, void *);
static void termi_tab_child_exited_cb(VteTerminal *, void *);
static void termi_tab_eof_cb(VteTerminal *, void *);
static void termi_tab_beep_cb(VteTerminal *, void *);
static void termi_tab_window_title_changed_cb(VteTerminal *, void *);
static void termi_tab_decrease_font_size_cb(VteTerminal *, void *);
static void termi_tab_increase_font_size_cb(VteTerminal *, void *);
static gboolean termi_tab_button_press_event_cb(VteTerminal *, GdkEventButton *, void *);
static gboolean termi_tablbl_button_press_event_cb(GtkWidget *, GdkEventButton *, TermiTab *);
static void termi_dlgcolor_cursor_toggled_cb(GtkToggleButton *, GtkWidget *);
static void termi_dlgtitle_entry_changed_cb(GtkEntry *, GtkDialog *);
#if VTE_CHECK_VERSION(0,26,0)
static void termi_dlgfind_entry_changed_cb(GtkEntry *, GtkDialog *);
#endif
//@}

/** @name Keybinding callbacks.
 */
//@{
#define TERMI_DEFINE_KB_CB(n,k,dm,dk)   static void termi_kb_##n##_cb(void);
  TERMI_KEY_BINDINGS_APPLY(TERMI_DEFINE_KB_CB)
#undef TERMI_DEFINE_KB_CB
//@}

/** @name Popup menu callbacks.
 */
//@{
static void termi_menu_open_uri_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_copy_uri_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_copy_selection_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_paste_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_set_tab_title_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_new_tab_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_close_tab_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_select_font_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_select_colors_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_conf_reload_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_conf_save_cb(TermiTab *, GtkMenuItem *);
static void termi_menu_save_conf_at_exit_cb(TermiTab *, GtkCheckMenuItem *);
//@}


/// Retrieve TermiTab from a VteTerminal widget.
static TermiTab *termi_tab_from_vte(VteTerminal *vte);
/// Retrieve TermiTab from an index page.
static TermiTab *termi_tab_from_index(gint index);
/// Retrieve page index from a TermiTab.
static gint termi_tab_get_index(TermiTab *tab);


/// Display a user error/warning message
#define termi_error(msg, ...)  g_printerr(PROGRAM_NAME": " msg "\n", ##__VA_ARGS__)



void termi_winmain_init(void)
{
  // create the main window
  termi.winmain = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
  gtk_window_set_title(termi.winmain, PROGRAM_NAME);
  gtk_widget_set_name(GTK_WIDGET(termi.winmain), PROGRAM_NAME);
  // set icon
  GtkIconTheme *icon_theme = gtk_icon_theme_get_default();
  gint *icon_sizes = gtk_icon_theme_get_icon_sizes(icon_theme, TERMI_ICON_NAME);
  GList *icons = NULL;
  gint *icon_size_it;
  for( icon_size_it=icon_sizes; *icon_size_it!=0; icon_size_it++ ) {
    GdkPixbuf *icon = gtk_icon_theme_load_icon(icon_theme, TERMI_ICON_NAME, *icon_size_it, 0, NULL);
    if( icon != NULL ) {
      icons = g_list_append(icons, icon);
    }
  }
  g_free(icon_sizes);
  gtk_window_set_icon_list(termi.winmain, icons);
#if GLIB_CHECK_VERSION(2,28,0)
  g_list_free_full(icons, g_object_unref);
#else
  GList *icons_it;
  for( icons_it=icons; icons_it!=NULL; icons_it=icons_it->next ) {
    g_object_unref(icons_it->data);
  }
  g_list_free(icons);
#endif

  // create the notebook
  termi.notebook = GTK_NOTEBOOK(gtk_notebook_new());
  gtk_notebook_set_scrollable(termi.notebook, TRUE);
  gtk_notebook_set_show_border(termi.notebook, FALSE);

  gtk_container_add(GTK_CONTAINER(termi.winmain), GTK_WIDGET(termi.notebook));

  // setup signals
  g_signal_connect(G_OBJECT(termi.winmain), "destroy", G_CALLBACK(termi_winmain_destroy_cb), NULL);
  g_signal_connect(G_OBJECT(termi.winmain), "delete-event", G_CALLBACK(termi_winmain_delete_event_cb), NULL);
  g_signal_connect(G_OBJECT(termi.winmain), "key-press-event", G_CALLBACK(termi_winmain_key_press_event_cb), NULL);
  g_signal_connect(G_OBJECT(termi.winmain), "focus-in-event", G_CALLBACK(termi_winmain_focus_in_event_cb), NULL);
  g_signal_connect(G_OBJECT(termi.notebook), "switch-page", G_CALLBACK(termi_notebook_switch_page_cb), NULL);
}

void termi_quit(void)
{
  if( termi.quitting ) {
    return; // already quitting
  }
  termi.quitting = TRUE;

  if( termi.save_conf_at_exit ) {
    termi_conf_save();
  }

  gint npages = gtk_notebook_get_n_pages(termi.notebook);
  gint i;
  for( i=0; i<npages; i++ ) {
    g_free( termi_tab_from_index(i) );
  }

  gtk_widget_destroy(GTK_WIDGET(termi.winmain));
  g_free(termi.word_chars);
  g_regex_unref(termi.uri_regex);
#if VTE_CHECK_VERSION(0,26,0)
  if( termi.search_regex ) {
    g_regex_unref(termi.search_regex);
  }
#endif
  if( termi.vte_font != NULL ) {
    pango_font_description_free(termi.vte_font);
  }
  g_key_file_free(termi.cfg);
  g_free(termi.cfg_file);

  gtk_main_quit();
}


void termi_conf_load(void)
{
  if( termi.cfg_file == NULL ) {
    termi.cfg_file = g_build_filename(g_get_user_config_dir(), PROGRAM_NAME, PROGRAM_NAME".ini", NULL);
  }
  if( termi.cfg != NULL ) {
    g_key_file_free(termi.cfg);
  }
  termi.cfg = g_key_file_new();

  if( !g_key_file_load_from_file(termi.cfg, termi.cfg_file, G_KEY_FILE_KEEP_COMMENTS, NULL) ) {
    // silently ignore errors (file does not exist, etc.)
  }

  // load cfg entries
  gchar *val_s;

  // General
  termi.save_conf_at_exit = termi_conf_load_bool(TERMI_CFGGRP_GENERAL, "SaveConfAtExit", TRUE);
  termi.show_single_tab = termi_conf_load_bool(TERMI_CFGGRP_GENERAL, "ShowSingleTab", FALSE);
  termi.force_tab_title = termi_conf_load_bool(TERMI_CFGGRP_GENERAL, "ForceTabTitle", FALSE);
  termi.audible_bell = termi_conf_load_bool(TERMI_CFGGRP_GENERAL, "AudibleBell", FALSE);
  termi.visible_bell = termi_conf_load_bool(TERMI_CFGGRP_GENERAL, "VisibleBell", FALSE);
  termi.blink_mode = termi_conf_load_bool(TERMI_CFGGRP_GENERAL, "BlinkMode", FALSE);

  termi.buffer_lines = g_key_file_get_integer(termi.cfg, TERMI_CFGGRP_GENERAL, "BufferLines", NULL);
  if( termi.buffer_lines <= 0 ) {
    termi.buffer_lines = 100; // default (errors silently ignored)
  }

  g_free(termi.word_chars);
  termi.word_chars = g_key_file_get_string(termi.cfg, TERMI_CFGGRP_GENERAL, "WordChars", NULL);
  if( termi.word_chars == NULL ) {
    termi.word_chars = g_strdup("-a-zA-Z0-9_./@~"); // default
  }

#if VTE_CHECK_VERSION(0,26,0)
  termi.search_wrap = termi_conf_load_bool(TERMI_CFGGRP_GENERAL, "SearchWrap", TRUE);
#endif

  // Font
  val_s = g_key_file_get_string(termi.cfg, TERMI_CFGGRP_GENERAL, "Font", NULL);
  PangoFontDescription *vte_font = NULL; // default
  if( val_s != NULL && *val_s != '\0' ) {
    vte_font = pango_font_description_from_string(val_s);
    if( vte_font == NULL ) {
      termi_error("invalid value for Font: %s", val_s);
    }
  }
  g_free(val_s);
  // Colors
  GdkColor col_fg = { .red = 0xc000, .blue = 0xc000, .green = 0xc000 };
  termi_conf_load_color(TERMI_CFGGRP_GENERAL, "ForegroundColor", &col_fg);
  GdkColor col_bg = { .red = 0x0000, .blue = 0x0000, .green = 0x0000 };
  termi_conf_load_color(TERMI_CFGGRP_GENERAL, "BackgroundColor", &col_bg);
  GdkColor col_cursor;
  gboolean col_cursor_default = !termi_conf_load_color(TERMI_CFGGRP_GENERAL, "CursorColor", &col_cursor);

  // Keys
#define TERMI_LOAD_CONF_KB(n,k,dm,dk) \
  termi.kb_##n.mod = dm; \
  termi.kb_##n.key = dk; \
  termi_conf_load_keys(k, &termi.kb_##n);
  TERMI_KEY_BINDINGS_APPLY(TERMI_LOAD_CONF_KB);
#undef TERMI_LOAD_CONF_KB


  // Reapply configuration
  gint npages = gtk_notebook_get_n_pages(termi.notebook);
  gint i;
  for( i=0; i<npages; i++ ) {
    VteTerminal *vte = VTE_TERMINAL(gtk_notebook_get_nth_page(termi.notebook, i));
    vte_terminal_set_audible_bell(vte, termi.audible_bell);
    vte_terminal_set_visible_bell(vte, termi.visible_bell);
    vte_terminal_set_cursor_blink_mode(vte, termi.blink_mode ? VTE_CURSOR_BLINK_ON : VTE_CURSOR_BLINK_OFF);
    vte_terminal_set_scrollback_lines(vte, termi.buffer_lines);
    vte_terminal_set_word_chars(vte, termi.word_chars);
#if VTE_CHECK_VERSION(0,26,0)
    vte_terminal_search_set_wrap_around(vte, termi.search_wrap);
#endif
  }
  if( npages == 1 ) {
    gtk_notebook_set_show_tabs(termi.notebook, termi.show_single_tab);
  }
  termi_set_vte_font(vte_font);
  termi_set_vte_colors(&col_fg, &col_bg, col_cursor_default ? NULL : &col_cursor);
}

void termi_conf_save(void)
{
  g_assert( termi.cfg != NULL );

  // update cfg entries
  g_key_file_set_boolean(termi.cfg, TERMI_CFGGRP_GENERAL, "SaveConfAtExit", termi.save_conf_at_exit);
  g_key_file_set_boolean(termi.cfg, TERMI_CFGGRP_GENERAL, "ShowSingleTab", termi.show_single_tab);
  g_key_file_set_boolean(termi.cfg, TERMI_CFGGRP_GENERAL, "ForceTabTitle", termi.force_tab_title);
  g_key_file_set_boolean(termi.cfg, TERMI_CFGGRP_GENERAL, "AudibleBell", termi.audible_bell);
  g_key_file_set_boolean(termi.cfg, TERMI_CFGGRP_GENERAL, "VisibleBell", termi.visible_bell);
  g_key_file_set_boolean(termi.cfg, TERMI_CFGGRP_GENERAL, "BlinkMode", termi.blink_mode);
  g_key_file_set_integer(termi.cfg, TERMI_CFGGRP_GENERAL, "BufferLines", termi.buffer_lines);
  g_key_file_set_string(termi.cfg, TERMI_CFGGRP_GENERAL, "WordChars", termi.word_chars);
#if VTE_CHECK_VERSION(0,26,0)
  g_key_file_set_boolean(termi.cfg, TERMI_CFGGRP_GENERAL, "SearchWrap", termi.search_wrap);
#endif

  if( termi.vte_font != NULL ) {
    gchar *s = pango_font_description_to_string(termi.vte_font);
    g_key_file_set_string(termi.cfg, TERMI_CFGGRP_GENERAL, "Font", s);
    g_free(s);
  } else {
    g_key_file_set_string(termi.cfg, TERMI_CFGGRP_GENERAL, "Font", "");
  }

  termi_conf_save_color(TERMI_CFGGRP_GENERAL, "ForegroundColor", &termi.vte_fg_color);
  termi_conf_save_color(TERMI_CFGGRP_GENERAL, "BackgroundColor", &termi.vte_bg_color);
  termi_conf_save_color(TERMI_CFGGRP_GENERAL, "CursorColor",
                        termi.vte_cursor_color_default ? NULL : &termi.vte_cursor_color);

  // key bindings are updated at loading

  // save to file

  gchar *cfg_dir = g_path_get_dirname(termi.cfg_file);
  if( g_mkdir_with_parents(cfg_dir, 0700) != 0 ) {
    termi_error("failed to create configuration file directory: %s", g_strerror(errno));
    g_free(cfg_dir);
    return;
  }
  g_free(cfg_dir);

  gsize cfg_len;
  gchar *cfg_data = g_key_file_to_data(termi.cfg, &cfg_len, NULL);
  GError *gerror = NULL;
  if( !g_file_set_contents(termi.cfg_file, cfg_data, cfg_len, &gerror) ) {
    termi_error("failed to save configuration: %s", gerror->message); 
    g_error_free(gerror);
    g_free(cfg_data);
    return;
  }
  g_free(cfg_data);
}

void termi_conf_load_keys(const char *name, TermiKeyBinding *kb)
{
  gchar *s = g_key_file_get_string(termi.cfg, TERMI_CFGGRP_KEYS, name, NULL);
  if( s == NULL ) {
    // default value (only when not set at all)
    // update the configuration now
    s = gtk_accelerator_name(kb->key, kb->mod);
    g_key_file_set_string(termi.cfg, TERMI_CFGGRP_KEYS, name, s);
  } else if( *s == '\0' ) {
    // empty value: binding disabled
    kb->mod = 0;
    kb->key = 0;
  } else {
    gtk_accelerator_parse(s, &kb->key, &kb->mod);
    if( kb->key == 0 && kb->mod == 0 ) {
      termi_error("invalid key binding for %s: %s", name, s);
    }
  }
  g_free(s);
}

gboolean termi_conf_load_bool(const char *grp, const char *name, gboolean def)
{
  GError *gerror = NULL;
  gboolean v = g_key_file_get_boolean(termi.cfg, grp, name, &gerror);
  if( gerror ) {
    v = def;
    g_error_free(gerror);
  }
  return v;
}

gboolean termi_conf_load_color(const char *grp, const char *name, GdkColor *color)
{
  gchar *s = g_key_file_get_string(termi.cfg, grp, name, NULL);
  gboolean ret = FALSE;
  if( s != NULL && *s != '\0' ) {
    if( gdk_color_parse(s, color) ) {
      ret = TRUE;
    } else {
      termi_error("invalid color string for %s: %s", name, s);
    }
  }
  g_free(s);
  return ret;
}

void termi_conf_save_color(const char *grp, const char *name, const GdkColor *color)
{
  if( color == NULL ) {
    g_key_file_set_string(termi.cfg, grp, name, "");
  } else {
    gchar *s = g_strdup_printf("#%02x%02x%02x", color->red>>8, color->green>>8, color->blue>>8);
    g_key_file_set_string(termi.cfg, grp, name, s);
    g_free(s);
  }
}

void termi_menu_popup(TermiTab *tab, const GdkEvent *ev, gboolean full)
{
  //TODO allow to set window title (not just tab)
  GtkMenu *popup_menu = GTK_MENU(gtk_menu_new());
  GtkMenuShell *menu_shell;

  // add a menu entry to menu_shell
#define TERMI_APPEND_MENU_ITEM(n,lbl) do { \
  GtkWidget *item_ = gtk_menu_item_new_with_mnemonic(lbl); \
  gtk_menu_shell_append(menu_shell, item_); \
  g_signal_connect_swapped(G_OBJECT(item_), "activate", G_CALLBACK(termi_menu_##n##_cb), tab); \
} while(0)
  // add a menu entry with a stock image to menu_shell
#define TERMI_APPEND_IMAGE_MENU_ITEM(n,lbl,img) do { \
  GtkWidget *item_ = gtk_image_menu_item_new_with_mnemonic(lbl); \
  gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item_), gtk_image_new_from_stock(img, GTK_ICON_SIZE_MENU)); \
  gtk_menu_shell_append(menu_shell, item_); \
  g_signal_connect_swapped(G_OBJECT(item_), "activate", G_CALLBACK(termi_menu_##n##_cb), tab); \
} while(0)
  // add a menu entry for a boolean config option to menu_shell
#define TERMI_APPEND_BOOL_CONF_MENU(n,lbl) do { \
  GtkWidget *item_ = gtk_check_menu_item_new_with_label(lbl); \
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item_), termi.n); \
  gtk_menu_shell_append(menu_shell, item_); \
  g_signal_connect_swapped(G_OBJECT(item_), "activate", G_CALLBACK(termi_menu_##n##_cb), tab); \
} while(0)
  // add a submenu entry to menu_shell
#define TERMI_APPEND_SUBMENU(menu, lbl) do { \
  GtkWidget *item_ = gtk_menu_item_new_with_mnemonic(lbl); \
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_), menu); \
  gtk_menu_shell_append(menu_shell, item_); \
} while(0)
  // add a separator to menu_shell
#define TERMI_APPEND_SEPARATOR() \
  gtk_menu_shell_append(menu_shell, gtk_separator_menu_item_new())

  // config submenu
  GtkWidget *menu_conf = gtk_menu_new();
  menu_shell = GTK_MENU_SHELL(menu_conf);
  TERMI_APPEND_IMAGE_MENU_ITEM(conf_reload, "Reload", GTK_STOCK_REFRESH);
  TERMI_APPEND_IMAGE_MENU_ITEM(conf_save, "Save now", GTK_STOCK_SAVE);
  TERMI_APPEND_BOOL_CONF_MENU(save_conf_at_exit, "Save at exit");

  // main menu
  menu_shell = GTK_MENU_SHELL(popup_menu);
  if( full ) {
    if( termi.menu_uri != NULL ) {
      g_free(termi.menu_uri);
    }
    if( ev->type == GDK_BUTTON_PRESS ) {
      termi.menu_uri = termi_get_cursor_uri(tab, &ev->button);
    }
    if( termi.menu_uri != NULL ) {
      TERMI_APPEND_IMAGE_MENU_ITEM(open_uri, "_Open URI...", GTK_STOCK_JUMP_TO);
      TERMI_APPEND_IMAGE_MENU_ITEM(copy_uri, "_Copy URI", GTK_STOCK_COPY);
    } else {
      TERMI_APPEND_IMAGE_MENU_ITEM(copy_selection, "_Copy", GTK_STOCK_COPY);
    }
    TERMI_APPEND_IMAGE_MENU_ITEM(paste, "_Paste", GTK_STOCK_PASTE);
    TERMI_APPEND_SEPARATOR();
  }
  TERMI_APPEND_IMAGE_MENU_ITEM(set_tab_title, "Tab _title", GTK_STOCK_EDIT);
  TERMI_APPEND_IMAGE_MENU_ITEM(new_tab, "_New tab", GTK_STOCK_NEW);
  TERMI_APPEND_IMAGE_MENU_ITEM(close_tab, "Close tab", GTK_STOCK_CLOSE);
  TERMI_APPEND_SEPARATOR();
  TERMI_APPEND_IMAGE_MENU_ITEM(select_font, "Select _font", GTK_STOCK_SELECT_FONT);
  TERMI_APPEND_IMAGE_MENU_ITEM(select_colors, "Select co_lors", GTK_STOCK_SELECT_COLOR);
  TERMI_APPEND_SUBMENU(menu_conf, "Confi_guration");

#undef TERMI_APPEND_MENU_ITEM
#undef TERMI_APPEND_IMAGE_MENU_ITEM
#undef TERMI_APPEND_BOOL_CONF_MENU
#undef TERMI_APPEND_SUBMENU
#undef TERMI_APPEND_SEPARATOR

  gtk_widget_show_all(GTK_WIDGET(popup_menu));
  gtk_menu_popup(popup_menu, NULL, NULL, NULL, NULL, 0, gdk_event_get_time(ev));
  g_object_ref_sink(G_OBJECT(popup_menu));
  g_object_unref(G_OBJECT(popup_menu));
}

void termi_set_vte_font(PangoFontDescription *font)
{
  if( font != termi.vte_font ) {
    if( termi.vte_font != NULL ) {
      pango_font_description_free(termi.vte_font);
    }
    termi.vte_font = font;
  }
  gint npages = gtk_notebook_get_n_pages(termi.notebook);
  // get col,row before window is resized
  gint col = -1;
  gint row = -1;
  if( npages > 0 ) {
    TermiTab *tab = termi_tab_from_index(0);
    col = tab->vte->column_count;
    row = tab->vte->row_count;
  }
  gint i;
  for( i=0; i<npages; i++ ) {
    TermiTab *tab = termi_tab_from_index(i);
    vte_terminal_set_font(tab->vte, font);
  }
  if( npages > 0 ) {
    termi_resize(col, row);
  }
}

void termi_set_vte_colors(const GdkColor *fg, const GdkColor *bg, const GdkColor *cursor)
{
  memcpy(&termi.vte_fg_color, fg, sizeof(*fg));
  memcpy(&termi.vte_bg_color, bg, sizeof(*bg));
  if( cursor == NULL ) {
    termi.vte_cursor_color_default = TRUE;
  } else {
    termi.vte_cursor_color_default = FALSE;
    memcpy(&termi.vte_cursor_color, cursor, sizeof(*cursor));
  }

  gint npages = gtk_notebook_get_n_pages(termi.notebook);
  gint i;
  for( i=0; i<npages; i++ ) {
    TermiTab *tab = termi_tab_from_index(i);
    vte_terminal_set_color_foreground(tab->vte, fg);
    vte_terminal_set_color_background(tab->vte, bg);
    vte_terminal_set_color_cursor(tab->vte, cursor);
  }
}


TermiTab *termi_tab_new(gchar *cmd, const gchar *cwd)
{
  TermiTab *tab = g_new0(TermiTab, 1);
  tab->vte = VTE_TERMINAL(vte_terminal_new());
  g_object_set_qdata(G_OBJECT(tab->vte), termi.quark, tab);

  // add to the notebook
  gchar *lbl_txt = g_strdup_printf("Term %u", termi.label_nb++);
  GtkWidget *evbox = gtk_event_box_new();
  tab->lbl = GTK_LABEL(gtk_label_new(lbl_txt));
  g_free(lbl_txt);
  gtk_container_add(GTK_CONTAINER(evbox), GTK_WIDGET(tab->lbl));
  gint index = gtk_notebook_append_page(termi.notebook, GTK_WIDGET(tab->vte), evbox);
  if( index == -1 ) {
    termi_error("failed to create a new tab");
    gtk_widget_destroy(GTK_WIDGET(tab->vte));
    g_free(tab);
    return NULL;
  }

  // split shell command, if any
  GError *gerror = NULL;
  char **argv = NULL;
  if( cmd != NULL ) {
    gint argc;
    if( !g_shell_parse_argv(cmd, &argc, &argv, &gerror) ) {
      termi_error("cannot parse command: %s", gerror->message);
      cmd = NULL;
      g_error_free(gerror);
    }
  }

  // get workding directory of the current tab, if any
  gchar *wdir = cwd ? g_strdup(cwd) : NULL;
  if(wdir == NULL) {
    gint cur_index = gtk_notebook_get_current_page(termi.notebook);
    if( cur_index != -1 ) {
      TermiTab *cur_tab = termi_tab_from_index(cur_index);
      g_assert( cur_tab->pid >= 0 );
      gchar *p = g_strdup_printf("/proc/%d/cwd", cur_tab->pid);
      if( p != NULL ) {
        wdir = g_file_read_link(p, NULL); // ignore errors
        g_free(p);
      }
    }
  }

  // run the command
#if VTE_CHECK_VERSION(0,26,0)
  char *argv2[2] = { NULL, NULL };
  char *cmd_alloc = NULL;
  if( cmd == NULL ) {
#if VTE_CHECK_VERSION(0,28,0)
    cmd_alloc = vte_get_user_shell();
    cmd = cmd_alloc;
#else
    cmd = g_getenv("SHELL");
#endif
    if( cmd == NULL ) {
      cmd = "/bin/sh";
    }
    argv2[0] = cmd;
    argv = argv2;
  }
  gboolean gret = vte_terminal_fork_command_full(
      tab->vte,
      VTE_PTY_NO_LASTLOG|VTE_PTY_NO_UTMP|VTE_PTY_NO_WTMP|VTE_PTY_NO_HELPER,
      wdir, argv, NULL,
      G_SPAWN_CHILD_INHERITS_STDIN|G_SPAWN_SEARCH_PATH, NULL, NULL, &tab->pid, &gerror);
  g_free(cmd_alloc);
#else
  tab->pid = vte_terminal_fork_command(tab->vte, argv==NULL?NULL:argv[0], argv, NULL,
                                       wdir, FALSE, FALSE, FALSE);
  gboolean gret = tab->pid >= 0;
#endif
  g_free(wdir);

  if( !gret ) {
#if VTE_CHECK_VERSION(0,26,0)
    termi_error("cannot run tab command: %s", gerror->message);
    g_error_free(gerror);
#else
    termi_error("cannot run tab command");
#endif
    gtk_notebook_remove_page(termi.notebook, index);
    gtk_widget_destroy(GTK_WIDGET(tab->vte));
    g_free(tab);
    return NULL;
  }

  gtk_notebook_set_tab_reorderable(termi.notebook, GTK_WIDGET(tab->vte), TRUE);
  vte_terminal_set_mouse_autohide(tab->vte, TRUE);
  tab->uri_regex_tag = vte_terminal_match_add_gregex(tab->vte, termi.uri_regex, 0);

  // setup signals
  g_signal_connect(G_OBJECT(tab->vte), "child-exited", G_CALLBACK(termi_tab_child_exited_cb), NULL);
  g_signal_connect(G_OBJECT(tab->vte), "eof", G_CALLBACK(termi_tab_eof_cb), NULL);
  g_signal_connect(G_OBJECT(tab->vte), "beep", G_CALLBACK(termi_tab_beep_cb), NULL);
  g_signal_connect(G_OBJECT(tab->vte), "window-title-changed", G_CALLBACK(termi_tab_window_title_changed_cb), NULL);
  g_signal_connect(G_OBJECT(tab->vte), "decrease-font-size", G_CALLBACK(termi_tab_decrease_font_size_cb), NULL);
  g_signal_connect(G_OBJECT(tab->vte), "increase-font-size", G_CALLBACK(termi_tab_increase_font_size_cb), NULL);
  g_signal_connect(G_OBJECT(tab->vte), "button-press-event", G_CALLBACK(termi_tab_button_press_event_cb), NULL);
  g_signal_connect(G_OBJECT(evbox), "button-press-event", G_CALLBACK(termi_tablbl_button_press_event_cb), tab);

  // various configurable options
  if( !termi.show_single_tab && gtk_notebook_get_n_pages(termi.notebook) == 1 ) {
    gtk_notebook_set_show_tabs(termi.notebook, FALSE);
  } else {
    gtk_notebook_set_show_tabs(termi.notebook, TRUE);
  }
  vte_terminal_set_audible_bell(tab->vte, termi.audible_bell);
  vte_terminal_set_visible_bell(tab->vte, termi.visible_bell);
  vte_terminal_set_cursor_blink_mode(tab->vte, termi.blink_mode ? VTE_CURSOR_BLINK_ON : VTE_CURSOR_BLINK_OFF);
  vte_terminal_set_scrollback_lines(tab->vte, termi.buffer_lines);
  vte_terminal_set_word_chars(tab->vte, termi.word_chars);
#if VTE_CHECK_VERSION(0,26,0)
  vte_terminal_search_set_wrap_around(tab->vte, termi.search_wrap);
#endif
  vte_terminal_set_font(tab->vte, termi.vte_font);
  vte_terminal_set_color_foreground(tab->vte, &termi.vte_fg_color);
  vte_terminal_set_color_background(tab->vte, &termi.vte_bg_color);
  if( !termi.vte_cursor_color_default ) {
    vte_terminal_set_color_cursor(tab->vte, &termi.vte_cursor_color);
  }

  gtk_widget_show_all(evbox);
  gtk_widget_show_all(GTK_WIDGET(tab->vte));

  termi_tab_focus(tab);

  return tab;
}

void termi_tab_del(TermiTab *tab)
{
  gint index = gtk_notebook_page_num(termi.notebook, GTK_WIDGET(tab->vte));
  g_assert( index != -1 );

  // check for running processes
  if( termi_tab_has_running_processes(tab) ) {
    GtkWidget *dlg = gtk_message_dialog_new(
        termi.winmain, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "There are processes still running.\nClose anyway?");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_CLOSE, GTK_RESPONSE_ACCEPT,
                           NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gint response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if( response != GTK_RESPONSE_ACCEPT ) {
      return;
    }
  }

  // removing the page will modify cur_tab/prev_tab,
  // so memorize the next candidate now 
  TermiTab *next_tab = NULL;
  if( termi.cur_tab == tab ) {
    termi.cur_tab = NULL;
    next_tab = termi.prev_tab;
  }
  if( termi.prev_tab == tab ) {
    termi.prev_tab = NULL;
  }

  gtk_notebook_remove_page(termi.notebook, index);
  if( gtk_notebook_get_n_pages(termi.notebook) == 0 ) {
    termi_quit();
    return;
  }

  if( !termi.show_single_tab && gtk_notebook_get_n_pages(termi.notebook) == 1 ) {
    gtk_notebook_set_show_tabs(termi.notebook, FALSE);
  }

  // focus new tab if needed
  if( next_tab != NULL ) {
    termi_tab_focus(next_tab);
  }
}

void termi_tab_focus(TermiTab *tab)
{
  TermiTab *old_tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
  gint index = termi_tab_get_index(tab);
  gtk_notebook_set_current_page(termi.notebook, index);
  gtk_widget_grab_focus(GTK_WIDGET(tab->vte));
  if( old_tab != tab ) {
    termi.prev_tab = old_tab;
  }
}

void termi_tab_focus_rel(int n)
{
  gint new_index = ( gtk_notebook_get_current_page(termi.notebook) + n )
      % gtk_notebook_get_n_pages(termi.notebook);
  termi_tab_focus(termi_tab_from_index(new_index));
}

gboolean termi_tab_has_running_processes(TermiTab *tab)
{
  if( tab->pid < 0 ) {
    return FALSE;
  }
#if VTE_CHECK_VERSION(0,26,0)
  int pty_fd = vte_pty_get_fd(vte_terminal_get_pty_object(tab->vte));
#else
  int pty_fd = vte_terminal_get_pty(tab->vte);
#endif
  pid_t pgid = tcgetpgrp(pty_fd);
  return ( pgid == -1 || pgid != tab->pid );
}

void termi_tab_set_title(TermiTab *tab, const gchar *title)
{
  //XXX truncate title if too long?
  gtk_label_set_text(tab->lbl, title);
}


gchar *termi_get_cursor_uri(const TermiTab *tab, const GdkEventButton *ev)
{
  glong col = ev->x / vte_terminal_get_char_width(tab->vte);
  glong row = ev->y / vte_terminal_get_char_height(tab->vte);
  int tag = -1;
  gchar *s = vte_terminal_match_check(tab->vte, col, row, &tag);
  if( s == NULL || tag != tab->uri_regex_tag ) {
    g_free(s);
    return NULL;
  }
  return s;
}

void termi_open_uri(gchar *uri)
{
  static const char *browser_exec[] = {
    "xdg-open", "x-www-browser", "www-browser", NULL
  };
  gchar *browser = NULL;
  const char **it;
  for( it=browser_exec; *it != NULL; it++ ) {
    browser = g_find_program_in_path(*it);
    if( browser != NULL ) {
      break;
    }
  }
  if( browser == NULL ) {
    termi_error("cannot find a browser");
  }
  gchar *argv[] = { browser, uri, NULL };
  GError *gerror = NULL;
  if( !g_spawn_async(NULL, argv, NULL, 0, NULL, NULL, NULL, &gerror) ) {
    termi_error("failed to open URI: %s", gerror->message);
  }
  g_free(browser);
}

#if VTE_CHECK_VERSION(0,26,0)

gboolean termi_search_modify(void)
{
  GtkDialog *dlg = GTK_DIALOG(gtk_dialog_new_with_buttons(
      "Find regex", termi.winmain, GTK_DIALOG_MODAL,
      GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
      GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL));
  gtk_dialog_set_default_response(dlg, GTK_RESPONSE_ACCEPT);

  GtkEntry *entry = GTK_ENTRY(gtk_entry_new());
  if( termi.search_regex ) {
    gtk_entry_set_text(entry, g_regex_get_pattern(termi.search_regex));
  }
  gtk_entry_set_activates_default(entry, TRUE);
  GtkWidget *check = gtk_check_button_new_with_label("Wrap around");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), termi.search_wrap);

  gtk_box_pack_start(GTK_BOX(dlg->vbox), GTK_WIDGET(entry), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(dlg->vbox), check, FALSE, FALSE, 5);
  gtk_widget_show_all(dlg->vbox);

  g_signal_connect(G_OBJECT(entry), "changed", G_CALLBACK(termi_dlgfind_entry_changed_cb), dlg);

  gboolean ret = gtk_dialog_run(dlg) == GTK_RESPONSE_ACCEPT;
  if( ret ) {
    if( termi.search_regex ) {
      g_regex_unref(termi.search_regex);
    }
    termi.search_regex = NULL;
    const gchar *txt = gtk_entry_get_text(entry);
    if( *txt != '\0' ) {
      // should not fail: checked in termi_dlgfind_entry_changed_cb()
      termi.search_regex = g_regex_new(txt, 0, 0, NULL);
    }
    termi.search_wrap = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check));

    gint npages = gtk_notebook_get_n_pages(termi.notebook);
    gint i;
    for( i=0; i<npages; i++ ) {
      TermiTab *tab = termi_tab_from_index(i);
      vte_terminal_search_set_gregex(tab->vte, termi.search_regex);
    }
  }
  gtk_widget_destroy(GTK_WIDGET(dlg));
  return ret;
}

void termi_search_find(const TermiTab *tab, int way)
{
  if( way > 0 ) {
    vte_terminal_search_find_next(tab->vte);
  } else if( way < 0 ) {
    vte_terminal_search_find_previous(tab->vte);
  } else {
    g_assert(FALSE);
  }
}

#endif


void termi_winmain_destroy_cb(GtkWindow *winmain, void *data)
{
  termi_quit();
}

gboolean termi_winmain_delete_event_cb(GtkWindow *winmain, GdkEvent *ev, void *data)
{
  // check for running processes
  gint npages = gtk_notebook_get_n_pages(termi.notebook);
  gint i;
  for( i=0; i<npages; i++ ) {
    TermiTab *tab = termi_tab_from_index(i);
    if( termi_tab_has_running_processes(tab) ) {
      GtkWidget *dlg = gtk_message_dialog_new(
          winmain, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
          "There are processes still running.\nQuit anyway?");
      gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                             GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                             GTK_STOCK_QUIT, GTK_RESPONSE_ACCEPT,
                             NULL);
      gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
      gint response = gtk_dialog_run(GTK_DIALOG(dlg));
      gtk_widget_destroy(dlg);
      if( response == GTK_RESPONSE_ACCEPT ) {
        return FALSE;
      } else {
        return TRUE;
      }
    }
  }
  return FALSE; // quit
}

gboolean termi_winmain_key_press_event_cb(GtkWindow *winmain, GdkEventKey *ev, void *data)
{
  if( ev->type != GDK_KEY_PRESS ) {
    return FALSE; // should not happen
  }
  TermiKeyBinding kb = { ev->state & gtk_accelerator_get_default_mod_mask(), ev->keyval };
  if( kb.key >= 'A' && kb.key <= 'Z' ) {
    kb.key |= 0x20;
  }
#define TERMI_CHECK_KB(n,k,dm,dk) \
  if( kb.mod == termi.kb_##n.mod && kb.key == termi.kb_##n.key ) { \
    termi_kb_##n##_cb(); \
  } else
  TERMI_KEY_BINDINGS_APPLY(TERMI_CHECK_KB)
#undef TERMI_CHECK_KB
  if( kb.mod == 0 && kb.key == GDK_Menu ) {  // note: par of a "else if"
    TermiTab *tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
    termi_menu_popup(tab, (GdkEvent *)ev, TRUE);
  } else { // follow a "else"
    return FALSE;
  }
  return TRUE; // handled
}

gboolean termi_winmain_focus_in_event_cb(GtkWindow *winmain, GdkEvent *ev, void *data)
{
  gtk_window_set_urgency_hint(winmain, FALSE);
  return FALSE;
}

void termi_notebook_switch_page_cb(GtkNotebook *notebook, gpointer ptr, gint index, void *data)
{
  TermiTab *tab = termi_tab_from_index(index);
  if( tab == termi.cur_tab ) {
    return;
  }
  termi.prev_tab = termi.cur_tab;
  termi.cur_tab = tab;
}


void termi_menu_open_uri_cb(TermiTab *tab, GtkMenuItem *item)
{
  if( termi.menu_uri != NULL ) { // should always be true
    termi_open_uri(termi.menu_uri);
  }
}

void termi_menu_copy_uri_cb(TermiTab *tab, GtkMenuItem *item)
{
  if( termi.menu_uri != NULL ) { // should always be true
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), termi.menu_uri, -1);
  }
}

void termi_menu_copy_selection_cb(TermiTab *tab, GtkMenuItem *item)
{
  if( vte_terminal_get_has_selection(tab->vte) ) {
    vte_terminal_copy_clipboard(tab->vte);
  }
}

void termi_menu_paste_cb(TermiTab *tab, GtkMenuItem *item)
{
  vte_terminal_paste_clipboard(tab->vte);
}

void termi_menu_set_tab_title_cb(TermiTab *tab, GtkMenuItem *item)
{
  GtkDialog *dlg = GTK_DIALOG(gtk_dialog_new_with_buttons(
      "Set tab title", termi.winmain, GTK_DIALOG_MODAL,
      GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
      GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL));
  gtk_dialog_set_default_response(dlg, GTK_RESPONSE_ACCEPT);

  GtkEntry *entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_text(entry, gtk_label_get_text(tab->lbl));
  gtk_entry_set_activates_default(entry, TRUE);
  GtkWidget *check = gtk_check_button_new_with_label("Allow terminal to change tab title");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), !termi.force_tab_title);

  gtk_box_pack_start(GTK_BOX(dlg->vbox), GTK_WIDGET(entry), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(dlg->vbox), check, FALSE, FALSE, 5);
  gtk_widget_show_all(dlg->vbox);

  g_signal_connect(G_OBJECT(entry), "changed", G_CALLBACK(termi_dlgtitle_entry_changed_cb), dlg);

  if( gtk_dialog_run(dlg) == GTK_RESPONSE_ACCEPT ) {
    termi_tab_set_title(tab, gtk_entry_get_text(entry));
    //TODO make this option local to the tab
    termi.force_tab_title = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check));
  }

  gtk_widget_destroy(GTK_WIDGET(dlg));
}

void termi_menu_new_tab_cb(TermiTab *tab, GtkMenuItem *item)
{
  termi_tab_new(NULL, NULL);
}

void termi_menu_close_tab_cb(TermiTab *tab, GtkMenuItem *item)
{
  termi_tab_del(tab);
}

void termi_menu_select_font_cb(TermiTab *tab, GtkMenuItem *item)
{
  GtkWidget *dlg = gtk_font_selection_dialog_new("Select terminal font");
  if( termi.vte_font != NULL ) {
    gtk_font_selection_dialog_set_font_name(GTK_FONT_SELECTION_DIALOG(dlg),
                                            pango_font_description_to_string(termi.vte_font));
  }
  if( gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK ) {
    gchar *s = gtk_font_selection_dialog_get_font_name(GTK_FONT_SELECTION_DIALOG(dlg));
    if( s != NULL ) {
      termi_set_vte_font( pango_font_description_from_string(s) );
      g_free(s);
    }
  }
  gtk_widget_destroy(dlg);
}

void termi_menu_select_colors_cb(TermiTab *tab, GtkMenuItem *item)
{
  GtkDialog *dlg = GTK_DIALOG(gtk_dialog_new_with_buttons(
      "Select terminal colors", termi.winmain, GTK_DIALOG_MODAL,
      GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
      GTK_STOCK_OK, GTK_RESPONSE_ACCEPT, NULL));
  gtk_dialog_set_default_response(dlg, GTK_RESPONSE_ACCEPT);

  GtkWidget *button_fg = gtk_color_button_new_with_color(&termi.vte_fg_color);
  GtkWidget *button_bg = gtk_color_button_new_with_color(&termi.vte_bg_color);
  GtkWidget *toggle_cursor = gtk_toggle_button_new_with_label("Default");
  GtkWidget *button_cursor = gtk_color_button_new();
  if( termi.vte_cursor_color_default ) {
    gtk_color_button_set_color(GTK_COLOR_BUTTON(button_cursor), &termi.vte_fg_color);
    gtk_widget_set_sensitive(button_cursor, FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle_cursor), TRUE);
  } else {
    gtk_color_button_set_color(GTK_COLOR_BUTTON(button_cursor), &termi.vte_cursor_color);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle_cursor), FALSE);
  }
  g_signal_connect(G_OBJECT(toggle_cursor), "toggled", G_CALLBACK(termi_dlgcolor_cursor_toggled_cb), button_cursor);

  GtkTable *table = GTK_TABLE(gtk_table_new(3, 3, FALSE));
  GtkWidget *lbl;
  lbl = gtk_label_new("Foreground color:"); gtk_misc_set_alignment(GTK_MISC(lbl), 0,0);
  gtk_table_attach(table, lbl, 0,1,0,1, GTK_FILL,0, 5,2);
  lbl = gtk_label_new("Background color:"); gtk_misc_set_alignment(GTK_MISC(lbl), 0,0);
  gtk_table_attach(table, lbl, 0,1,1,2, GTK_FILL,0, 5,2);
  lbl = gtk_label_new("Cursor color:");    gtk_misc_set_alignment(GTK_MISC(lbl), 0,0);
  gtk_table_attach(table, lbl, 0,1,2,3, GTK_FILL,0, 5,2);
  gtk_table_attach(table, button_fg,     1,2,0,1, 0,0, 5,2);
  gtk_table_attach(table, button_bg,     1,2,1,2, 0,0, 5,2);
  gtk_table_attach(table, button_cursor, 1,2,2,3, 0,0, 5,2);
  gtk_table_attach(table, toggle_cursor, 2,3,2,3, 0,0, 5,2);

  gtk_box_pack_start(GTK_BOX(dlg->vbox), GTK_WIDGET(table), FALSE, FALSE, 10);
  gtk_widget_show_all(dlg->vbox);

  if( gtk_dialog_run(dlg) == GTK_RESPONSE_ACCEPT ) {
    GdkColor fg, bg;
    gtk_color_button_get_color(GTK_COLOR_BUTTON(button_fg), &fg);
    gtk_color_button_get_color(GTK_COLOR_BUTTON(button_bg), &bg);
    if( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle_cursor)) ) {
      termi_set_vte_colors(&fg, &bg, NULL);
    } else {
      GdkColor cursor;
      gtk_color_button_get_color(GTK_COLOR_BUTTON(button_cursor), &cursor);
      termi_set_vte_colors(&fg, &bg, &cursor);
    }
  }

  gtk_widget_destroy(GTK_WIDGET(dlg));
}

void termi_menu_conf_reload_cb(TermiTab *tab, GtkMenuItem *item)
{
  termi_conf_load();
}

void termi_menu_conf_save_cb(TermiTab *tab, GtkMenuItem *item)
{
  termi_conf_save();
}

void termi_menu_save_conf_at_exit_cb(TermiTab *tab, GtkCheckMenuItem *item)
{
  termi.save_conf_at_exit = gtk_check_menu_item_get_active(item);
}


void termi_tab_child_exited_cb(VteTerminal *vte, void *data)
{
  TermiTab *tab = termi_tab_from_vte(vte);
  tab->pid = -1; // avoid check for running processes
  termi_tab_del(tab);
}

void termi_tab_eof_cb(VteTerminal *vte, void *data)
{
  TermiTab *tab = termi_tab_from_vte(vte);
  tab->pid = -1; // avoid check for running processes
  termi_tab_del(tab);
}

void termi_resize(gint col, gint row)
{
  TermiTab *tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
  VteTerminal *vte = tab->vte;
  if( col < 0 ) {
    col = vte->column_count;
  }
  if( row < 0 ) {
    row = vte->row_count;
  }

  // Update geometry hints of a tab.
  GtkBorder *border = NULL;
  gtk_widget_style_get(GTK_WIDGET(vte), "inner-border", &border, NULL);
  gint pad_x = 0;
  gint pad_y = 0;
  if( border != NULL ) {
    pad_x = border->left + border->right;
    pad_y = border->top + border->bottom;
    gtk_border_free(border);
  }
  glong char_x = vte_terminal_get_char_width(vte);
  glong char_y = vte_terminal_get_char_height(vte);

  GdkGeometry geom = {
    .min_width = pad_x + char_x,
    .min_height = pad_y + char_y,
    .base_width = pad_x,
    .base_height = pad_y,
    .width_inc = char_x,
    .height_inc = char_y,
  };
  gtk_window_set_geometry_hints(
      termi.winmain, GTK_WIDGET(vte), &geom,
      GDK_HINT_RESIZE_INC|GDK_HINT_MIN_SIZE|GDK_HINT_BASE_SIZE);

  // not the first tab: resize window too
  if( gtk_widget_get_realized(GTK_WIDGET(termi.winmain)) ) {
    GtkRequisition req;
    gtk_widget_size_request(GTK_WIDGET(termi.winmain), &req);
    gint win_width  = req.width;
    gint win_height = req.height;
    gtk_widget_size_request(GTK_WIDGET(termi.notebook), &req);
    win_width  -= req.width;
    win_height -= req.height;
    win_width  += pad_x + col * char_x;
    win_height += pad_y + row * char_y;
    gtk_window_resize(termi.winmain, win_width, win_height);
  }
}

void termi_tab_decrease_font_size_cb(VteTerminal *vte, void *data)
{
  gint size = pango_font_description_get_size(termi.vte_font);
  size -= PANGO_SCALE;
  pango_font_description_set_size(termi.vte_font, size);
  termi_set_vte_font(termi.vte_font);
}

void termi_tab_increase_font_size_cb(VteTerminal *vte, void *data)
{
  gint size = pango_font_description_get_size(termi.vte_font);
  size += PANGO_SCALE;
  pango_font_description_set_size(termi.vte_font, size);
  termi_set_vte_font(termi.vte_font);
}

void termi_tab_window_title_changed_cb(VteTerminal *vte, void *data)
{
  if( !termi.force_tab_title ) {
    TermiTab *tab = termi_tab_from_vte(vte);
    termi_tab_set_title(tab, vte->window_title);
  }
}

void termi_tab_beep_cb(VteTerminal *vte, void *data)
{
  if( !gtk_window_is_active(termi.winmain) ) {
    gtk_window_set_urgency_hint(termi.winmain, TRUE);
  }
}

gboolean termi_tab_button_press_event_cb(VteTerminal *vte, GdkEventButton *ev, void *data)
{
  if( ev->button == 3 ) {  // right click
    TermiTab *tab = termi_tab_from_vte(vte);
    termi_menu_popup(tab, (GdkEvent *)ev, TRUE);
  } else if( ev->button == 2 ) {  // middle click
    TermiTab *tab = termi_tab_from_vte(vte);
    // open URI under the cursor, if any
    gchar *uri = termi_get_cursor_uri(tab, ev);
    if( uri == NULL ) {
      return FALSE;
    }
    termi_open_uri(uri);
    g_free(uri);
  } else {
    return FALSE;
  }
  return TRUE; // handled
}

gboolean termi_tablbl_button_press_event_cb(GtkWidget *lbl, GdkEventButton *ev, TermiTab *tab)
{
  if( ev->button == 3 ) {  // right click
    termi_menu_popup(tab, (GdkEvent *)ev, FALSE);
  } else if( ev->button == 2 ) {  // middle click
    termi_tab_del(tab);
  } else {
    return FALSE;
  }
  return TRUE; // handled
}

void termi_dlgcolor_cursor_toggled_cb(GtkToggleButton *toggle, GtkWidget *button)
{
  gtk_widget_set_sensitive(button, !gtk_toggle_button_get_active(toggle));
}

void termi_dlgtitle_entry_changed_cb(GtkEntry *entry, GtkDialog *dlg)
{
  gtk_dialog_set_response_sensitive(dlg, GTK_RESPONSE_ACCEPT, *gtk_entry_get_text(entry) != '\0');
}

#if VTE_CHECK_VERSION(0,26,0)
void termi_dlgfind_entry_changed_cb(GtkEntry *entry, GtkDialog *dlg)
{
  const gchar *txt = gtk_entry_get_text(entry);
  gboolean sensitive = FALSE;
  if( *txt == '\0' ) {
    sensitive = TRUE;
  } else {
    GError *gerror = NULL;
    GRegex *regex = g_regex_new(txt, 0, 0, &gerror);
    if( gerror ) {
      sensitive = FALSE;
      g_error_free(gerror);
    } else {
      g_regex_unref(regex);
      sensitive = TRUE;
    }
  }
  gtk_dialog_set_response_sensitive(dlg, GTK_RESPONSE_ACCEPT, sensitive);
}
#endif



void termi_kb_new_tab_cb(void)   { termi_tab_new(NULL, NULL); }
void termi_kb_left_tab_cb(void)  { termi_tab_focus_rel(-1); }
void termi_kb_right_tab_cb(void) { termi_tab_focus_rel(+1); }
void termi_kb_prev_tab_cb(void)  { if( termi.prev_tab ) termi_tab_focus(termi.prev_tab); }
void termi_kb_copy_cb(void)
{
  TermiTab *tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
  if( vte_terminal_get_has_selection(tab->vte) ) {
    vte_terminal_copy_clipboard(tab->vte);
  }
}
void termi_kb_paste_cb(void)
{
  TermiTab *tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
  vte_terminal_paste_clipboard(tab->vte);
}

#if VTE_CHECK_VERSION(0,26,0)
void termi_kb_find_cb(void)
{
  if( termi_search_modify() && termi.search_regex ) {
    TermiTab *tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
    termi_search_find(tab, +1);
  }
}
void termi_kb_find_next_cb(void)
{
  if( termi.search_regex || (termi_search_modify() && termi.search_regex) ) {
    TermiTab *tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
    termi_search_find(tab, +1);
  }
}
void termi_kb_find_prev_cb(void)
{
  if( termi.search_regex || (termi_search_modify() && termi.search_regex) ) {
    TermiTab *tab = termi_tab_from_index(gtk_notebook_get_current_page(termi.notebook));
    termi_search_find(tab, -1);
  }
}
#endif


TermiTab *termi_tab_from_vte(VteTerminal *vte)
{
  TermiTab *tab = g_object_get_qdata(G_OBJECT(vte), termi.quark);
  g_assert( tab != NULL );
  return tab;
}

TermiTab *termi_tab_from_index(gint index)
{
  GtkWidget *vte = gtk_notebook_get_nth_page(termi.notebook, index);
  g_assert( vte != NULL );
  TermiTab *tab = g_object_get_qdata(G_OBJECT(vte), termi.quark);
  g_assert( tab != NULL );
  return tab;
}

gint termi_tab_get_index(TermiTab *tab)
{
  gint index = gtk_notebook_page_num(termi.notebook, GTK_WIDGET(tab->vte));
  g_assert( index != -1 );
  return index;
}


typedef struct {
  gchar *title;
  gchar *cwd;
  gchar *command;
} termi_opt_tab_t;

typedef struct {
  GArray *tabs;
} termi_opt_data_t;

void termi_opt_tab_free(gpointer data)
{
  termi_opt_tab_t *d = data;
  g_free(d->title);
  g_free(d->cwd);
  g_free(d->command);
}

gboolean termi_opt_tab_cb(const gchar *option_name, const gchar *value,
                          gpointer data, GError **error)
{
  const gchar *sep = g_strstr_len(value, -1, "  ");
  const gchar *end = value + strlen(value);
  termi_opt_data_t *d = data;
  termi_opt_tab_t tab = { NULL, NULL, NULL };
  if(sep == NULL) {
    if(end > value) {
      tab.command = g_strdup(value);
    }
  } else {
    if(sep > value) {
      tab.title = g_strndup(value, sep-value);
    }
    const gchar *sep2 = g_strstr_len(sep+2, -1, "  ");
    if(sep2 == NULL) {
      if(end > sep+2) {
        tab.command = g_strdup(sep+2);
      }
    } else {
      if(sep2 > sep+2) {
        tab.cwd = g_strndup(sep+2, sep2-(sep+2));
      }
      if(end > sep2+2) {
        tab.command = g_strdup(sep2+2);
      }
    }
  }
  g_array_append_val(d->tabs, tab);
  return TRUE;
}



int main(int argc, char *argv[])
{
  gboolean opt_version = FALSE;
  gchar *opt_execute = NULL;
  gchar *opt_title = NULL;
  gchar *opt_geometry = NULL;

  const GOptionEntry opt_entries[] = {
    { "execute", 'e', 0, G_OPTION_ARG_STRING, &opt_execute, "Execute given command in first tab", NULL },
    { "title", 't', 0, G_OPTION_ARG_STRING, &opt_title, "Window title", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Display version number", NULL },
    { "geometry", 0, 0, G_OPTION_ARG_STRING, &opt_geometry, "X geometry for the window", NULL },
    { "tab", 0, 0, G_OPTION_ARG_CALLBACK, &termi_opt_tab_cb, "Create a tab; format is \"[tab-title  [cwd  ]][command]\"", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
  };

  termi_opt_data_t opt_data = {
    .tabs = g_array_new(FALSE, FALSE, sizeof(termi_opt_tab_t)),
  };
  g_array_set_clear_func(opt_data.tabs, termi_opt_tab_free);

  GOptionContext *opt_context = g_option_context_new("- mini terminal emulator");
  GOptionGroup *opt_group = g_option_group_new(NULL, NULL, NULL, &opt_data, NULL);
  g_option_context_set_main_group(opt_context, opt_group);
  g_option_context_add_main_entries(opt_context, opt_entries, NULL);
  g_option_context_add_group(opt_context, gtk_get_option_group(TRUE));

  GError *gerror = NULL;
  g_option_context_parse(opt_context, &argc, &argv, &gerror);
  g_option_context_free(opt_context);
  if( gerror ) {
    termi_error("option parsing failed: %s", gerror->message);
    g_error_free(gerror);
    exit(1);
  }

  if( opt_version ) {
    g_print("%s\n", VERSION);
    return 0;
  }

  // global init
  termi.quark = g_quark_from_static_string(TERMI_QUARK_STR);
  termi.uri_regex = g_regex_new("[a-zA-Z0-9+-]+://\\S*[a-zA-Z0-9_/%&=]", G_REGEX_OPTIMIZE, 0, NULL);

  gtk_init(&argc, &argv);
  termi_winmain_init();
  // load configuration (window has to be created first)
  termi_conf_load();

  if( opt_title != NULL) {
    gtk_window_set_title(termi.winmain, opt_title);
    g_free(opt_title);
  }

  // create initial tabs
  gboolean has_opt_tabs = opt_data.tabs->len > 0;
  if(opt_execute || !has_opt_tabs) {
    // "default" tab
    TermiTab *tab = termi_tab_new(opt_execute, NULL);
    if(tab == NULL) {
      termi_error("failed to create default tab");
    }
    g_free(opt_execute);
  }
  if(has_opt_tabs) {
    guint i;
    for(i=0; i<opt_data.tabs->len; i++) {
      termi_opt_tab_t *opt_tab = &g_array_index(opt_data.tabs, termi_opt_tab_t, i);
      TermiTab *tab = termi_tab_new(opt_tab->command, opt_tab->cwd);
      if(tab == NULL) {
        termi_error("failed to create tab '%s'", opt_tab->title);
      }
      if(opt_tab->title != NULL) {
        termi_tab_set_title(tab, opt_tab->title);
      }
    }
  }
  termi_resize(80, 24);

  // free opt_data
  g_array_free(opt_data.tabs, TRUE);

  // set geometry (has to be done before showing the main window)
  if( opt_geometry != NULL ) {
    if( !gtk_window_parse_geometry(termi.winmain, opt_geometry) ) {
      termi_error("invalid geometry string");
    }
    g_free(opt_geometry);
  }

  gtk_widget_show_all(GTK_WIDGET(termi.winmain));

  // select first tab
  TermiTab *tab = termi_tab_from_index(0);
  termi_tab_focus(tab);
  //XXX:hack colors are not properly set for the first tab, it seems the window
  // has to ben realized first.
  // This may be fixed in newer versions of libvte.
  // copy paste from term_tab_new()
  vte_terminal_set_color_foreground(tab->vte, &termi.vte_fg_color);
  vte_terminal_set_color_background(tab->vte, &termi.vte_bg_color);
  if( !termi.vte_cursor_color_default ) {
    vte_terminal_set_color_cursor(tab->vte, &termi.vte_cursor_color);
  }

  gtk_main();

  return 0;
}

