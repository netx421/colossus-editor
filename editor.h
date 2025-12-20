// editor.h — COLOSSUS Editor header (upgraded)

#pragma once

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <gio/gio.h>
#include <string>

class Editor {
public:
    explicit Editor(GtkApplication* app);
    ~Editor();

    // Application signal handlers
    static void on_activate(GtkApplication* app, gpointer user_data);
    static void on_open(GtkApplication* app,
                        GFile** files,
                        gint n_files,
                        const gchar* hint,
                        gpointer user_data);

private:
    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* text_view_ = nullptr;
    GtkWidget* status_bar_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    GtkSourceLanguageManager* lang_manager_ = nullptr;

    std::string current_file_;
    bool modified_ = false;

    // dialogs
    GtkWidget* find_dialog_ = nullptr;
    GtkWidget* replace_dialog_ = nullptr;

    // recent files
    GtkRecentManager* recent_mgr_ = nullptr;

    // search
    GtkSourceSearchSettings* search_settings_ = nullptr;
    GtkSourceSearchContext*  search_context_  = nullptr;

    // file monitor
    GFileMonitor* file_monitor_ = nullptr;
    guint64 file_mtime_utc_us_ = 0;
    bool suppress_monitor_once_ = false;

    // prefs
    bool trim_ws_on_save_ = true;
    bool ensure_newline_eof_ = true;
    int tab_width_ = 4;

    // zoom
    int font_pt_ = 11;

    // session
    bool opened_via_cli_ = false;

    // UI setup
    void setup_ui();
    GtkWidget* create_menu_bar();
    void setup_sourceview_defaults();
    void setup_search();
    void setup_recent();

    // File ops
    void new_file();
    void open_file();  // dialog-based open
    void save_file();
    void save_file_as();
    void open_file_from_path(const std::string& path);

    // extra file ops
    void open_containing_folder();
    void remove_file_monitor();
    void install_file_monitor(const std::string& path);
    static guint64 get_file_mtime_us(const std::string& path);

    // prompts
    bool maybe_confirm_discard(const char* action_label);
    bool confirm_reload_external(const char* reason);

    // Edit ops
    void cut();
    void copy();
    void paste();
    void select_all();

    // Find / Replace / Go To
    void show_find_dialog();
    void show_replace_dialog();
    void show_goto_line_dialog();
    void goto_line(int line);

    // search helpers
    void ensure_search_context();
    void update_search_from_dialog(GtkWidget* dialog);
    void search_find_next(bool backwards);
    void search_replace_one(const std::string& repl);
    void search_replace_all(const std::string& repl);

    // Syntax highlighting
    void update_language_for_filename(const std::string& filename);

    // status + title
    void update_title();
    void update_status_full();
    void update_cursor_status();
    void mark_modified(bool is_modified);

    // save helpers
    void apply_save_fixes(std::string& text);
    static std::string make_backup_path(const std::string& path);

    // recent helper
    void add_recent_item(const std::string& path);

    // config + session
    void load_config();
    void save_config();
    void load_session();
    void save_session();

    // zoom
    void zoom_set(int pt);
    void zoom_step(int delta);

    // Static callbacks
    static void s_on_new_activate(GtkWidget*, gpointer);
    static void s_on_open_activate(GtkWidget*, gpointer);
    static void s_on_save_activate(GtkWidget*, gpointer);
    static void s_on_save_as_activate(GtkWidget*, gpointer);
    static void s_on_reload_activate(GtkWidget*, gpointer);
    static void s_on_open_folder_activate(GtkWidget*, gpointer);
    static void s_on_quit_activate(GtkWidget*, gpointer);

    static void s_on_cut_activate(GtkWidget*, gpointer);
    static void s_on_copy_activate(GtkWidget*, gpointer);
    static void s_on_paste_activate(GtkWidget*, gpointer);
    static void s_on_select_all_activate(GtkWidget*, gpointer);

    static void s_on_find_activate(GtkWidget*, gpointer);
    static void s_on_replace_activate(GtkWidget*, gpointer);
    static void s_on_goto_line_activate(GtkWidget*, gpointer);

    static void s_on_buffer_changed(GtkTextBuffer*, gpointer);
    static void s_on_cursor_notify(GObject*, GParamSpec*, gpointer);

    static gboolean s_on_key_press(GtkWidget*, GdkEventKey*, gpointer);

    static void s_on_find_dialog_response(GtkDialog*, gint, gpointer);
    static void s_on_replace_dialog_response(GtkDialog*, gint, gpointer);
    static void s_on_goto_line_response(GtkDialog*, gint, gpointer);

    static void s_on_recent_activated(GtkRecentChooser*, gpointer);
    static void s_on_file_monitor_changed(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer);

    static void s_on_zoom_in_activate(GtkWidget*, gpointer);
    static void s_on_zoom_out_activate(GtkWidget*, gpointer);
    static void s_on_zoom_reset_activate(GtkWidget*, gpointer);

    static void s_on_toggle_trim_ws(GtkWidget*, gpointer);
    static void s_on_toggle_eof_nl(GtkWidget*, gpointer);
    static void s_on_spaces_toggle(GtkWidget*, gpointer);
    static void s_on_tab_width_2(GtkWidget*, gpointer);
    static void s_on_tab_width_4(GtkWidget*, gpointer);
    static void s_on_tab_width_8(GtkWidget*, gpointer);
};

// C-style entrypoint used from main.cpp
int run_colossus_editor(int argc, char** argv);
