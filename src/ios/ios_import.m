/*
 * ios_import.m -- bring the user's own extracted disc into the app.
 *
 * Runs before the title boots, when NFSU2_GAME_DIR (Documents/game) has no
 * default.xbe: a plain screen asks for the folder holding the extracted
 * Xbox disc (default.xbe, NFSUNDER/, ...) through the system folder picker,
 * copies it with a progress bar into <game>.import, checks that default.xbe
 * is the NTSC-U build the lifted code was made from, and only then moves it
 * into place. Copying through Finder or the Files app into Documents/game
 * still works and skips all this. NFSU2_IMPORT_FROM=<folder> starts the
 * same import from that folder without the picker (the Simulator can read
 * the Mac's disc folder: tests).
 *
 * Main thread only for UIKit; the copy runs on a background queue. The
 * caller's main() turns the run loop here until the game is in place.
 */
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "ios_scene.h"

/* The XBE the lifted code in gen/ comes from (docs/IOS_PORT_LOG.md). */
#define XBE_SIZE 4038656ull
#define XBE_SHA1 "428b5a70b9400f19cc5be7679db6c72a6f80a1c9"
#define COPY_CHUNK (4u << 20)

static NSString *xbe_sha1(NSString *path)
{
    unsigned char md[CC_SHA1_DIGEST_LENGTH];
    NSMutableString *hex = [NSMutableString string];
    FILE *f = fopen(path.fileSystemRepresentation, "rb");
    char buf[1 << 16];
    size_t n;
    CC_SHA1_CTX c;

    if (!f)
        return nil;
    /* Identifies a known file; nothing here relies on SHA-1 for security. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA1_Init(&c);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        CC_SHA1_Update(&c, buf, (CC_LONG)n);
    CC_SHA1_Final(md, &c);
#pragma clang diagnostic pop
    fclose(f);
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++)
        [hex appendFormat:@"%02x", md[i]];
    return hex;
}

/* The picked folder, or a folder right inside it, that holds default.xbe. */
static NSURL *disc_root(NSURL *picked)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray<NSURL *> *kids;

    if ([fm fileExistsAtPath:[picked URLByAppendingPathComponent:@"default.xbe"].path])
        return picked;
    kids = [fm contentsOfDirectoryAtURL:picked includingPropertiesForKeys:nil options:0 error:nil];
    for (NSURL *k in kids)
        if ([fm fileExistsAtPath:[k URLByAppendingPathComponent:@"default.xbe"].path])
            return k;
    return nil;
}

static BOOL copy_file(NSString *from, NSString *to, char *buf,
                      void (^progress)(uint64_t), NSString **err)
{
    FILE *in = fopen(from.fileSystemRepresentation, "rb");
    FILE *out = in ? fopen(to.fileSystemRepresentation, "wb") : NULL;
    size_t n;
    BOOL ok = YES;

    if (!in || !out) {
        *err = [NSString stringWithFormat:@"%@: %s%@", from.lastPathComponent, strerror(errno),
                errno == ENOENT ? @" (if it is in iCloud, download it in Files first)" : @""];
        if (in)
            fclose(in);
        return NO;
    }
    while ((n = fread(buf, 1, COPY_CHUNK, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            *err = [NSString stringWithFormat:@"writing %@: %s", to.lastPathComponent, strerror(errno)];
            ok = NO;
            break;
        }
        progress(n);
    }
    if (ok && ferror(in)) {
        *err = [NSString stringWithFormat:@"reading %@: %s", from.lastPathComponent, strerror(errno)];
        ok = NO;
    }
    fclose(in);
    if (fclose(out) != 0 && ok) {
        *err = [NSString stringWithFormat:@"writing %@: %s", to.lastPathComponent, strerror(errno)];
        ok = NO;
    }
    return ok;
}

@interface NFSImportController : UIViewController <UIDocumentPickerDelegate>
@property (nonatomic, copy) NSString *gameDir;
@property (nonatomic) BOOL done;
- (void)importFrom:(NSURL *)picked;
@end

@implementation NFSImportController {
    UILabel        *_status;
    UIButton       *_choose;
    UIProgressView *_bar;
}

- (void)viewDidLoad
{
    UILabel *title = [UILabel new], *body = [UILabel new];
    UIStackView *stack;
    UIButtonConfiguration *cfg = [UIButtonConfiguration filledButtonConfiguration];

    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    title.text = @"Need for Speed: Underground 2";
    title.font = [UIFont boldSystemFontOfSize:24];
    title.textColor = UIColor.whiteColor;
    body.text = @"Choose the folder with your extracted Xbox disc (NTSC-U): the one that "
                @"holds default.xbe and NFSUNDER. It is copied into the app (about 2.5 GB). "
                @"You can also copy it into the game folder with the Files app or Finder.";
    body.numberOfLines = 0;
    body.textAlignment = NSTextAlignmentCenter;
    body.textColor = [UIColor colorWithWhite:0.8 alpha:1];
    cfg.title = @"Choose Game Folder…";
    _choose = [UIButton buttonWithConfiguration:cfg primaryAction:
               [UIAction actionWithHandler:^(UIAction *a) { [self pick]; }]];
    _bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _bar.hidden = YES;
    _status = [UILabel new];
    _status.numberOfLines = 0;
    _status.textAlignment = NSTextAlignmentCenter;
    _status.textColor = [UIColor colorWithWhite:0.7 alpha:1];
    _status.font = [UIFont systemFontOfSize:14];

    stack = [[UIStackView alloc] initWithArrangedSubviews:@[ title, body, _choose, _bar, _status ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentCenter;
    stack.spacing = 18;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:560],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.widthAnchor
                                                    constant:-40],
        [body.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [_status.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [_bar.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
    ]];
}

- (void)pick
{
    UIDocumentPickerViewController *p =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
    p.delegate = self;
    p.allowsMultipleSelection = NO;
    [self presentViewController:p animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    if (urls.count)
        [self importFrom:urls.firstObject];
}

- (void)fail:(NSString *)why
{
    fprintf(stderr, "[ios] import failed: %s\n", why.UTF8String);
    _status.text = why;
    _bar.hidden = YES;
    _choose.enabled = YES;
}

- (void)importFrom:(NSURL *)picked
{
    NSString *game = self.gameDir, *tmp = [game stringByAppendingString:@".import"];
    BOOL scoped = [picked startAccessingSecurityScopedResource];
    UIApplication *app = UIApplication.sharedApplication;
    __block UIBackgroundTaskIdentifier task;

    _choose.enabled = NO;
    _bar.hidden = NO;
    _bar.progress = 0;
    _status.text = @"Copying…";
    app.idleTimerDisabled = YES;
    task = [app beginBackgroundTaskWithExpirationHandler:^{
        [app endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
    }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *err = [self copyFrom:picked into:tmp game:game];
        if (scoped)
            [picked stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            app.idleTimerDisabled = NO;
            if (task != UIBackgroundTaskInvalid)
                [app endBackgroundTask:task];
            if (err) {
                [self fail:err];
            } else {
                fprintf(stderr, "[ios] game imported into %s\n", game.fileSystemRepresentation);
                self.done = YES;
            }
        });
    });
}

/* Background queue. Returns nil when the game is in place, else why not. */
- (NSString *)copyFrom:(NSURL *)picked into:(NSString *)tmp game:(NSString *)game
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *keys = @[ NSURLIsDirectoryKey, NSURLFileSizeKey ];
    NSURL *root = disc_root(picked);
    NSMutableArray<NSURL *> *items = [NSMutableArray array];
    NSDictionary *space;
    NSError *e = nil;
    __block uint64_t done = 0, total = 0, shown = 0;
    NSString *err = nil, *sha;
    char *buf;

    if (!root)
        return @"No default.xbe in that folder. Pick the folder of the extracted disc.";
    for (NSURL *u in [fm enumeratorAtURL:root includingPropertiesForKeys:keys options:0 errorHandler:nil]) {
        NSNumber *dir = nil, *size = nil;
        [u getResourceValue:&dir forKey:NSURLIsDirectoryKey error:nil];
        [u getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        [items addObject:u];
        if (!dir.boolValue)
            total += size.unsignedLongLongValue;
    }
    space = [[NSURL fileURLWithPath:game.stringByDeletingLastPathComponent]
             resourceValuesForKeys:@[ NSURLVolumeAvailableCapacityForImportantUsageKey ] error:nil];
    if ([space[NSURLVolumeAvailableCapacityForImportantUsageKey] unsignedLongLongValue] < total + (256ull << 20))
        return [NSString stringWithFormat:@"Not enough free space: the game needs %.1f GB.",
                total / 1e9];

    [fm removeItemAtPath:tmp error:nil];
    if (![fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:&e])
        return e.localizedDescription;
    buf = malloc(COPY_CHUNK);
    if (!buf)
        return @"Out of memory.";
    fprintf(stderr, "[ios] importing %s (%llu MB)\n", root.fileSystemRepresentation,
            (unsigned long long)(total >> 20));
    for (NSURL *u in items) {
        NSString *rel = [u.path substringFromIndex:root.path.length];
        NSString *dst = [tmp stringByAppendingPathComponent:rel];
        NSNumber *dir = nil;
        [u getResourceValue:&dir forKey:NSURLIsDirectoryKey error:nil];
        if (dir.boolValue) {
            if (![fm createDirectoryAtPath:dst withIntermediateDirectories:YES attributes:nil error:&e]) {
                err = e.localizedDescription;
                break;
            }
            continue;
        }
        if (!copy_file(u.path, dst, buf, ^(uint64_t n) {
                done += n;
                if (done - shown >= (32ull << 20) || done == total) {
                    float f = total ? (float)done / (float)total : 1.0f;
                    shown = done;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        self->_bar.progress = f;
                        self->_status.text = [NSString stringWithFormat:@"Copying… %.0f%%", f * 100];
                    });
                }
            }, &err))
            break;
    }
    free(buf);

    if (!err) {
        NSString *xbe = [tmp stringByAppendingPathComponent:@"default.xbe"];
        NSDictionary *a = [fm attributesOfItemAtPath:xbe error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{ self->_status.text = @"Checking default.xbe…"; });
        sha = xbe_sha1(xbe);
        if (a.fileSize != XBE_SIZE || ![sha isEqualToString:@XBE_SHA1])
            err = [NSString stringWithFormat:@"This default.xbe is not the North American (NTSC-U) "
                   @"release this port is built from (SHA-1 %@).", sha ?: @"unreadable"];
    }
    /* In place: replace what a partial earlier copy left, item by item. */
    if (!err) {
        [fm createDirectoryAtPath:game withIntermediateDirectories:YES attributes:nil error:nil];
        for (NSString *name in [fm contentsOfDirectoryAtPath:tmp error:nil]) {
            NSString *to = [game stringByAppendingPathComponent:name];
            [fm removeItemAtPath:to error:nil];
            if (![fm moveItemAtPath:[tmp stringByAppendingPathComponent:name] toPath:to error:&e]) {
                err = e.localizedDescription;
                break;
            }
        }
    }
    [fm removeItemAtPath:tmp error:nil];
    return err;
}
@end

/* 1 once <game_dir>/default.xbe exists: at once, or after an import. */
int ios_import_game(const char *game_dir)
{
    NSString *game = [NSString stringWithUTF8String:game_dir];
    UIWindowScene *scene;
    UIWindow *win;
    NFSImportController *vc;

    if ([NSFileManager.defaultManager fileExistsAtPath:[game stringByAppendingPathComponent:@"default.xbe"]])
        return 1;
    scene = ios_window_scene();
    if (!scene) {
        fprintf(stderr, "[ios] no window scene for the game import\n");
        return 0;
    }
    fprintf(stderr, "[ios] no default.xbe in %s: asking for the game folder\n", game_dir);
    vc = [NFSImportController new];
    vc.gameDir = game;
    win = [[UIWindow alloc] initWithWindowScene:scene];
    win.rootViewController = vc;
    [win makeKeyAndVisible];
    if (getenv("NFSU2_IMPORT_FROM"))
        [vc importFrom:[NSURL fileURLWithPath:@(getenv("NFSU2_IMPORT_FROM")) isDirectory:YES]];
    while (!vc.done)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    win.hidden = YES;
    return 1;
}

/* NFSU2_VERIFY=1: the SHA-1 and size of every disc file under the game
 * folder, in the log ("[verify] <sha1> <bytes> <path>"), to compare with the
 * copy the game came from (shasum on the Mac) -- a transfer that damaged the
 * data files. UDATA and TDATA are the title's own writes and are skipped. */
void ios_verify_game(const char *game_dir)
{
    @autoreleasepool {
        NSString *root = @(game_dir), *rel;
        NSDirectoryEnumerator<NSString *> *e = [NSFileManager.defaultManager enumeratorAtPath:root];
        CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        unsigned long long total = 0;
        unsigned n = 0, bad = 0;

        while ((rel = [e nextObject])) {
            NSDictionary *a = e.fileAttributes;
            NSString *sha;
            if ([a.fileType isEqualToString:NSFileTypeDirectory]) {
                if ([rel isEqualToString:@"UDATA"] || [rel isEqualToString:@"TDATA"])
                    [e skipDescendants];
                continue;
            }
            if (![a.fileType isEqualToString:NSFileTypeRegular])
                continue;
            sha = xbe_sha1([root stringByAppendingPathComponent:rel]);
            if (!sha)
                bad++;
            fprintf(stderr, "[verify] %s %llu %s\n", sha ? sha.UTF8String : "unreadable",
                    a.fileSize, rel.UTF8String);
            n++;
            total += a.fileSize;
        }
        fprintf(stderr, "[verify] %u files, %llu MB, %u unreadable, %.0f s\n", n, total >> 20, bad,
                CFAbsoluteTimeGetCurrent() - t0);
    }
}
