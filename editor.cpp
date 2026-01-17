// editor.cpp — COLOSSUS Editor implementation (GTK3 + GtkSourceView-3 compatible)

#include "editor.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Global instance for GApplication callbacks
static Editor* g_editor_instance = nullptr;

static std::string dirname_of(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

static std::string basename_of(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return p;
    return p.substr(pos + 1);
}

static bool is_space_char(unsigned char c) { return std::isspace(c) != 0; }

static std::string rtrim_copy(std::string s) {
    while (!s.empty() && is_space_char((unsigned char)s.back())) s.pop_back();
    return s;
}

// file mtime in microseconds using GIO
static guint64 file_mtime_us_gio(const std::string& path) {
    GFile* f = g_file_new_for_path(path.c_str());
    if (!f) return 0;

    GError* err = nullptr;
    GFileInfo* info = g_file_query_info(
        f,
        G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NONE,
        nullptr,
        &err
    );

    guint64 out = 0;
    if (info) {
        guint64 sec = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        out = sec * 1000000ULL;
        g_object_unref(info);
    }

    if (err) g_error_free(err);
    g_object_unref(f);
    return out;
}

// config location: ~/.config/colossus-editor/config.ini
static std::string config_dir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else {
        const char* home = std::getenv("HOME");
        base = (home && *home) ? home : ".";
        base += "/.config";
    }
    return base + "/colossus-editor";
}

static std::string config_path() {
    return config_dir() + "/config.ini";
}

static void ensure_dir_exists(const std::string& dir) {
    GError* err = nullptr;
    if (!g_file_test(dir.c_str(), G_FILE_TEST_IS_DIR)) {
        if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) {
            // ignore
        }
    }
    if (err) g_error_free(err);
}

static std::string to_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

// ───────────────────────────────────────────────
//  Public interface
// ───────────────────────────────────────────────

Editor::Editor(GtkApplication* app)
    : app_(app)
{
    g_editor_instance = this;
    load_config();
    load_session();
    setup_ui();
}

Editor::~Editor() {
    remove_file_monitor();

    if (search_context_) g_object_unref(search_context_);
    if (search_settings_) g_object_unref(search_settings_);

    save_session();
    save_config();
}

// ───────────────────────────────────────────────
//  UI setup
// ───────────────────────────────────────────────

void Editor::setup_ui() {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_default_size(GTK_WINDOW(window_), 900, 700);
    gtk_window_set_title(GTK_WINDOW(window_), "Untitled — COLOSSUS Editor");
    gtk_window_set_icon_name(GTK_WINDOW(window_), "accessories-text-editor");

    // Main vertical box
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), vbox);

    // Menu bar
    GtkWidget* menubar = create_menu_bar();
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    // Source buffer + view
    lang_manager_ = gtk_source_language_manager_get_default();
    GtkSourceBuffer* src_buffer = gtk_source_buffer_new(nullptr);
	// Load COLOSSUS monochrome style scheme (project-local ./styles)
	GtkSourceStyleSchemeManager* scheme_mgr = gtk_source_style_scheme_manager_get_default();
	gtk_source_style_scheme_manager_append_search_path(scheme_mgr, "./styles");
	gtk_source_style_scheme_manager_force_rescan(scheme_mgr);

	GtkSourceStyleScheme* scheme =
    		gtk_source_style_scheme_manager_get_scheme(scheme_mgr, "colossus-mono");

	if (scheme) {
    		gtk_source_buffer_set_style_scheme(src_buffer, scheme);
	} else {
    g_printerr("COLOSSUS: could not find style scheme 'colossus-mono' in ./styles\n");
}

    buffer_ = GTK_TEXT_BUFFER(src_buffer);

    text_view_ = gtk_source_view_new_with_buffer(src_buffer);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_), GTK_WRAP_NONE);

    // inherit GTK theme (no forced palette), but we can set tiny CSS for current line if desired
    // NOTE: keep minimal to respect your “match system theme” goal.
    GtkCssProvider* provider = gtk_css_provider_new();
    const gchar* css =
        "GtkSourceView.view .current-line {"
        "  background-color: rgba(0,0,0,0.10);"
        "}\n";
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    GtkStyleContext* ctx = gtk_widget_get_style_context(text_view_);
    gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    setup_sourceview_defaults();
    setup_search();
    setup_recent();

    // Scroll container
    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(scrolled), text_view_);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    // Status bar (label)
    status_bar_ = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(vbox), status_bar_, FALSE, FALSE, 4);

    // signals
    g_signal_connect(buffer_, "changed", G_CALLBACK(Editor::s_on_buffer_changed), this);
    g_signal_connect(buffer_, "notify::cursor-position", G_CALLBACK(Editor::s_on_cursor_notify), this);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(Editor::s_on_key_press), this);

    // Apply initial zoom
    zoom_set(font_pt_);

    // initial status + title
    update_title();
    update_status_full();

    gtk_widget_show_all(window_);
}

void Editor::setup_sourceview_defaults() {
    // Line numbers + current line highlight
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(text_view_), TRUE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(text_view_), TRUE);

    // tab behavior
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(text_view_), tab_width_);
    gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(text_view_), TRUE);

    // bracket matching: GtkSourceView-3 uses buffer API
    gtk_source_buffer_set_highlight_matching_brackets(GTK_SOURCE_BUFFER(buffer_), TRUE);

    // show right margin is optional; leaving off keeps it neutral
}

void Editor::setup_search() {
    search_settings_ = gtk_source_search_settings_new();
    // default: case-insensitive, wrap around on our side
    gtk_source_search_settings_set_case_sensitive(search_settings_, FALSE);
    gtk_source_search_settings_set_wrap_around(search_settings_, FALSE);

    search_context_ = gtk_source_search_context_new(GTK_SOURCE_BUFFER(buffer_), search_settings_);
    gtk_source_search_context_set_highlight(search_context_, TRUE);
}

void Editor::setup_recent() {
    recent_mgr_ = gtk_recent_manager_get_default();
}

GtkWidget* Editor::create_menu_bar() {
    GtkWidget* menubar = gtk_menu_bar_new();
    GtkAccelGroup* accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(window_), accel);

    auto add_item = [&](GtkWidget* menu, const char* label, const char* accel_str, GCallback cb) -> GtkWidget* {
        GtkWidget* item = gtk_menu_item_new_with_mnemonic(label);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        if (accel_str && *accel_str) {
            guint key; GdkModifierType mods;
            gtk_accelerator_parse(accel_str, &key, &mods);
            gtk_widget_add_accelerator(item, "activate", accel, key, mods, GTK_ACCEL_VISIBLE);
        }
        if (cb) g_signal_connect(item, "activate", cb, this);
        return item;
    };

    // ───── File ─────
    GtkWidget* file_menu = gtk_menu_new();
    GtkWidget* file_item = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    add_item(file_menu, "_New", "<Control>N", G_CALLBACK(Editor::s_on_new_activate));
    add_item(file_menu, "_Open…", "<Control>O", G_CALLBACK(Editor::s_on_open_activate));

    // Open Recent submenu
    GtkWidget* recent_item = gtk_menu_item_new_with_mnemonic("Open _Recent");
    GtkWidget* recent_menu = gtk_recent_chooser_menu_new_for_manager(recent_mgr_);
    gtk_recent_chooser_set_show_icons(GTK_RECENT_CHOOSER(recent_menu), TRUE);
    gtk_recent_chooser_set_limit(GTK_RECENT_CHOOSER(recent_menu), 12);
    gtk_recent_chooser_set_sort_type(GTK_RECENT_CHOOSER(recent_menu), GTK_RECENT_SORT_MRU);
    g_signal_connect(recent_menu, "item-activated", G_CALLBACK(Editor::s_on_recent_activated), this);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(recent_item), recent_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), recent_item);

    add_item(file_menu, "_Save", "<Control>S", G_CALLBACK(Editor::s_on_save_activate));
    add_item(file_menu, "Save _As…", "<Shift><Control>S", G_CALLBACK(Editor::s_on_save_as_activate));
    add_item(file_menu, "_Reload from Disk", "F5", G_CALLBACK(Editor::s_on_reload_activate));
    add_item(file_menu, "Open _Containing Folder", "<Control><Shift>O", G_CALLBACK(Editor::s_on_open_folder_activate));

    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());
    add_item(file_menu, "_Quit", "<Control>Q", G_CALLBACK(Editor::s_on_quit_activate));

    // ───── Edit ─────
    GtkWidget* edit_menu = gtk_menu_new();
    GtkWidget* edit_item = gtk_menu_item_new_with_mnemonic("_Edit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_item), edit_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), edit_item);

    add_item(edit_menu, "Cu_t", "<Control>X", G_CALLBACK(Editor::s_on_cut_activate));
    add_item(edit_menu, "_Copy", "<Control>C", G_CALLBACK(Editor::s_on_copy_activate));
    add_item(edit_menu, "_Paste", "<Control>V", G_CALLBACK(Editor::s_on_paste_activate));
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), gtk_separator_menu_item_new());
    add_item(edit_menu, "Select _All", "<Control>A", G_CALLBACK(Editor::s_on_select_all_activate));

    // ───── Search ─────
    GtkWidget* search_menu = gtk_menu_new();
    GtkWidget* search_item = gtk_menu_item_new_with_mnemonic("_Search");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(search_item), search_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), search_item);

    add_item(search_menu, "_Find…", "<Control>F", G_CALLBACK(Editor::s_on_find_activate));
    add_item(search_menu, "_Replace…", "<Control>H", G_CALLBACK(Editor::s_on_replace_activate));
    add_item(search_menu, "_Go to Line…", "<Control>L", G_CALLBACK(Editor::s_on_goto_line_activate));

    // ───── View ─────
    GtkWidget* view_menu = gtk_menu_new();
    GtkWidget* view_item = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_item);

    add_item(view_menu, "Zoom _In", "<Control>plus", G_CALLBACK(Editor::s_on_zoom_in_activate));
    add_item(view_menu, "Zoom _Out", "<Control>minus", G_CALLBACK(Editor::s_on_zoom_out_activate));
    add_item(view_menu, "Zoom _Reset", "<Control>0", G_CALLBACK(Editor::s_on_zoom_reset_activate));

    // ───── Options ─────
    GtkWidget* opt_menu = gtk_menu_new();
    GtkWidget* opt_item = gtk_menu_item_new_with_mnemonic("_Options");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(opt_item), opt_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), opt_item);

    GtkWidget* trim_item = gtk_check_menu_item_new_with_mnemonic("Trim trailing _whitespace on save");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(trim_item), trim_ws_on_save_);
    g_signal_connect(trim_item, "activate", G_CALLBACK(Editor::s_on_toggle_trim_ws), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(opt_menu), trim_item);

    GtkWidget* nl_item = gtk_check_menu_item_new_with_mnemonic("Ensure _newline at EOF");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(nl_item), ensure_newline_eof_);
    g_signal_connect(nl_item, "activate", G_CALLBACK(Editor::s_on_toggle_eof_nl), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(opt_menu), nl_item);

    GtkWidget* spaces_item = gtk_check_menu_item_new_with_mnemonic("Insert _spaces instead of tabs");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(spaces_item), TRUE);
    g_signal_connect(spaces_item, "activate", G_CALLBACK(Editor::s_on_spaces_toggle), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(opt_menu), spaces_item);

    GtkWidget* tab_menu_item = gtk_menu_item_new_with_mnemonic("_Tab Width");
    GtkWidget* tab_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(tab_menu_item), tab_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(opt_menu), tab_menu_item);

    add_item(tab_menu, "_2", nullptr, G_CALLBACK(Editor::s_on_tab_width_2));
    add_item(tab_menu, "_4", nullptr, G_CALLBACK(Editor::s_on_tab_width_4));
    add_item(tab_menu, "_8", nullptr, G_CALLBACK(Editor::s_on_tab_width_8));

    return menubar;
}

// ───────────────────────────────────────────────
//  Prompts
// ───────────────────────────────────────────────

bool Editor::maybe_confirm_discard(const char* action_label) {
    if (!modified_) return true;

    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(window_),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE,
        "You have unsaved changes."
    );
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
                                            "Do you want to discard changes and %s?",
                                            action_label ? action_label : "continue");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Discard", GTK_RESPONSE_ACCEPT,
                           nullptr);

    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp == GTK_RESPONSE_ACCEPT;
}

bool Editor::confirm_reload_external(const char* reason) {
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(window_),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "File changed on disk."
    );
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
                                            "%s\nReload from disk? (Unsaved changes will be lost.)",
                                            reason ? reason : "");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           "_Reload", GTK_RESPONSE_ACCEPT,
                           nullptr);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp == GTK_RESPONSE_ACCEPT;
}

// ───────────────────────────────────────────────
//  File monitor
// ───────────────────────────────────────────────

void Editor::remove_file_monitor() {
    if (file_monitor_) {
        g_object_unref(file_monitor_);
        file_monitor_ = nullptr;
    }
}

void Editor::install_file_monitor(const std::string& path) {
    remove_file_monitor();
    if (path.empty()) return;

    GFile* f = g_file_new_for_path(path.c_str());
    if (!f) return;

    GError* err = nullptr;
    file_monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_NONE, nullptr, &err);
    g_object_unref(f);

    if (err) {
        g_error_free(err);
        if (file_monitor_) { g_object_unref(file_monitor_); file_monitor_ = nullptr; }
        return;
    }

    file_mtime_utc_us_ = get_file_mtime_us(path);
    g_signal_connect(file_monitor_, "changed", G_CALLBACK(Editor::s_on_file_monitor_changed), this);
}

guint64 Editor::get_file_mtime_us(const std::string& path) {
    return file_mtime_us_gio(path);
}

// ───────────────────────────────────────────────
//  File operations
// ───────────────────────────────────────────────

void Editor::new_file() {
    if (!maybe_confirm_discard("create a new file")) return;

    gtk_text_buffer_set_text(buffer_, "", -1);
    current_file_.clear();
    update_language_for_filename(current_file_);
    remove_file_monitor();
    mark_modified(false);
    update_title();
    update_status_full();
}

void Editor::open_file() {
    if (!maybe_confirm_discard("open a file")) return;

    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Open File",
        GTK_WINDOW(window_),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        nullptr);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            open_file_from_path(filename);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}

void Editor::open_file_from_path(const std::string& path) {
    if (!opened_via_cli_) {
        // only prompt discard if this came from menu actions
        // (CLI open usually means you want it opened)
        // still safe if modified:
        if (modified_ && !maybe_confirm_discard("open another file")) return;
    }

    gchar* contents = nullptr;
    gsize length = 0;
    GError* error = nullptr;

    if (g_file_get_contents(path.c_str(), &contents, &length, &error)) {
        suppress_monitor_once_ = true; // avoid seeing our own subsequent writes as "external"
        gtk_text_buffer_set_text(buffer_, contents, (gint)length);
        g_free(contents);

        current_file_ = path;
        update_language_for_filename(current_file_);
        add_recent_item(current_file_);
        install_file_monitor(current_file_);

        mark_modified(false);
        update_title();
        update_status_full();
    } else {
        // If file doesn't exist, treat as new empty file with that name
        if (error && error->code == G_FILE_ERROR_NOENT) {
            gtk_text_buffer_set_text(buffer_, "", -1);
            current_file_ = path;
            update_language_for_filename(current_file_);
            remove_file_monitor();
            mark_modified(false);
            update_title();
            update_status_full();
        } else {
            std::cerr << "Error opening file: " << (error ? error->message : "unknown") << "\n";
        }
        if (error) g_error_free(error);
    }
}

static std::string make_backup_path_impl(const std::string& path) {
    return path + ".bak";
}

std::string Editor::make_backup_path(const std::string& path) {
    return make_backup_path_impl(path);
}

void Editor::apply_save_fixes(std::string& text) {
    if (trim_ws_on_save_) {
        std::stringstream in(text);
        std::string out;
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!first) out.push_back('\n');
            first = false;
            out += rtrim_copy(line);
        }
        // preserve trailing newline that getline strips? handled below by ensure_newline_eof_
        text = out;
    }
    if (ensure_newline_eof_) {
        if (text.empty() || text.back() != '\n') text.push_back('\n');
    }
}

void Editor::save_file() {
    if (current_file_.empty()) {
        save_file_as();
        return;
    }

    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer_, &start);
    gtk_text_buffer_get_end_iter(buffer_, &end);
    gchar* raw = gtk_text_buffer_get_text(buffer_, &start, &end, FALSE);

    std::string text = raw ? raw : "";
    if (raw) g_free(raw);

    apply_save_fixes(text);

    // Backup existing file
    if (g_file_test(current_file_.c_str(), G_FILE_TEST_EXISTS)) {
        std::string bak = make_backup_path(current_file_);
        // best-effort backup
        std::ifstream src(current_file_, std::ios::binary);
        std::ofstream dst(bak, std::ios::binary | std::ios::trunc);
        if (src && dst) dst << src.rdbuf();
    }

    GError* error = nullptr;
    suppress_monitor_once_ = true;
    if (g_file_set_contents(current_file_.c_str(), text.c_str(), (gssize)text.size(), &error)) {
        file_mtime_utc_us_ = get_file_mtime_us(current_file_);
        mark_modified(false);
        update_title();
        update_status_full();
    } else {
        std::cerr << "Error saving file: " << (error ? error->message : "unknown") << "\n";
        if (error) g_error_free(error);
    }
}

void Editor::save_file_as() {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Save File As",
        GTK_WINDOW(window_),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_ACCEPT,
        nullptr);

    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

    if (!current_file_.empty())
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), current_file_.c_str());

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (!filename) {
            gtk_widget_destroy(dialog);
            return;
        }

        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(buffer_, &start);
        gtk_text_buffer_get_end_iter(buffer_, &end);
        gchar* raw = gtk_text_buffer_get_text(buffer_, &start, &end, FALSE);

        std::string text = raw ? raw : "";
        if (raw) g_free(raw);

        apply_save_fixes(text);

        GError* error = nullptr;
        suppress_monitor_once_ = true;
        if (g_file_set_contents(filename, text.c_str(), (gssize)text.size(), &error)) {
            current_file_ = filename;
            update_language_for_filename(current_file_);
            add_recent_item(current_file_);
            install_file_monitor(current_file_);
            file_mtime_utc_us_ = get_file_mtime_us(current_file_);

            mark_modified(false);
            update_title();
            update_status_full();
        } else {
            std::cerr << "Error saving file: " << (error ? error->message : "unknown") << "\n";
            if (error) g_error_free(error);
        }

        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void Editor::open_containing_folder() {
    if (current_file_.empty()) return;
    std::string dir = dirname_of(current_file_);
    std::string uri = "file://" + dir;

    GError* err = nullptr;
    gtk_show_uri_on_window(GTK_WINDOW(window_), uri.c_str(), GDK_CURRENT_TIME, &err);
    if (err) g_error_free(err);
}

// ───────────────────────────────────────────────
//  Edit operations
// ───────────────────────────────────────────────

void Editor::cut() {
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_cut_clipboard(buffer_, cb, TRUE);
}
void Editor::copy() {
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_copy_clipboard(buffer_, cb);
}
void Editor::paste() {
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_paste_clipboard(buffer_, cb, nullptr, TRUE);
}
void Editor::select_all() {
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(buffer_, &s);
    gtk_text_buffer_get_end_iter(buffer_, &e);
    gtk_text_buffer_select_range(buffer_, &s, &e);
}

// ───────────────────────────────────────────────
//  Search / Replace / Go To
// ───────────────────────────────────────────────

void Editor::ensure_search_context() {
    if (!search_settings_) {
        search_settings_ = gtk_source_search_settings_new();
        gtk_source_search_settings_set_case_sensitive(search_settings_, FALSE);
        gtk_source_search_settings_set_wrap_around(search_settings_, FALSE);
    }
    if (!search_context_) {
        search_context_ = gtk_source_search_context_new(GTK_SOURCE_BUFFER(buffer_), search_settings_);
        gtk_source_search_context_set_highlight(search_context_, TRUE);
    }
}

void Editor::update_search_from_dialog(GtkWidget* dialog) {
    ensure_search_context();

    GtkWidget* entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dialog), "find_entry"));
    const char* t = entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
    gtk_source_search_settings_set_search_text(search_settings_, t ? t : "");
}

void Editor::search_find_next(bool backwards) {
    ensure_search_context();

    GtkTextIter iter, mstart, mend;
    gtk_text_buffer_get_iter_at_mark(buffer_, &iter, gtk_text_buffer_get_insert(buffer_));

    gboolean found = FALSE;
    if (backwards) {
        found = gtk_source_search_context_backward(search_context_, &iter, &mstart, &mend);
        if (!found) {
            // wrap to end
            gtk_text_buffer_get_end_iter(buffer_, &iter);
            found = gtk_source_search_context_backward(search_context_, &iter, &mstart, &mend);
        }
    } else {
        found = gtk_source_search_context_forward(search_context_, &iter, &mstart, &mend);
        if (!found) {
            // wrap to start
            gtk_text_buffer_get_start_iter(buffer_, &iter);
            found = gtk_source_search_context_forward(search_context_, &iter, &mstart, &mend);
        }
    }

    if (!found) {
        update_status_full();
        return;
    }

    gtk_text_buffer_select_range(buffer_, &mstart, &mend);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view_), &mstart, 0.2, FALSE, 0, 0);
    update_status_full();
}

void Editor::search_replace_one(const std::string& repl) {
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(buffer_, &s, &e)) {
        // no selection: do a find next first
        search_find_next(false);
        if (!gtk_text_buffer_get_selection_bounds(buffer_, &s, &e)) return;
    }

    gtk_text_buffer_begin_user_action(buffer_);
    gtk_text_buffer_delete(buffer_, &s, &e);
    gtk_text_buffer_insert(buffer_, &s, repl.c_str(), -1);
    gtk_text_buffer_end_user_action(buffer_);
}

void Editor::search_replace_all(const std::string& repl) {
    ensure_search_context();

    GtkTextIter iter, mstart, mend;
    gtk_text_buffer_get_start_iter(buffer_, &iter);

    int count = 0;
    gtk_text_buffer_begin_user_action(buffer_);

    while (gtk_source_search_context_forward(search_context_, &iter, &mstart, &mend)) {
        gtk_text_buffer_delete(buffer_, &mstart, &mend);
        gtk_text_buffer_insert(buffer_, &mstart, repl.c_str(), -1);
        count++;

        iter = mstart;
        gtk_text_iter_forward_chars(&iter, (gint)repl.size());
    }

    gtk_text_buffer_end_user_action(buffer_);
    (void)count;
}

void Editor::show_find_dialog() {
    if (find_dialog_) {
        gtk_window_present(GTK_WINDOW(find_dialog_));
        return;
    }

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Find",
        GTK_WINDOW(window_),
        GTK_DIALOG_MODAL,
        "_Close", GTK_RESPONSE_CLOSE,
        "_Find Next", GTK_RESPONSE_OK,
        nullptr);

    find_dialog_ = dialog;

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget* label = gtk_label_new("Find text:");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget* entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(dialog), "find_entry", entry);

    g_signal_connect(dialog, "response", G_CALLBACK(Editor::s_on_find_dialog_response), this);
    gtk_widget_show_all(dialog);
}

void Editor::show_replace_dialog() {
    if (replace_dialog_) {
        gtk_window_present(GTK_WINDOW(replace_dialog_));
        return;
    }

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Replace",
        GTK_WINDOW(window_),
        GTK_DIALOG_MODAL,
        "_Close", GTK_RESPONSE_CLOSE,
        "_Replace", GTK_RESPONSE_ACCEPT,
        "Replace _All", GTK_RESPONSE_APPLY,
        nullptr);

    replace_dialog_ = dialog;

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget* find_label = gtk_label_new("Find text:");
    gtk_box_pack_start(GTK_BOX(box), find_label, FALSE, FALSE, 0);

    GtkWidget* find_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(box), find_entry, FALSE, FALSE, 0);

    GtkWidget* repl_label = gtk_label_new("Replace with:");
    gtk_box_pack_start(GTK_BOX(box), repl_label, FALSE, FALSE, 0);

    GtkWidget* repl_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(box), repl_entry, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(dialog), "find_entry", find_entry);
    g_object_set_data(G_OBJECT(dialog), "repl_entry", repl_entry);

    g_signal_connect(dialog, "response", G_CALLBACK(Editor::s_on_replace_dialog_response), this);
    gtk_widget_show_all(dialog);
}

void Editor::show_goto_line_dialog() {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Go to Line",
        GTK_WINDOW(window_),
        GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Go", GTK_RESPONSE_OK,
        nullptr);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget* label = gtk_label_new("Line number (1-based):");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    g_object_set_data(G_OBJECT(dialog), "line_entry", entry);

    g_signal_connect(dialog, "response", G_CALLBACK(Editor::s_on_goto_line_response), this);
    gtk_widget_show_all(dialog);
}

void Editor::goto_line(int line) {
    if (line <= 0) return;

    int line_count = gtk_text_buffer_get_line_count(buffer_);
    if (line > line_count) line = line_count;

    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_line(buffer_, &iter, line - 1);
    gtk_text_buffer_place_cursor(buffer_, &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(text_view_), &iter, 0.2, FALSE, 0, 0);
    update_status_full();
}

// ───────────────────────────────────────────────
//  Syntax highlighting
// ───────────────────────────────────────────────

void Editor::update_language_for_filename(const std::string& filename) {
    if (!lang_manager_) return;

    std::string ext;
    auto dot = filename.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < filename.size())
        ext = to_lower(filename.substr(dot + 1));

    std::string lang_id;
    if (ext == "c") lang_id = "c";
    else if (ext == "cpp" || ext == "cc" || ext == "cxx" ||
             ext == "hpp" || ext == "hh" || ext == "hxx" || ext == "h")
        lang_id = "cpp";
    else if (ext == "py") lang_id = "python";
    else if (ext == "sh" || ext == "bash" || ext == "zsh") lang_id = "sh";
    else if (ext == "js") lang_id = "javascript";
    else if (ext == "html" || ext == "htm") lang_id = "html";
    else if (ext == "css") lang_id = "css";
    else if (ext == "json") lang_id = "json";
    else if (ext == "xml") lang_id = "xml";
    else if (ext == "md" || ext == "markdown") lang_id = "markdown";

    GtkSourceBuffer* srcb = GTK_SOURCE_BUFFER(buffer_);
    if (lang_id.empty()) {
        gtk_source_buffer_set_language(srcb, nullptr);
        gtk_source_buffer_set_highlight_syntax(srcb, FALSE);
        return;
    }

    GtkSourceLanguage* lang =
        gtk_source_language_manager_get_language(lang_manager_, lang_id.c_str());

    if (lang) {
        gtk_source_buffer_set_language(srcb, lang);
        gtk_source_buffer_set_highlight_syntax(srcb, TRUE);
    } else {
        gtk_source_buffer_set_language(srcb, nullptr);
        gtk_source_buffer_set_highlight_syntax(srcb, FALSE);
    }
}

// ───────────────────────────────────────────────
//  Status / title
// ───────────────────────────────────────────────

void Editor::update_title() {
    std::string title;
    if (current_file_.empty()) title = "Untitled — COLOSSUS Editor";
    else title = basename_of(current_file_) + " — COLOSSUS Editor";

    if (modified_) title = "*" + title;
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
}

void Editor::update_cursor_status() {
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark(buffer_, &iter, gtk_text_buffer_get_insert(buffer_));
    int line = gtk_text_iter_get_line(&iter) + 1;
    int col  = gtk_text_iter_get_line_offset(&iter) + 1;

    std::stringstream ss;
    ss << "Ln " << line << ", Col " << col;
    if (!current_file_.empty()) ss << "  —  " << current_file_;
    if (modified_) ss << "  (modified)";
    gtk_label_set_text(GTK_LABEL(status_bar_), ss.str().c_str());
}

void Editor::update_status_full() {
    update_cursor_status();
}

void Editor::mark_modified(bool is_modified) {
    modified_ = is_modified;
    update_title();
    update_status_full();
}

// ───────────────────────────────────────────────
//  Recent files
// ───────────────────────────────────────────────

void Editor::add_recent_item(const std::string& path) {
    if (!recent_mgr_ || path.empty()) return;

    std::string uri = "file://" + path;
    gtk_recent_manager_add_item(recent_mgr_, uri.c_str());
}

// ───────────────────────────────────────────────
//  Zoom
// ───────────────────────────────────────────────

void Editor::zoom_set(int pt) {
    if (pt < 7) pt = 7;
    if (pt > 32) pt = 32;
    font_pt_ = pt;

    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, "monospace");
    pango_font_description_set_size(desc, pt * PANGO_SCALE);

    // Deprecated but fine in GTK3 and easy
    gtk_widget_override_font(text_view_, desc);
    pango_font_description_free(desc);

    update_status_full();
}

void Editor::zoom_step(int delta) {
    zoom_set(font_pt_ + delta);
}

// ───────────────────────────────────────────────
//  Config / session
// ───────────────────────────────────────────────

void Editor::load_config() {
    ensure_dir_exists(config_dir());

    GKeyFile* kf = g_key_file_new();
    GError* err = nullptr;

    if (!g_key_file_load_from_file(kf, config_path().c_str(), G_KEY_FILE_NONE, &err)) {
        if (err) g_error_free(err);
        g_key_file_free(kf);
        return;
    }

    if (g_key_file_has_key(kf, "prefs", "trim_ws_on_save", nullptr))
        trim_ws_on_save_ = g_key_file_get_boolean(kf, "prefs", "trim_ws_on_save", nullptr);
    if (g_key_file_has_key(kf, "prefs", "ensure_newline_eof", nullptr))
        ensure_newline_eof_ = g_key_file_get_boolean(kf, "prefs", "ensure_newline_eof", nullptr);
    if (g_key_file_has_key(kf, "prefs", "tab_width", nullptr))
        tab_width_ = (int)g_key_file_get_integer(kf, "prefs", "tab_width", nullptr);
    if (g_key_file_has_key(kf, "prefs", "font_pt", nullptr))
        font_pt_ = (int)g_key_file_get_integer(kf, "prefs", "font_pt", nullptr);

    g_key_file_free(kf);
}

void Editor::save_config() {
    ensure_dir_exists(config_dir());

    GKeyFile* kf = g_key_file_new();
    g_key_file_set_boolean(kf, "prefs", "trim_ws_on_save", trim_ws_on_save_);
    g_key_file_set_boolean(kf, "prefs", "ensure_newline_eof", ensure_newline_eof_);
    g_key_file_set_integer(kf, "prefs", "tab_width", tab_width_);
    g_key_file_set_integer(kf, "prefs", "font_pt", font_pt_);

    gsize len = 0;
    gchar* data = g_key_file_to_data(kf, &len, nullptr);
    if (data) {
        g_file_set_contents(config_path().c_str(), data, (gssize)len, nullptr);
        g_free(data);
    }
    g_key_file_free(kf);
}

void Editor::load_session() {
    // simple: keep last_file
    ensure_dir_exists(config_dir());

    GKeyFile* kf = g_key_file_new();
    GError* err = nullptr;
    if (!g_key_file_load_from_file(kf, config_path().c_str(), G_KEY_FILE_NONE, &err)) {
        if (err) g_error_free(err);
        g_key_file_free(kf);
        return;
    }

    if (g_key_file_has_key(kf, "session", "last_file", nullptr)) {
        gchar* lf = g_key_file_get_string(kf, "session", "last_file", nullptr);
        if (lf && *lf) current_file_ = lf;
        if (lf) g_free(lf);
    }

    g_key_file_free(kf);
}

void Editor::save_session() {
    // store session back into config.ini (same file)
    ensure_dir_exists(config_dir());

    GKeyFile* kf = g_key_file_new();
    GError* err = nullptr;

    // load existing first to preserve prefs keys
    g_key_file_load_from_file(kf, config_path().c_str(), G_KEY_FILE_NONE, &err);
    if (err) { g_error_free(err); err = nullptr; }

    g_key_file_set_string(kf, "session", "last_file", current_file_.c_str());

    gsize len = 0;
    gchar* data = g_key_file_to_data(kf, &len, nullptr);
    if (data) {
        g_file_set_contents(config_path().c_str(), data, (gssize)len, nullptr);
        g_free(data);
    }
    g_key_file_free(kf);
}

// ───────────────────────────────────────────────
//  Static callbacks
// ───────────────────────────────────────────────

void Editor::on_activate(GtkApplication* app, gpointer) {
    if (!g_editor_instance) g_editor_instance = new Editor(app);
}

void Editor::on_open(GtkApplication* app, GFile** files, gint n_files, const gchar*, gpointer) {
    if (!g_editor_instance) g_editor_instance = new Editor(app);
    g_editor_instance->opened_via_cli_ = true;

    for (gint i = 0; i < n_files; ++i) {
        char* path = g_file_get_path(files[i]);
        if (path) {
            g_editor_instance->open_file_from_path(path);
            g_free(path);
        }
    }
    g_editor_instance->opened_via_cli_ = false;
}

void Editor::s_on_new_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->new_file(); }
void Editor::s_on_open_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->open_file(); }
void Editor::s_on_save_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->save_file(); }
void Editor::s_on_save_as_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->save_file_as(); }

void Editor::s_on_reload_activate(GtkWidget*, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    if (self->current_file_.empty()) return;
    if (self->modified_ && !self->maybe_confirm_discard("reload the file")) return;
    self->open_file_from_path(self->current_file_);
}

void Editor::s_on_open_folder_activate(GtkWidget*, gpointer ud) {
    static_cast<Editor*>(ud)->open_containing_folder();
}

void Editor::s_on_quit_activate(GtkWidget*, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    if (!self->maybe_confirm_discard("quit")) return;
    gtk_window_close(GTK_WINDOW(self->window_));
}

void Editor::s_on_cut_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->cut(); }
void Editor::s_on_copy_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->copy(); }
void Editor::s_on_paste_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->paste(); }
void Editor::s_on_select_all_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->select_all(); }

void Editor::s_on_find_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->show_find_dialog(); }
void Editor::s_on_replace_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->show_replace_dialog(); }
void Editor::s_on_goto_line_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->show_goto_line_dialog(); }

void Editor::s_on_buffer_changed(GtkTextBuffer*, gpointer ud) {
    static_cast<Editor*>(ud)->mark_modified(true);
}

void Editor::s_on_cursor_notify(GObject*, GParamSpec*, gpointer ud) {
    static_cast<Editor*>(ud)->update_cursor_status();
}

gboolean Editor::s_on_key_press(GtkWidget*, GdkEventKey* e, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);

    const bool ctrl = (e->state & GDK_CONTROL_MASK) != 0;
    if (!ctrl) return FALSE;

    // Ctrl+Plus / Ctrl+Equal (many keyboards)
    if (e->keyval == GDK_KEY_plus || e->keyval == GDK_KEY_equal) {
        self->zoom_step(+1);
        return TRUE;
    }
    if (e->keyval == GDK_KEY_minus) {
        self->zoom_step(-1);
        return TRUE;
    }
    if (e->keyval == GDK_KEY_0) {
        self->zoom_set(11);
        return TRUE;
    }

    // Find next / previous
    if (e->keyval == GDK_KEY_g) { // Ctrl+g often means find next
        self->search_find_next(false);
        return TRUE;
    }
    if (e->keyval == GDK_KEY_G) { // Ctrl+Shift+g (varies)
        self->search_find_next(true);
        return TRUE;
    }

    return FALSE;
}

void Editor::s_on_find_dialog_response(GtkDialog* dlg, gint resp, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);

    if (resp == GTK_RESPONSE_OK) {
        self->update_search_from_dialog(GTK_WIDGET(dlg));
        self->search_find_next(false);
        return; // keep dialog open
    }

    self->find_dialog_ = nullptr;
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

void Editor::s_on_replace_dialog_response(GtkDialog* dlg, gint resp, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);

    GtkWidget* find_entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "find_entry"));
    GtkWidget* repl_entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "repl_entry"));

    const char* f = find_entry ? gtk_entry_get_text(GTK_ENTRY(find_entry)) : "";
    const char* r = repl_entry ? gtk_entry_get_text(GTK_ENTRY(repl_entry)) : "";

    self->ensure_search_context();
    gtk_source_search_settings_set_search_text(self->search_settings_, f ? f : "");

    if (resp == GTK_RESPONSE_ACCEPT) {
        // replace one (on selection if exists; else find next then replace)
        self->search_find_next(false);
        self->search_replace_one(r ? r : "");
        return;
    }
    if (resp == GTK_RESPONSE_APPLY) {
        self->search_replace_all(r ? r : "");
        return;
    }

    self->replace_dialog_ = nullptr;
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

void Editor::s_on_goto_line_response(GtkDialog* dlg, gint resp, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    if (resp == GTK_RESPONSE_OK) {
        GtkWidget* entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg), "line_entry"));
        const char* t = entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
        int line = t ? std::atoi(t) : 0;
        if (line > 0) self->goto_line(line);
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

void Editor::s_on_recent_activated(GtkRecentChooser* chooser, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    gchar* uri = gtk_recent_chooser_get_current_uri(chooser);
    if (!uri) return;

    gchar* path = g_filename_from_uri(uri, nullptr, nullptr);
    if (path) {
        self->opened_via_cli_ = true;
        self->open_file_from_path(path);
        self->opened_via_cli_ = false;
        g_free(path);
    }
    g_free(uri);
}

void Editor::s_on_file_monitor_changed(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    if (!self || self->current_file_.empty()) return;

    // ignore noise / and one-time suppress after our own writes
    if (self->suppress_monitor_once_) {
        self->suppress_monitor_once_ = false;
        return;
    }

    if (ev != G_FILE_MONITOR_EVENT_CHANGED &&
        ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        ev != G_FILE_MONITOR_EVENT_CREATED &&
        ev != G_FILE_MONITOR_EVENT_MOVED_IN)
        return;

    guint64 now_mtime = self->get_file_mtime_us(self->current_file_);
    if (now_mtime == 0) return;
    if (now_mtime == self->file_mtime_utc_us_) return;

    self->file_mtime_utc_us_ = now_mtime;

    // if we have unsaved changes, ask; else reload silently
    if (self->modified_) {
        if (self->confirm_reload_external("The file was modified externally.")) {
            self->open_file_from_path(self->current_file_);
        }
    } else {
        self->open_file_from_path(self->current_file_);
    }
}

void Editor::s_on_zoom_in_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->zoom_step(+1); }
void Editor::s_on_zoom_out_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->zoom_step(-1); }
void Editor::s_on_zoom_reset_activate(GtkWidget*, gpointer ud) { static_cast<Editor*>(ud)->zoom_set(11); }

void Editor::s_on_toggle_trim_ws(GtkWidget* w, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    self->trim_ws_on_save_ = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
}
void Editor::s_on_toggle_eof_nl(GtkWidget* w, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    self->ensure_newline_eof_ = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
}
void Editor::s_on_spaces_toggle(GtkWidget* w, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    gboolean on = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
    gtk_source_view_set_insert_spaces_instead_of_tabs(GTK_SOURCE_VIEW(self->text_view_), on);
}
void Editor::s_on_tab_width_2(GtkWidget*, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    self->tab_width_ = 2;
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->text_view_), self->tab_width_);
}
void Editor::s_on_tab_width_4(GtkWidget*, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    self->tab_width_ = 4;
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->text_view_), self->tab_width_);
}
void Editor::s_on_tab_width_8(GtkWidget*, gpointer ud) {
    Editor* self = static_cast<Editor*>(ud);
    self->tab_width_ = 8;
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(self->text_view_), self->tab_width_);
}

// ───────────────────────────────────────────────
//  Run function
// ───────────────────────────────────────────────

int run_colossus_editor(int argc, char** argv) {
    GtkApplication* app = gtk_application_new(
        "tech.will.colossus_editor",
        (GApplicationFlags)(G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE)
    );

    g_signal_connect(app, "activate", G_CALLBACK(Editor::on_activate), nullptr);
    g_signal_connect(app, "open",     G_CALLBACK(Editor::on_open),     nullptr);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
