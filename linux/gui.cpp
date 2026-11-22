// gui.cpp - Linux 原生 GTK3 界面
#include <gtk/gtk.h>
#include "../common/config.h"
#include "webdav_fuse3.h"
#include <thread>
#include <map>
#include <string>
#include <csignal>

using namespace wddisk;

static ConfigStore* g_cfg = nullptr;
static std::map<int, DavFuseFs*> g_mounted;
static std::map<int, std::thread*> g_threads;

enum { COL_NAME = 0, COL_URL, COL_USER, COL_MOUNT, COL_STATUS, N_COLS };

static GtkWidget *g_view, *g_status;
static GtkListStore* g_store;
static GtkTreeSelection* g_sel;

static void updateStatus(const std::string& s) {
    gtk_label_set_text(GTK_LABEL(g_status), s.c_str());
}

static void refreshRows() {
    gtk_list_store_clear(g_store);
    auto& sv = g_cfg->servers();
    for (size_t i = 0; i < sv.size(); ++i) {
        GtkTreeIter it;
        gtk_list_store_append(g_store, &it);
        gtk_list_store_set(g_store, &it,
            COL_NAME, sv[i].name.c_str(),
            COL_URL, sv[i].url.c_str(),
            COL_USER, sv[i].username.c_str(),
            COL_MOUNT, sv[i].mountPoint.c_str(),
            COL_STATUS, g_mounted.count((int)i) ? "● 已挂载" : "未挂载",
            -1);
    }
}

static int selectedRow() {
    GtkTreeIter it;
    GtkTreeModel* model;
    if (!gtk_tree_selection_get_selected(g_sel, &model, &it)) {
        updateStatus("请先选择一个服务器");
        return -1;
    }
    GtkTreePath* path = gtk_tree_model_get_path(model, &it);
    int idx = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return idx;
}

// ---------------- 挂载 ----------------
static void doMount(int idx) {
    if (g_mounted.count(idx)) { updateStatus("该服务器已挂载"); return; }
    DavConfig& s = g_cfg->servers()[idx];
    if (s.mountPoint.empty()) {
        s.mountPoint = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/Wddisk/" + s.name;
        g_cfg->save();
    }
    std::string cmd = "mkdir -p '" + s.mountPoint + "'";
    system(cmd.c_str());

    std::string cacheDir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.wddisk/cache";
    DavFuseFs* fs = new DavFuseFs(s, cacheDir);
    std::string mp = s.mountPoint;
    auto* t = new std::thread([fs, mp]() { fs->run(mp); });
    g_mounted[idx] = fs;
    g_threads[idx] = t;
    updateStatus("已挂载 " + s.name + " -> " + s.mountPoint + "（如未生效请确认 fusermount3 可用）");
    refreshRows();
}

static void doUnmount(int idx) {
    auto it = g_mounted.find(idx);
    if (it == g_mounted.end()) { updateStatus("该服务器未挂载"); return; }
    std::string cmd = "fusermount3 -u '" + it->second->cfg_.mountPoint + "' 2>/dev/null || fusermount -u '" + it->second->cfg_.mountPoint + "'";
    system(cmd.c_str());
    g_mounted.erase(it);
    updateStatus("已卸载");
    refreshRows();
}

// ---------------- 编辑器 ----------------
static void openEditor(GtkWindow* parent, int editIdx) {
    bool isNew = editIdx < 0;
    DavConfig c;
    if (!isNew) c = g_cfg->servers()[editIdx];

    GtkWidget* dlg = gtk_dialog_new_with_buttons(isNew ? "添加服务器" : "编辑服务器", parent,
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "取消", GTK_RESPONSE_CANCEL, "确定", GTK_RESPONSE_OK, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 440, 280);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_container_add(GTK_CONTAINER(area), grid);

    auto mkLabel = [&](int r, const char* text) {
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(text), 0, r, 1, 1);
    };
    auto mkEntry = [&](int r, const std::string& val, bool secret) {
        GtkWidget* e = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(e), val.c_str());
        if (secret) gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
        gtk_widget_set_hexpand(e, TRUE);
        gtk_grid_attach(GTK_GRID(grid), e, 1, r, 1, 1);
        return e;
    };

    mkLabel(0, "名称");        GtkWidget* eName = mkEntry(0, c.name, false);
    mkLabel(1, "服务器 URL");  GtkWidget* eUrl = mkEntry(1, c.url, false);
    mkLabel(2, "用户名");      GtkWidget* eUser = mkEntry(2, c.username, false);
    mkLabel(3, "密码");        GtkWidget* ePass = mkEntry(3, c.password, true);
    mkLabel(4, "挂载目录");    GtkWidget* eMount = mkEntry(4, c.mountPoint, false);
    GtkWidget* chk = gtk_check_button_new_with_label("跳过 TLS 证书验证");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), !c.verifyTls);
    gtk_grid_attach(GTK_GRID(grid), chk, 1, 5, 1, 1);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        DavConfig nc;
        nc.name = gtk_entry_get_text(GTK_ENTRY(eName));
        nc.url = gtk_entry_get_text(GTK_ENTRY(eUrl));
        nc.username = gtk_entry_get_text(GTK_ENTRY(eUser));
        nc.password = gtk_entry_get_text(GTK_ENTRY(ePass));
        nc.mountPoint = gtk_entry_get_text(GTK_ENTRY(eMount));
        nc.verifyTls = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk));
        if (nc.name.empty()) nc.name = "服务器";
        while (nc.url.size() > 1 && nc.url.back() == '/') nc.url.pop_back();
        if (isNew) g_cfg->servers().push_back(nc);
        else g_cfg->servers()[editIdx] = nc;
        g_cfg->save();
        refreshRows();
    }
    gtk_widget_destroy(dlg);
}

// ---------------- 回调 ----------------
static void onAdd(GtkWidget*, gpointer data) { openEditor(GTK_WINDOW(data), -1); }

static void onEdit(GtkWidget*, gpointer data) {
    int idx = selectedRow();
    if (idx >= 0) openEditor(GTK_WINDOW(data), idx);
}

static void onDel(GtkWidget*, gpointer) {
    int idx = selectedRow();
    if (idx < 0) return;
    doUnmount(idx);
    g_cfg->servers().erase(g_cfg->servers().begin() + idx);
    g_cfg->save();
    refreshRows();
    updateStatus("已删除");
}

static void onMount(GtkWidget*, gpointer) {
    int idx = selectedRow();
    if (idx >= 0) doMount(idx);
}

static void onUnmount(GtkWidget*, gpointer) {
    int idx = selectedRow();
    if (idx >= 0) doUnmount(idx);
}

static void onOpen(GtkWidget*, gpointer data) {
    int idx = selectedRow();
    if (idx < 0) return;
    if (!g_mounted.count(idx)) doMount(idx);
    std::string cmd = "xdg-open '" + g_cfg->servers()[idx].mountPoint + "' &";
    system(cmd.c_str());
    (void)data;
}

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_ALL);

    std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
    system(("mkdir -p '" + home + "/.wddisk'").c_str());
    static std::string cfgPath = home + "/.wddisk/config.json";
    static ConfigStore store(cfgPath);
    g_cfg = &store;

    gtk_init(&argc, &argv);

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Wddisk · WebDAV 网络磁盘");
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 460);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='x-large' weight='bold'>Wddisk · WebDAV 网络磁盘</span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

    // 列表
    g_store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_store));
    g_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_view));
    gtk_tree_selection_set_mode(g_sel, GTK_SELECTION_SINGLE);
    const char* titles[N_COLS] = { "名称", "服务器", "用户名", "挂载点", "状态" };
    for (int i = 0; i < N_COLS; ++i) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_view), col);
    }
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    // 按钮行
    GtkWidget* btnBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto mkBtn = [&](const char* text, GCallback cb) {
        GtkWidget* b = gtk_button_new_with_label(text);
        g_signal_connect(b, "clicked", cb, win);
        gtk_box_pack_start(GTK_BOX(btnBox), b, FALSE, FALSE, 0);
    };
    mkBtn("添加", G_CALLBACK(onAdd));
    mkBtn("编辑", G_CALLBACK(onEdit));
    mkBtn("删除", G_CALLBACK(onDel));
    mkBtn("挂载", G_CALLBACK(onMount));
    mkBtn("卸载", G_CALLBACK(onUnmount));
    mkBtn("打开", G_CALLBACK(onOpen));
    gtk_box_pack_start(GTK_BOX(vbox), btnBox, FALSE, FALSE, 0);

    g_status = gtk_label_new("就绪");
    gtk_widget_set_halign(g_status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);

    refreshRows();
    gtk_widget_show_all(win);
    gtk_main();

    curl_global_cleanup();
    return 0;
}
