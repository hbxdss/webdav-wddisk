// gui.mm - macOS 原生 AppKit 界面
#import <Cocoa/Cocoa.h>
#import <sys/mount.h>
#include "../common/config.h"
#include "webdav_fuse.h"

using namespace wddisk;

static ConfigStore* g_cfg = nullptr;
static std::map<int, DavFuseFs*> g_mounted;
static std::map<int, NSThread*> g_fuseThreads;

static std::string nsToStd(NSString* s) { return s ? std::string([s UTF8String]) : ""; }
static NSString* stdToNs(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

static NSString* configDir() {
    NSString* dir = [NSHomeDirectory() stringByAppendingPathComponent:@".wddisk"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

// ---------------- 编辑窗口 ----------------
@interface EditWindow : NSWindow <NSTextFieldDelegate>
@end

// ---------------- 主控制器 ----------------
@interface AppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate>
{
    NSWindow* _window;
    NSTableView* _table;
    NSTextField* _status;
    NSButton* _btnAdd, *_btnEdit, *_btnDel, *_btnMount, *_btnUnmount, *_btnOpen;
    NSMutableArray* _rows; // dictionaries
}
- (void)refreshRows;
- (void)updateStatus:(NSString*)s;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    NSRect frame = NSMakeRect(0, 0, 760, 460);
    _window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [_window setTitle:@"Wddisk · WebDAV 网络磁盘"];
    [_window center];

    NSView* content = [_window contentView];

    // 标题
    NSTextField* title = [NSTextField labelWithString:@"Wddisk · WebDAV 网络磁盘"];
    title.font = [NSFont boldSystemFontOfSize:18];
    title.frame = NSMakeRect(20, frame.size.height - 44, 400, 24);
    [content addSubview:title];

    // 表格
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 76, frame.size.width - 40, frame.size.height - 128)];
    scroll.borderType = NSBezelBorder;
    scroll.hasVerticalScroller = YES;
    _table = [[NSTableView alloc] init];
    _table.dataSource = self;
    _table.delegate = self;
    _table.rowHeight = 26;
    _table.allowsColumnReordering = NO;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.headerView.height = 24;
    NSTableColumn* c;
    auto addCol = ^(NSString* name, CGFloat w) {
        c = [[NSTableColumn alloc] initWithIdentifier:name];
        c.width = w;
        c.title = name;
        [_table addTableColumn:c];
    };
    addCol(@"name", 130); addCol(@"url", 230); addCol(@"user", 110); addCol(@"mount", 110); addCol(@"status", 110);
    _table.headerView = _table.headerView;
    scroll.documentView = _table;
    [content addSubview:scroll];

    // 按钮行
    auto mkBtn = ^(NSString* text, int x, SEL action) {
        NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(x, 34, 100, 30)];
        b.title = text;
        b.bezelStyle = NSBezelStyleRounded;
        b.target = self;
        b.action = action;
        [content addSubview:b];
        return b;
    };
    _btnAdd = mkBtn(@"添加", 20, @selector(onAdd:));
    _btnEdit = mkBtn(@"编辑", 124, @selector(onEdit:));
    _btnDel = mkBtn(@"删除", 228, @selector(onDel:));
    _btnMount = mkBtn(@"挂载", 360, @selector(onMount:));
    _btnUnmount = mkBtn(@"卸载", 464, @selector(onUnmount:));
    _btnOpen = mkBtn(@"打开", 568, @selector(onOpen:));

    // 状态栏
    _status = [NSTextField labelWithString:@"就绪"];
    _status.frame = NSMakeRect(20, 8, frame.size.width - 40, 18);
    _status.textColor = [NSColor secondaryLabelColor];
    [content addSubview:_status];

    [self refreshRows];
    [_window makeKeyAndOrderFront:nil];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    return (NSInteger)g_cfg->servers().size();
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    const DavConfig& s = g_cfg->servers()[row];
    NSString* text = @"";
    NSString* id = col.identifier;
    if ([id isEqualToString:@"name"]) text = stdToNs(s.name);
    else if ([id isEqualToString:@"url"]) text = stdToNs(s.url);
    else if ([id isEqualToString:@"user"]) text = stdToNs(s.username);
    else if ([id isEqualToString:@"mount"]) text = stdToNs(s.mountPoint);
    else {
        text = g_mounted.count((int)row) ? @"● 已挂载" : @"未挂载";
    }
    NSTextField* cell = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!cell) {
        cell = [NSTextField labelWithString:@""];
        cell.identifier = @"cell";
    }
    cell.stringValue = text;
    if ([id isEqualToString:@"status"]) {
        cell.textColor = g_mounted.count((int)row) ? [NSColor systemGreenColor] : [NSColor secondaryLabelColor];
    } else {
        cell.textColor = [NSColor labelColor];
    }
    return cell;
}

- (void)refreshRows { [_table reloadData]; }
- (void)updateStatus:(NSString*)s { _status.stringValue = s; }

- (NSInteger)selectedRow {
    NSInteger r = _table.selectedRow;
    if (r < 0) { [self updateStatus:@"请先选择一个服务器"]; }
    return r;
}

- (void)onAdd:(id)s { [self openEditor:-1]; }
- (void)onEdit:(id)s {
    NSInteger r = [self selectedRow];
    if (r >= 0) [self openEditor:(int)r];
}
- (void)onDel:(id)s {
    NSInteger r = [self selectedRow];
    if (r < 0) return;
    [self doUnmount:(int)r];
    g_cfg->servers().erase(g_cfg->servers().begin() + r);
    g_cfg->save();
    [self refreshRows];
    [self updateStatus:@"已删除"];
}
- (void)onMount:(id)s {
    NSInteger r = [self selectedRow];
    if (r >= 0) [self doMount:(int)r];
}
- (void)onUnmount:(id)s {
    NSInteger r = [self selectedRow];
    if (r >= 0) [self doUnmount:(int)r];
}
- (void)onOpen:(id)s {
    NSInteger r = [self selectedRow];
    if (r < 0) return;
    if (!g_mounted.count((int)r)) [self doMount:(int)r];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:stdToNs(g_cfg->servers()[r].mountPoint)]];
}

// ---------------- 挂载 ----------------
- (void)doMount:(int)idx {
    if (g_mounted.count(idx)) { [self updateStatus:@"该服务器已挂载"]; return; }
    DavConfig& s = g_cfg->servers()[idx];

    // 挂载点：~/Wddisk/<name>
    if (s.mountPoint.empty()) {
        s.mountPoint = nsToStd([NSString stringWithFormat:@"%@/Wddisk/%@",
            NSHomeDirectory(), stdToNs(s.name.empty() ? "server" : s.name)]);
        g_cfg->save();
    }
    NSString* mp = stdToNs(s.mountPoint);
    [[NSFileManager defaultManager] createDirectoryAtPath:mp withIntermediateDirectories:YES attributes:nil error:nil];

    NSString* cacheDir = [configDir() stringByAppendingPathComponent:@"cache"];
    DavFuseFs* fs = new DavFuseFs(s, nsToStd(cacheDir));

    // 后台线程运行 fuse 主循环
    NSThread* t = [[NSThread alloc] initWithBlock:^{
        fs->run(s.mountPoint);
    }];
    t.name = @"wddisk-fuse";
    [t start];
    g_mounted[idx] = fs;
    g_fuseThreads[idx] = t;
    // 给 fuse 一点时间挂载
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
            struct stat st;
            if (::stat(s.mountPoint.c_str(), &st) == 0)
                [self updateStatus:[NSString stringWithFormat:@"已挂载 %@ -> %@", stdToNs(s.name), mp]];
            else
                [self updateStatus:@"挂载失败：请确认已安装 macFUSE (osxfuse)"];
            [self refreshRows];
        });
}

- (void)doUnmount:(int)idx {
    auto it = g_mounted.find(idx);
    if (it == g_mounted.end()) { [self updateStatus:@"该服务器未挂载"]; return; }
    DavFuseFs* fs = it->second;
    // 通过 diskutil 卸载
    std::string cmd = "umount '" + fs->cfg_.mountPoint + "' 2>/dev/null; diskutil unmount '" + fs->cfg_.mountPoint + "' 2>/dev/null";
    system(cmd.c_str());
    g_mounted.erase(it);
    g_fuseThreads.erase(idx);
    [self updateStatus:@"已卸载"];
    [self refreshRows];
}

// ---------------- 编辑器 ----------------
- (void)openEditor:(int)editIdx {
    bool isNew = editIdx < 0;
    DavConfig c;
    if (!isNew) c = g_cfg->servers()[editIdx];

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = isNew ? @"添加服务器" : @"编辑服务器";
    [alert addButtonWithTitle:@"确定"];
    [alert addButtonWithTitle:@"取消"];
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 230)];
    alert.accessoryView = view;

    auto mkLabel = ^(NSString* text, int y) {
        NSTextField* l = [NSTextField labelWithString:text];
        l.frame = NSMakeRect(0, y, 90, 18);
        [view addSubview:l];
    };
    auto mkField = ^(NSString* val, int y, BOOL secure) {
        NSTextField* f = secure
            ? [[NSSecureTextField alloc] initWithFrame:NSMakeRect(100, y, 300, 24)]
            : [[NSTextField alloc] initWithFrame:NSMakeRect(100, y, 300, 24)];
        f.stringValue = val;
        [view addSubview:f];
        return f;
    };

    mkLabel(@"名称", 200);
    NSTextField* fName = mkField(stdToNs(c.name), 200, NO);
    mkLabel(@"服务器 URL", 166);
    NSTextField* fUrl = mkField(stdToNs(c.url), 166, NO);
    mkLabel(@"用户名", 132);
    NSTextField* fUser = mkField(stdToNs(c.username), 132, NO);
    mkLabel(@"密码", 98);
    NSTextField* fPass = mkField(stdToNs(c.password), 98, YES);
    mkLabel(@"挂载目录名", 64);
    NSTextField* fMount = mkField(stdToNs(c.mountPoint), 64, NO);
    NSButton* chk = [[NSButton alloc] initWithFrame:NSMakeRect(100, 30, 300, 20)];
    chk.title = @"跳过 TLS 证书验证";
    chk.setButtonType = NSSwitchButton;
    chk.state = c.verifyTls ? NSControlStateValueOff : NSControlStateValueOn;
    [view addSubview:chk];

    NSModalResponse resp = [alert runModal];
    if (resp != NSAlertFirstButtonReturn) return;

    DavConfig nc;
    nc.name = nsToStd(fName.stringValue);
    nc.url = nsToStd(fUrl.stringValue);
    nc.username = nsToStd(fUser.stringValue);
    nc.password = nsToStd(fPass.stringValue);
    nc.mountPoint = nsToStd(fMount.stringValue);
    nc.verifyTls = (chk.state != NSControlStateValueOn);
    if (nc.name.empty()) nc.name = "服务器";
    while (nc.url.size() > 1 && nc.url.back() == '/') nc.url.pop_back();
    if (isNew) g_cfg->servers().push_back(nc);
    else g_cfg->servers()[editIdx] = nc;
    g_cfg->save();
    [self refreshRows];
}

@end

int main(int argc, const char** argv) {
    curl_global_init(CURL_GLOBAL_ALL);
    @autoreleasepool {
        static std::string cfgPath = nsToStd([configDir() stringByAppendingPathComponent:@"config.json"]);
        static ConfigStore store(cfgPath);
        g_cfg = &store;

        NSApplication* app = [NSApplication sharedApplication];
        AppDelegate* del = [[AppDelegate alloc] init];
        app.delegate = del;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    curl_global_cleanup();
    return 0;
}
