#import "AppDelegate.h"
#import "EmulatorRunner.h"
#import "armsx_bridge.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static NSString *const ARMSXLibraryBookmarksKey = @"ARMSXLibraryDirectoryBookmarks";

@interface AppDelegate () <UIDocumentPickerDelegate>
@property (nonatomic, strong) EmulatorRunner *emulatorRunner;
@property (nonatomic, copy) NSString *launchProtocolURL;
@property (nonatomic, assign) BOOL pendingLibraryRecursive;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSURL *> *accessedLibraryURLs;
- (void)presentLibraryDirectoryPickerRecursive:(BOOL)recursive;
- (void)removePersistedLibraryDirectory:(NSString *)path;
- (NSURL *)prepareSharedDocumentsDirectory;
- (void)configureUnifiedLoggingAtDocumentsURL:(NSURL *)documentsURL;
@end

static void ARMSXRequestLibraryDirectory(int recursive, void *userdata) {
    AppDelegate *delegate = (__bridge AppDelegate *)userdata;
    void (^present)(void) = ^{
        [delegate presentLibraryDirectoryPickerRecursive:recursive != 0];
    };
    if (NSThread.isMainThread) {
        present();
    } else {
        dispatch_async(dispatch_get_main_queue(), present);
    }
}

static void ARMSXRemoveLibraryDirectory(const char *path, void *userdata) {
    if (!path) {
        return;
    }
    AppDelegate *delegate = (__bridge AppDelegate *)userdata;
    NSString *directoryPath = [NSString stringWithUTF8String:path];
    void (^remove)(void) = ^{
        [delegate removePersistedLibraryDirectory:directoryPath];
    };
    if (NSThread.isMainThread) {
        remove();
    } else {
        dispatch_async(dispatch_get_main_queue(), remove);
    }
}

@implementation AppDelegate

- (UIViewController *)topViewController {
    UIViewController *controller = self.window.rootViewController;
    while (controller.presentedViewController) {
        controller = controller.presentedViewController;
    }
    return controller;
}

- (void)presentLibraryDirectoryPickerRecursive:(BOOL)recursive {
    self.pendingLibraryRecursive = recursive;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[UTTypeFolder]
        asCopy:NO];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;

    UIViewController *presenter = [self topViewController];
    if (!presenter) {
        NSLog(@"ARMSX could not present the directory picker because no root view controller is active.");
        return;
    }
    [presenter presentViewController:picker animated:YES completion:nil];
}

- (void)registerLibraryURL:(NSURL *)url recursive:(BOOL)recursive persist:(BOOL)persist {
    if (!url.isFileURL || url.path.length == 0) {
        return;
    }

    NSString *path = url.URLByStandardizingPath.path;
    BOOL accessed = [url startAccessingSecurityScopedResource];
    if (accessed) {
        self.accessedLibraryURLs[path] = url;
    }

    NSString *label = nil;
    [url getResourceValue:&label forKey:NSURLNameKey error:nil];
    if (label.length == 0) {
        label = url.lastPathComponent.length ? url.lastPathComponent : @"Game Directory";
    }
    psxe_register_platform_library_directory(path.UTF8String, label.UTF8String, recursive ? 1 : 0);

    if (!persist) {
        return;
    }

    NSError *bookmarkError = nil;
    NSData *bookmark = [url bookmarkDataWithOptions:0
                         includingResourceValuesForKeys:@[NSURLNameKey]
                                          relativeToURL:nil
                                                  error:&bookmarkError];
    if (!bookmark) {
        NSLog(@"ARMSX could not save persistent directory access for %@: %@", path, bookmarkError);
        return;
    }

    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSMutableArray<NSDictionary *> *records =
        [[defaults arrayForKey:ARMSXLibraryBookmarksKey] mutableCopy] ?: [NSMutableArray array];
    NSIndexSet *duplicates = [records indexesOfObjectsPassingTest:
        ^BOOL(NSDictionary *record, NSUInteger index, BOOL *stop) {
            (void)index;
            (void)stop;
            return [record[@"path"] isEqualToString:path];
        }];
    [records removeObjectsAtIndexes:duplicates];
    [records addObject:@{
        @"bookmark": bookmark,
        @"path": path,
        @"label": label,
        @"recursive": @(recursive),
    }];
    [defaults setObject:records forKey:ARMSXLibraryBookmarksKey];
}

- (void)restoreLibraryDirectories {
    NSArray<NSDictionary *> *records =
        [NSUserDefaults.standardUserDefaults arrayForKey:ARMSXLibraryBookmarksKey] ?: @[];
    NSMutableArray<NSDictionary *> *validRecords = [NSMutableArray array];

    for (NSDictionary *record in records) {
        NSData *bookmark = record[@"bookmark"];
        if (![bookmark isKindOfClass:NSData.class]) {
            continue;
        }

        BOOL stale = NO;
        NSError *error = nil;
        NSURL *url = [NSURL URLByResolvingBookmarkData:bookmark
                                              options:0
                                        relativeToURL:nil
                                  bookmarkDataIsStale:&stale
                                                error:&error];
        if (!url || error) {
            NSLog(@"ARMSX could not restore a game directory bookmark: %@", error);
            continue;
        }

        BOOL recursive = [record[@"recursive"] boolValue];
        [self registerLibraryURL:url recursive:recursive persist:NO];

        if (stale) {
            NSError *bookmarkError = nil;
            NSData *refreshed = [url bookmarkDataWithOptions:0
                                  includingResourceValuesForKeys:@[NSURLNameKey]
                                                   relativeToURL:nil
                                                           error:&bookmarkError];
            if (refreshed) {
                NSMutableDictionary *updated = [record mutableCopy];
                updated[@"bookmark"] = refreshed;
                updated[@"path"] = url.URLByStandardizingPath.path;
                [validRecords addObject:updated];
                continue;
            }
            NSLog(@"ARMSX could not refresh a stale game directory bookmark: %@", bookmarkError);
        }
        [validRecords addObject:record];
    }

    [NSUserDefaults.standardUserDefaults setObject:validRecords forKey:ARMSXLibraryBookmarksKey];
}

- (void)removePersistedLibraryDirectory:(NSString *)path {
    NSString *standardPath = [NSURL fileURLWithPath:path].URLByStandardizingPath.path;
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSMutableArray<NSDictionary *> *records =
        [[defaults arrayForKey:ARMSXLibraryBookmarksKey] mutableCopy] ?: [NSMutableArray array];
    NSIndexSet *matches = [records indexesOfObjectsPassingTest:
        ^BOOL(NSDictionary *record, NSUInteger index, BOOL *stop) {
            (void)index;
            (void)stop;
            NSString *recordPath = record[@"path"];
            return [recordPath isEqualToString:standardPath];
        }];
    [records removeObjectsAtIndexes:matches];
    [defaults setObject:records forKey:ARMSXLibraryBookmarksKey];

    NSURL *url = self.accessedLibraryURLs[standardPath];
    if (url) {
        [url stopAccessingSecurityScopedResource];
        [self.accessedLibraryURLs removeObjectForKey:standardPath];
    }
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *url = urls.firstObject;
    if (url) {
        [self registerLibraryURL:url recursive:self.pendingLibraryRecursive persist:YES];
    }
}

- (void)forceLandscapeIfNeeded {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIInterfaceOrientation currentOrientation = UIInterfaceOrientationUnknown;
        if (@available(iOS 13.0, *)) {
            currentOrientation = self.window.windowScene.interfaceOrientation;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            currentOrientation = UIApplication.sharedApplication.statusBarOrientation;
#pragma clang diagnostic pop
        }

        if (UIInterfaceOrientationIsLandscape(currentOrientation)) {
            return;
        }

        NSNumber *value = @(UIInterfaceOrientationLandscapeRight);
        [[UIDevice currentDevice] setValue:value forKey:@"orientation"];
        [UIViewController attemptRotationToDeviceOrientation];
    });
}

- (NSURL *)prepareSharedDocumentsDirectory {
    NSFileManager *fileManager = NSFileManager.defaultManager;
    NSURL *documentsURL = [[fileManager URLsForDirectory:NSDocumentDirectory
                                               inDomains:NSUserDomainMask] firstObject];
    if (!documentsURL) {
        NSLog(@"ARMSX could not locate its Documents directory.");
        return nil;
    }

    NSError *error = nil;
    if (![fileManager createDirectoryAtURL:documentsURL
               withIntermediateDirectories:YES
                                attributes:nil
                                     error:&error]) {
        NSLog(@"ARMSX could not create its Documents directory: %@", error);
        return nil;
    }

    for (NSString *directoryName in @[@"Games", @"BIOS", @"Saves", @"Screenshots"]) {
        NSURL *directoryURL = [documentsURL URLByAppendingPathComponent:directoryName isDirectory:YES];
        error = nil;
        if (![fileManager createDirectoryAtURL:directoryURL
                   withIntermediateDirectories:YES
                                    attributes:nil
                                         error:&error]) {
            NSLog(@"ARMSX could not create shared directory %@: %@", directoryName, error);
        }
    }

    setenv("ARMSX_DOCUMENTS_DIR", documentsURL.fileSystemRepresentation, 1);
    return documentsURL;
}

- (void)configureUnifiedLoggingAtDocumentsURL:(NSURL *)documentsURL {
    if (!documentsURL) {
        return;
    }

    NSURL *logURL = [documentsURL URLByAppendingPathComponent:@"armsx_log.txt" isDirectory:NO];
    const char *logPath = logURL.fileSystemRepresentation;
    int logFD = open(logPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (logFD < 0) {
        NSLog(@"ARMSX could not open its unified log at %@: %s", logURL.path, strerror(errno));
        return;
    }

    const int stderrResult = dup2(logFD, STDERR_FILENO);
    const int stdoutResult = dup2(logFD, STDOUT_FILENO);
    if (logFD != STDERR_FILENO && logFD != STDOUT_FILENO) {
        close(logFD);
    }

    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("ARMSX_DIAGNOSTICS_LOG_PATH", logPath, 1);

    fprintf(
        stderr,
        "@@LOG_SINK@@ unified=1 path=%s pid=%d stdout=%d stderr=%d\n",
        logPath,
        getpid(),
        stdoutResult >= 0 ? 1 : 0,
        stderrResult >= 0 ? 1 : 0
    );
    NSString *bundleIdentifier = NSBundle.mainBundle.bundleIdentifier;
    NSString *bundleVersion = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"];
    fprintf(
        stderr,
        "@@BUILD@@ bundle=%s version=%s\n",
        bundleIdentifier.length ? bundleIdentifier.UTF8String : "(unknown)",
        bundleVersion.length ? bundleVersion.UTF8String : "(unknown)"
    );
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor blackColor];
    UIViewController *rootViewController = [UIViewController new];
    rootViewController.view.backgroundColor = [UIColor blackColor];
    self.window.rootViewController = rootViewController;
    [self.window makeKeyAndVisible];
    [self forceLandscapeIfNeeded];
    self.accessedLibraryURLs = [NSMutableDictionary dictionary];

    NSURL *documentsURL = [self prepareSharedDocumentsDirectory];
    [self configureUnifiedLoggingAtDocumentsURL:documentsURL];

    psxe_set_library_directory_picker_callback(
        ARMSXRequestLibraryDirectory,
        (__bridge void *)self
    );
    psxe_set_library_directory_removed_callback(
        ARMSXRemoveLibraryDirectory,
        (__bridge void *)self
    );
    [self restoreLibraryDirectories];

    NSURL *launchURL = launchOptions[UIApplicationLaunchOptionsURLKey];
    if (launchURL.absoluteString.length) {
        self.launchProtocolURL = launchURL.absoluteString;
    }

    self.emulatorRunner = [EmulatorRunner new];
    [self.emulatorRunner startWithArgs:self.launchProtocolURL.length ? @[self.launchProtocolURL] : @[]];

    return YES;
}

- (void)applicationWillTerminate:(UIApplication *)application {
    (void)application;
    for (NSURL *url in self.accessedLibraryURLs.allValues) {
        [url stopAccessingSecurityScopedResource];
    }
    [self.accessedLibraryURLs removeAllObjects];
    psxe_set_library_directory_picker_callback(NULL, NULL);
    psxe_set_library_directory_removed_callback(NULL, NULL);
}

- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url options:(NSDictionary<UIApplicationOpenURLOptionsKey, id> *)options {
    (void)application;
    (void)options;

    NSString *argument = url.absoluteString;
    if (argument.length == 0) {
        return NO;
    }

    if (self.launchProtocolURL.length && [self.launchProtocolURL isEqualToString:argument]) {
        self.launchProtocolURL = nil;
        return YES;
    }

    if (!self.emulatorRunner) {
        self.emulatorRunner = [EmulatorRunner new];
        [self.emulatorRunner startWithArgs:@[argument]];
    } else {
        [self.emulatorRunner enqueueLaunchArgument:argument];
    }

    return YES;
}

@end
