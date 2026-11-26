// SPDX-License-Identifier: MIT
// The cellular side's own account of itself, on the serial console.
//
// ABM (CommCenter's baseband manager) says why it does not drive a baseband only in the unified log, on a
// subsystem that never reaches the serial console. This reads that log through the same API the probe daemon
// uses for its export (OSLogStore in the system scope, which the logging entitlements allow) and prints just
// the telephony entries to stdout; the launchd job hands stdout to /dev/console, which is the line the
// captured boot log already carries. Filtered and bounded, because the console is a slow line.
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <unistd.h>
#include <objc/message.h>

static const char *levelName(NSInteger level)
{
    switch (level) {
    case 1: return "debug";
    case 2: return "info";
    case 3: return "notice";
    case 4: return "error";
    case 5: return "fault";
    default: return "-";
    }
}

static bool interesting(NSString *process, NSString *subsystem, NSString *category)
{
    for (NSString *text in @[process ?: @"", subsystem ?: @"", category ?: @""]) {
        if ([text localizedCaseInsensitiveContainsString:@"commcenter"]) return true;
        if ([text localizedCaseInsensitiveContainsString:@"telephony"]) return true;
        if ([text localizedCaseInsensitiveContainsString:@"baseband"]) return true;
        if ([text localizedCaseInsensitiveContainsString:@"abm"]) return true;
        if ([text localizedCaseInsensitiveContainsString:@"qmux"]) return true;
    }
    return false;
}



#pragma mark - the cellular bridge's firmware

/* The Mav16 archive the payload carries is a zip of the firmware images ABM hands to the baseband. This unpacks
 * it where the kernel's union view expects it (/private/var/ngpu/owner-apps/firmware/Baseband/Mav16). A small
 * reader is enough: stored and deflated entries, no zip64, no encryption -- what the .bbfw uses. */
#include <zlib.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <mach/mach.h>
/* The bootstrap lookups are not in the iPhoneOS public headers; libSystem exports both. */
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);
#include <signal.h>
#include <string.h>
#include <stdlib.h>

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool makeDirs(const char *path)
{
    char buffer[512];
    if (strlen(path) >= sizeof buffer) return false;
    strcpy(buffer, path);
    for (char *p = buffer + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buffer, 0755);
        *p = '/';
    }
    return true;
}

/* ABM reads the firmware from the preboot tree as well: <preboot>/<boot-manifest-hash>/usr/local/standalone/
 * firmware/Baseband/Mav16. The hash is the 96 hex characters of /chosen/boot-manifest-hash, which is the name of
 * the preboot directory; the files are hard links to the ones already unpacked (same volume, no second copy). */
static bool mirrorPreboot(const char *root)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *entries = [fm contentsOfDirectoryAtPath:@"/private/preboot" error:nil];
    NSString *hash = nil;
    for (NSString *name in entries) {
        if (name.length == 96) { hash = name; break; }
    }
    if (!hash) return false;
    char target[512];
    snprintf(target, sizeof target, "/private/var/ngpu/owner-apps/preboot/%s/usr/local/standalone/firmware/Baseband/Mav16",
             hash.UTF8String);
    makeDirs(target);
    NSArray<NSString *> *files = [fm contentsOfDirectoryAtPath:[NSString stringWithUTF8String:root] error:nil];
    for (NSString *name in files) {
        char from[512], to[512];
        snprintf(from, sizeof from, "%s/%s", root, name.UTF8String);
        snprintf(to, sizeof to, "%s/%s", target, name.UTF8String);
        unlink(to);
        if (link(from, to) != 0) {
            /* Different volume, or a hard link refused: copy the bytes instead. */
            FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
            if (!in || !out) { if (in) fclose(in); if (out) fclose(out); continue; }
            char block[65536];
            size_t got;
            while ((got = fread(block, 1, sizeof block, in)) > 0) fwrite(block, 1, got, out);
            fclose(in); fclose(out);
        }
    }
    return true;
}



static unsigned unpackFirmware(void)
{
    const char *source = "/private/var/ngpu/baseband/vinyl_02_Mav16-11.00.01.Release.bbfw";
    const char *root = "/private/var/ngpu/owner-apps/firmware/Baseband/Mav16";
    FILE *in = fopen(source, "rb");
    if (!in) return 0;
    fseek(in, 0, SEEK_END);
    const long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (size < 22 || size > (64 << 20)) { fclose(in); return 0; }
    uint8_t *zip = malloc((size_t)size);
    if (!zip) { fclose(in); return 0; }
    if (fread(zip, 1, (size_t)size, in) != (size_t)size) { free(zip); fclose(in); return 0; }
    fclose(in);
    /* The end-of-central-directory record: the last 22 bytes plus a comment of up to 64 KiB. */
    long eocd = -1;
    for (long at = size - 22; at >= 0 && at >= size - 22 - 65536; --at) {
        if (le32(zip + at) == 0x06054b50) { eocd = at; break; }
    }
    if (eocd < 0) { free(zip); return 0; }
    const unsigned count = le16(zip + eocd + 10);
    long at = (long)(uint32_t)le32(zip + eocd + 16);
    makeDirs(root);
    unsigned written = 0;
    for (unsigned i = 0; i < count && at + 46 <= size; ++i) {
        if (le32(zip + at) != 0x02014b50) break;
        const uint16_t method = le16(zip + at + 10);
        const uint32_t packed = le32(zip + at + 20), plain = le32(zip + at + 24);
        const uint16_t nameLength = le16(zip + at + 28), extraLength = le16(zip + at + 30), commentLength = le16(zip + at + 32);
        const uint32_t offset = le32(zip + at + 42);
        char name[256];
        if (nameLength >= sizeof name) { at += 46 + nameLength + extraLength + commentLength; continue; }
        memcpy(name, zip + at + 46, nameLength);
        name[nameLength] = 0;
        at += 46 + nameLength + extraLength + commentLength;
        if (name[nameLength - 1] == '/') continue;
        if (strstr(name, "..")) continue;
        char target[512];
        snprintf(target, sizeof target, "%s/%s", root, name);
        if (!makeDirs(target)) continue;
        if ((long)offset + 30 > size || le32(zip + offset) != 0x04034b50) continue;
        const uint16_t localName = le16(zip + offset + 26), localExtra = le16(zip + offset + 28);
        const uint8_t *body = zip + offset + 30 + localName + localExtra;
        if ((long)(body - zip) + (long)packed > size) continue;
        uint8_t *out = malloc(plain ? plain : 1);
        if (!out) continue;
        bool ok = false;
        if (method == 0 && packed == plain) { memcpy(out, body, plain); ok = true; }
        else if (method == 8) {
            z_stream stream = {0};
            if (inflateInit2(&stream, -15) == Z_OK) {
                stream.next_in = (Bytef *)body; stream.avail_in = packed;
                stream.next_out = out; stream.avail_out = plain;
                ok = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.total_out == plain;
                inflateEnd(&stream);
            }
        }
        if (ok) {
            FILE *file = fopen(target, "wb");
            if (file && fwrite(out, 1, plain, file) == plain) ++written;
            if (file) fclose(file);
        }
        free(out);
    }
    free(zip);
    return written;
}


static NSString *mgKey(NSString *name)
{
    /* MobileGestalt stores its answers under base64(MD5("MGCopyAnswer" + key))[:22] (tools/xnu_screen.py uses
       the same derivation for ProductType). */
    NSString *full = [@"MGCopyAnswer" stringByAppendingString:name];
    unsigned char digest[CC_MD5_DIGEST_LENGTH];
    CC_MD5(full.UTF8String, (CC_LONG)strlen(full.UTF8String), digest);
    NSString *encoded = [[NSData dataWithBytes:digest length:sizeof digest] base64EncodedStringWithOptions:0];
    return encoded.length >= 22 ? [encoded substringToIndex:22] : encoded;
}

static void probeSystem(FILE *file)
{
    /* The three questions the ABM investigation came down to: what MobileGestalt answers (the radio module
       gate), which of the two CommCenter jobs the launchd manifest carries, and which CommCenter binary is in
       effect under CoreTelephony. */
    NSString *cachePath = @"/private/var/containers/Shared/SystemGroup/"
        "systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist";
    NSData *cache = [NSData dataWithContentsOfFile:cachePath];
    fprintf(file, "# MG cache %s (%lu bytes)\n", cache ? "present" : "absent", (unsigned long)cache.length);
    NSArray *keys = @[@"ProductType", @"HasBaseband", @"HWModelStr", @"IsVirtualDevice"];
    if (cache) {
        id plist = [NSPropertyListSerialization propertyListWithData:cache options:NSPropertyListImmutable format:NULL error:NULL];
        NSDictionary *extra = [plist isKindOfClass:[NSDictionary class]] ? [plist objectForKey:@"CacheExtra"] : nil;
        for (NSString *name in keys) {
            NSString *key = mgKey(name);
            id value = [extra isKindOfClass:[NSDictionary class]] ? [extra objectForKey:key] : nil;
            fprintf(file, "# MG %-14s %s (key %s)\n", name.UTF8String,
                    value ? [[value description] UTF8String] : "not cached", key.UTF8String);
            printf("FUXILOG: MG %s = %s\n", name.UTF8String, value ? [[value description] UTF8String] : "not cached");
        }
    }
    NSData *manifestData = [NSData dataWithContentsOfFile:@"/System/Library/xpc/launchd.plist"];
    id manifestPlist = manifestData ? [NSPropertyListSerialization propertyListWithData:manifestData options:NSPropertyListImmutable format:NULL error:NULL] : nil;
    NSDictionary *manifest = [manifestPlist isKindOfClass:[NSDictionary class]] ? manifestPlist : nil;
    fprintf(file, "# manifest %s (%lu bytes)\n", manifest ? "loaded" : "missing", (unsigned long)manifestData.length);
    for (NSString *job in @[@"/System/Library/LaunchDaemons/com.apple.CommCenter.plist",
                            @"/System/Library/LaunchDaemons/com.apple.CommCenter-noncellular.plist"]) {
        NSDictionary *entry = [manifest objectForKey:job];
        fprintf(file, "# job %s: %s\n", job.lastPathComponent.UTF8String, entry ? "present" : "absent");
        if (entry) {
            fprintf(file, "#   limit-to-device-tree %s\n", [[entry objectForKey:@"_LimitLoadToDeviceTree"] description].UTF8String);
            fprintf(file, "#   limit-from-device-tree %s\n", [[entry objectForKey:@"_LimitLoadFromDeviceTree"] description].UTF8String);
            fprintf(file, "#   program %s\n", [[entry objectForKey:@"ProgramArguments"] description].UTF8String);
            fprintf(file, "#   mach %s\n", [[[entry objectForKey:@"MachServices"] allKeys] description].UTF8String);
        }
    }
    for (NSString *path in @[@"/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter",
                             @"/private/var/ngpu/baseband/CommCenter"]) {
        NSData *data = [NSData dataWithContentsOfFile:path];
        unsigned char digest[CC_SHA256_DIGEST_LENGTH] = {0};
        if (data) CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
        fprintf(file, "# file %s: %s %lu bytes sha %02x%02x%02x%02x%02x%02x%02x%02x\n", path.UTF8String,
                data ? "present" : "absent", (unsigned long)data.length,
                digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7]);
        printf("FUXILOG: %s %s\n", path.UTF8String, data ? "present" : "absent");
    }
    fflush(file);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        /* The console may not be the line this port captures, so the same text also goes to a file the kernel
           reads back and prints: an entry there proves the job ran even when the console path is silent. */
        const unsigned firmware = unpackFirmware();
        static const char firmwareRoot[] = "/private/var/ngpu/owner-apps/firmware/Baseband/Mav16";
        const bool mirrored = firmware ? mirrorPreboot(firmwareRoot) : false;
        FILE *file = fopen("/private/var/db/fuxi-logdump.txt", "w");
        printf("FUXILOG: reading the unified log for the telephony side\n");
        if (file) fprintf(file, "FUXILOG: the job ran, %u firmware files unpacked, preboot mirror %d\n", firmware, mirrored);
        /* Which of the two CommCenter jobs launchd loaded, and whether CommCenter brought its baseband manager up:
           com.apple.basebandd.xpc is the service ABM lives behind, carrierspace exists only on the cellular job. */
        if (file) {
            static const char *const probes[] = {
                "com.apple.basebandd.xpc", "com.apple.commcenter.xpc",
                "com.apple.commcenter.carrierspace.xpc", "abmtool",
            };
            for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; ++i) {
                mach_port_t port = MACH_PORT_NULL;
                const kern_return_t found = bootstrap_look_up(bootstrap_port, probes[i], &port);
                fprintf(file, "# %s %s\n", probes[i], found == KERN_SUCCESS ? "REGISTERED" : "absent");
                if (found == KERN_SUCCESS) mach_port_deallocate(mach_task_self(), port);
            }
            fflush(file);
        }
        probeSystem(file);
        /* The real launch chain is demand driven: CommCenter connects to com.apple.abm.helper.wireless, launchd
           starts abm-helper for it, and abm-helper then asks com.apple.abm.helper.root, which starts
           CommCenterRootHelper. Looking the two names up is what launchd reacts to, so the daemon holds the
           ports for the rest of its run instead of spawning anything itself. */
        /* launchd runs the ABM helper as _wireless: if this Data has no such user the job cannot spawn at all,
           and the missing log file is the only evidence. */
        {
            static const char *const users[] = {"_wireless", "_assetcache", "mobile", "root"};
            for (unsigned i = 0; i < sizeof users / sizeof users[0]; ++i) {
                struct passwd *entry = getpwnam(users[i]);
                if (file) fprintf(file, "# user %-12s %s\n", users[i], entry ? "present" : "MISSING");
                printf("FUXILOG: user %s %s\n", users[i], entry ? "present" : "MISSING");
            }
            if (file) fflush(file);
        }
        /* launchd will not spawn abm-helper here (its log file is never even created), so run it the way launchd
           would: as _wireless, with its output captured, and its exit status recorded. That separates "the image
           cannot execute it" from "it runs and decides there is nothing to serve". */
        {
            const pid_t child = fork();
            if (child == 0) {
                (void)setgid(25);
                (void)setuid(25);
                const int fd = open("/private/var/db/abm-helper.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { (void)dup2(fd, 1); (void)dup2(fd, 2); }
                execl("/System/Library/PrivateFrameworks/ABMHelper.framework/Support/abm-helper", "abm-helper", (char *)NULL);
                _exit(127);
            }
            int status = 0;
            bool reaped = false;
            for (int i = 0; child > 0 && i < 60; ++i) {
                if (waitpid(child, &status, WNOHANG) == child) { reaped = true; break; }
                usleep(500000);
            }
            if (child > 0 && !reaped) { (void)kill(child, SIGKILL); (void)waitpid(child, &status, 0); }
            if (file) fprintf(file, "# abm-helper %s: exit %d signal %d\n", reaped ? "exited" : "killed after 30 s",
                              WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            printf("FUXILOG: abm-helper %s exit %d\n", reaped ? "exited" : "killed", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            if (file) fflush(file);
        }
        /* _wireless's home: launchd may refuse to spawn the ABM helper for a user whose home is missing, and on
           this port /private/var is the Data volume, not the sealed root that carries the stock directory. */
        {
            const int made = mkdir("/private/var/wireless", 0755);
            const int owned = (made == 0 || errno == EEXIST) ? chown("/private/var/wireless", 25, 25) : -1;
            if (file) fprintf(file, "# /private/var/wireless mkdir %d chown %d\n", made, owned);
            printf("FUXILOG: /private/var/wireless mkdir %d chown %d\n", made, owned);
            if (file) fflush(file);
        }
        static mach_port_t held[2];
        static const char *const demand[] = {"com.apple.abm.helper.wireless", "com.apple.abm.helper.root"};
        for (unsigned i = 0; i < 2; ++i) {
            const kern_return_t found = bootstrap_look_up(bootstrap_port, demand[i], &held[i]);
            if (file) fprintf(file, "# demand %s %s\n", demand[i], found == KERN_SUCCESS ? "started" : "absent");
            printf("FUXILOG: %s %s\n", demand[i], found == KERN_SUCCESS ? "started" : "absent");
        }
        if (file) fflush(file);
        /* CommCenter starts around 20 s and ABM has made its decision well before a minute; a minute and a
           half leaves the retries behind without spending the console too early. */
        sleep(15);
        Class storeClass = NSClassFromString(@"OSLogStore");
        if (!storeClass) {
            printf("FUXILOG: OSLogStore is not in this userspace\n");
            return 1;
        }
        NSError *error = nil;
        id store = ((id (*)(Class, SEL, NSInteger, NSError **))objc_msgSend)(
            storeClass, @selector(storeWithScope:error:), 0, &error);
        if (!store) {
            printf("FUXILOG: no store: %s\n", (error ? error.description : @"nil").UTF8String);
            return 2;
        }
        /* Only the recent window: the whole-boot enumeration takes minutes and the console read it too early. */
        id position = nil;
        const BOOL windowed = [store respondsToSelector:@selector(positionWithTimeIntervalSinceNow:)];
        if (windowed) {
            position = ((id (*)(id, SEL, NSTimeInterval))objc_msgSend)(store, @selector(positionWithTimeIntervalSinceNow:), -120.0);
        }
        if (!position) {
            position = ((id (*)(id, SEL, NSTimeInterval))objc_msgSend)(store, @selector(positionWithTimeIntervalSinceLatestBoot:), 0);
        }
        printf("FUXILOG: the window is %s\n", windowed ? "the last 120 seconds" : "the whole boot");
        id entries = ((id (*)(id, SEL, NSUInteger, id, id, NSError **))objc_msgSend)(
            store, @selector(entriesEnumeratorWithOptions:position:predicate:error:), 0, position, nil, &error);
        if (!entries) {
            printf("FUXILOG: no entries: %s\n", (error ? error.description : @"nil").UTF8String);
            return 3;
        }
        NSDate *start = nil;
        unsigned seen = 0, printed = 0;
        for (id entry in entries) {
            @autoreleasepool {
                NSString *process = [entry respondsToSelector:@selector(process)] ? [entry valueForKey:@"process"] : @"";
                NSString *subsystem = [entry respondsToSelector:@selector(subsystem)] ? [entry valueForKey:@"subsystem"] : @"";
                NSString *category = [entry respondsToSelector:@selector(category)] ? [entry valueForKey:@"category"] : @"";
                if (!interesting(process, subsystem, category)) continue;
                ++seen;
                /* 600 lines of console is about what the capture keeps clean; the rest is counted below. */
                if (printed >= 600) continue;
                const NSInteger level = [entry respondsToSelector:@selector(level)] ? [[entry valueForKey:@"level"] integerValue] : 0;
                NSDate *date = [entry valueForKey:@"date"];
                if (!start) start = date;
                const double at = [date timeIntervalSinceDate:start];
                NSString *message = [entry valueForKey:@"composedMessage"];
                if (message.length > 300) message = [message substringToIndex:300];
                printf("FUXILOG %8.3f %s %s:%s %s %s\n", at, process.UTF8String ?: "?",
                       subsystem.UTF8String ?: "", category.UTF8String ?: "", levelName(level),
                       message.UTF8String ?: "");
                if (file) fprintf(file, "%8.3f %s %s:%s %s %s\n", at, process.UTF8String ?: "?",
                       subsystem.UTF8String ?: "", category.UTF8String ?: "", levelName(level),
                       message.UTF8String ?: "");
                ++printed;
            }
        }
        printf("FUXILOG: %u telephony entries, %u printed\n", seen, printed);
        if (file) {
            fprintf(file, "FUXILOG: %u telephony entries, %u printed\n", seen, printed);
            fclose(file);
        }
    }
    return 0;
}
