// SPDX-License-Identifier: MIT
// Explicit temporary bring-up policy for an unavailable watchdog backend.
// Runs after writable scratch Data mounts, before the unchanged launchd exec.
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <pexpert/pexpert.h>

static const char policy_head[] =
 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
 "<plist version=\"1.0\"><dict>";
static const char policy_watchdogd[]="<key>com.apple.watchdogd</key><true/>";
// Bring-up isolation: hardware stacks show identityservicesd, contextstored and locationd in a
// cycle of synchronous XPC waits. A disabled job makes its Mach service lookups fail instead of hang.
// sheng-disable-jobs=<label>[,<label>...] (restricted label characters, bounded length).
static const char policy_tail[]="</dict></plist>\n";
// sheng-lite=1: jobs that only reach for the network, iCloud or the photo library and have nothing to do on this tablet (no radios, no accounts),
// all of which start at the first boot of a fresh Data and compete with SpringBoard for the CPU and for memory. The first four are the
// cycle of synchronous XPC waits the isolation work found. Labels are the ones in the target's own LaunchDaemons.
static const char lite_jobs[]=
 "com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd,"
 "com.apple.photoanalysisd,com.apple.mediaanalysisd,com.apple.mediastream.mstreamd,com.apple.icloud.searchpartyd,"
 "com.apple.findmy.findmybeaconingd,com.apple.familycircled,com.apple.financed,com.apple.softwareupdateservicesd,"
 "com.apple.ndoagent,com.apple.biomesyncd,com.apple.email.maild,com.apple.amsengagementd,com.apple.adid,com.apple.eyereliefd,"
 // Crash or respawn loops seen on hardware (int-176): fairplayd.A2 is KeepAlive and exits at once (22 launches in 6 minutes), passd crashes,
 // obliteration is a data-wipe daemon with nothing to do here. iconservicesagent also dies in CI::GLContext::init, but disabling it leaves
 // the icons missing (seen on hardware), so it stays enabled.
 "com.apple.fairplayd.A2,com.apple.passd,com.apple.mobile.obliteration";
static char policy[2048],jobs[200];
volatile int sheng_watchdog_policy_status=-1;

// sheng-setup=1 or sheng-data-reset=2 (the install entry's "show Hello" choice): remove Setup's completion flags once, so this boot shows the Hello page.
// iPadOS writes them again when Setup finishes, so later boots do not show it. Absent files are fine.
static int sheng_boot_setup_reset(proc_t p,uint64_t scratch) {
 static const char *const files[]={
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist",
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist"};
 for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i) {
  int32_t result=0;
  int error=copyout(files[i],scratch,strlen(files[i])+1);
  if(error)return error;
  struct unlink_args rm={.path=scratch};
  error=unlink(p,&rm,&result);
  if(error && error!=ENOENT)return error;
 }
 return 0;
}

// One comma-separated label at *c into the policy as a disabled key; advances *c past it.
static int append_job(const char **cursor) {
 const char *c=*cursor,*e=c;while(*e && *e!=',')++e;
 if(e>c) {
  if(strlcat(policy,"<key>",sizeof(policy))>=sizeof(policy))return EINVAL;
  size_t at=strlen(policy);if(at+(size_t)(e-c)+1>=sizeof(policy))return EINVAL;
  memcpy(policy+at,c,(size_t)(e-c));policy[at+(size_t)(e-c)]=0;
  if(strlcat(policy,"</key><true/>",sizeof(policy))>=sizeof(policy))return EINVAL;
 }
 *cursor=*e?e+1:e;return 0;
}



/* ---- the cellular bridge's firmware, unpacked before the union that shows it ---- */
/* The payload carries Apple's Mav16 firmware package (a zip inside the GPU addon blob). ABM reads the images
 * from /usr/standalone/firmware/Baseband/Mav16 through the union this port mounts there, but a bindfs union made
 * before the files exist does not show them afterwards, so they are written here, before the mount: the same
 * order the reference implementation uses (its archive is plain data, this one is a zip, so the kernel's zlib
 * inflates it). */
#include <libkern/zlib.h>
extern int native_ram_block_gpu_addon(const uint8_t **, uint64_t *) __attribute__((weak_import));

static voidpf fw_zalloc(voidpf opaque __unused, uInt items, uInt size)
{
    size_t bytes = (size_t)items * size + 16;
    uint8_t *block = kalloc_data(bytes, Z_WAITOK | Z_ZERO);
    if (!block) return Z_NULL;
    memcpy(block, &bytes, sizeof bytes);
    return block + 16;
}

static void fw_zfree(voidpf opaque __unused, voidpf pointer)
{
    if (!pointer) return;
    uint8_t *block = (uint8_t *)pointer - 16;
    size_t bytes = 0;
    memcpy(&bytes, block, sizeof bytes);
    kfree_data(block, bytes);
}

static uint32_t fw_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t fw_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int fw_mkdirs(proc_t p, user_addr_t scratch, const char *path)
{
    char buffer[256];
    if (strlen(path) >= sizeof buffer) return ENAMETOOLONG;
    strlcpy(buffer, path, sizeof buffer);
    for (char *at = buffer + 1;; ++at) {
        const bool last = *at == 0;
        if (!last && *at != '/') continue;
        const char saved = *at;
        *at = 0;
        int32_t result = 0;
        int error = copyout(buffer, scratch, strlen(buffer) + 1);
        if (!error) {
            struct mkdir_args md = {.path = scratch, .mode = 0755};
            error = mkdir(p, &md, &result);
        }
        /* EROFS on a directory that already exists (the sealed root's own, like /private): the write
         * permission of the parent is checked before the name is. The final open() reports a real absence. */
        if (error && error != EEXIST && error != EROFS) {
            printf("sheng baseband firmware: mkdir %s says %d\n", buffer, error);
            *at = saved;
            return error;
        }
        *at = saved;
        if (last) return 0;
    }
}

static int fw_write(proc_t p, user_addr_t scratch, const char *path, const uint8_t *data, size_t size)
{
    if (copyout(path, scratch, strlen(path) + 1)) return EFAULT;
    int32_t result = 0;
    /* An earlier attempt may have made the name a directory: take it back so the file can be written. */
    {
        struct rmdir_args rd = {.path = scratch};
        (void)rmdir(p, &rd, &result);
        if (copyout(path, scratch, strlen(path) + 1)) return EFAULT;
    }
    struct open_args op = {.path = scratch, .flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, .mode = 0644};
    int error = open(p, &op, &result);
    if (error) return error;
    const int fd = result;
    for (size_t sent = 0; sent < size;) {
        const size_t piece = size - sent < 0x1f0 ? size - sent : 0x1f0;
        if (!(error = copyout(data + sent, scratch + 0x200, piece))) {
            struct write_args wr = {.fd = fd, .cbuf = scratch + 0x200, .nbyte = piece};
            user_ssize_t written = 0;
            if ((error = write(p, &wr, &written))) break;
            if (written != (user_ssize_t)piece) { error = EIO; break; }
        }
        if (error) break;
        sent += piece;
    }
    struct close_args cl = {.fd = fd};
    const int closed = sys_close(p, &cl, &result);
    if (!error && closed) error = closed;
    if (!error) {
        /* ABM reads the images as root; the file is written through the kernel's own credentials. */
    }
    return error;
}

static unsigned fw_unpack_zip(proc_t p, user_addr_t scratch, const uint8_t *zip, size_t size, const char *root)
{
    if (size < 22) return 0;
    long eocd = -1;
    for (long at = (long)size - 22; at >= 0 && at >= (long)size - 65558; --at) {
        if (fw_le32(zip + at) == 0x06054b50) { eocd = at; break; }
    }
    if (eocd < 0) {
        printf("sheng baseband firmware: no zip end record in %llu bytes, head %02x %02x %02x %02x, tail %02x %02x %02x %02x\n",
               (unsigned long long)size, zip[0], zip[1], zip[2], zip[3], zip[size-4], zip[size-3], zip[size-2], zip[size-1]);
        return 0;
    }
    const unsigned count = fw_le16(zip + eocd + 10);
    printf("sheng baseband firmware: zip %llu bytes, eocd at %ld, %u entries\n", (unsigned long long)size, eocd, count);
    long at = (long)(uint32_t)fw_le32(zip + eocd + 16);
    unsigned written = 0;
    for (unsigned i = 0; i < count && at >= 0 && (size_t)(at + 46) <= size; ++i) {
        if (fw_le32(zip + at) != 0x02014b50) break;
        const uint16_t method = fw_le16(zip + at + 10);
        const uint32_t packed = fw_le32(zip + at + 20), plain = fw_le32(zip + at + 24);
        const uint16_t nameLength = fw_le16(zip + at + 28), extraLength = fw_le16(zip + at + 30);
        const uint16_t commentLength = fw_le16(zip + at + 32);
        const uint32_t offset = fw_le32(zip + at + 42);
        char name[160];
        if (nameLength >= sizeof name) { at += 46 + nameLength + extraLength + commentLength; continue; }
        memcpy(name, zip + at + 46, nameLength);
        name[nameLength] = 0;
        at += 46 + nameLength + extraLength + commentLength;
        if (name[nameLength - 1] == '/') continue;
        if (strnstr(name, "..", nameLength)) continue;
        char target[256], parent[256];
        snprintf(target, sizeof target, "%s/%s", root, name);
        strlcpy(parent, target, sizeof parent);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; if (fw_mkdirs(p, scratch, parent)) continue; }
        if ((size_t)offset + 30 > size || fw_le32(zip + offset) != 0x04034b50) continue;
        const uint16_t localName = fw_le16(zip + offset + 26), localExtra = fw_le16(zip + offset + 28);
        const uint8_t *body = zip + offset + 30 + localName + localExtra;
        if ((size_t)(body - zip) + packed > size) continue;
        uint8_t *out = kalloc_data(plain ? plain : 1, Z_WAITOK);
        if (!out) continue;
        bool ok = false;
        if (method == 0 && packed == plain) { memcpy(out, body, plain); ok = true; }
        else if (method == 8) {
            z_stream stream = {0};
            stream.zalloc = fw_zalloc;
            stream.zfree = fw_zfree;
            if (inflateInit2(&stream, -15) == Z_OK) {
                stream.next_in = (Bytef *)body; stream.avail_in = packed;
                stream.next_out = out; stream.avail_out = plain;
                ok = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.total_out == plain;
                inflateEnd(&stream);
            }
        }
        if (ok) {
            const int storeError = fw_write(p, scratch, target, out, plain);
            if (storeError) printf("sheng baseband firmware: %s refused %d\n", name, storeError);
            else ++written;
        } else {
            printf("sheng baseband firmware: %s could not inflate (method %u, %u bytes)\n", name, method, plain);
        }
        kfree_data(out, plain ? plain : 1);
    }
    return written;
}

/* Which parts of the Data volume accept a directory at this point: the reference implementation writes its
 * firmware here before launchd, so if any of these is refused the difference is the mount, not the timing. */
static void fw_probe(proc_t p, user_addr_t scratch)
{
    static const char *const paths[] = {"/private/var/coachz-probe", "/private/var/coachz",
                                        "/private/var/coachz/baseband", "/private/var/coachz/baseband/standalone"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int32_t result = 0;
        int error = copyout(paths[i], scratch, strlen(paths[i]) + 1);
        if (!error) {
            struct mkdir_args md = {.path = scratch, .mode = 0755};
            error = mkdir(p, &md, &result);
        }
        printf("sheng baseband firmware: mkdir %s -> %d\n", paths[i], error);
    }
}

/* MobileGestalt caches its capability answers on disk, and a cache built before /baseband existed keeps saying
 * this machine has no cellular hardware: CommCenter then answers "
'abmtool' is not supported on this product" and
 * never starts the baseband manager. The reference implementation removes the file on every baseband boot so the
 * cache is rebuilt from the device tree and the IORegistry, which now carry the node. */
extern int unlink1(vfs_context_t, vnode_t, user_addr_t, int, int);

static void
sheng_reset_mobilegestalt(void)
{
    static const char *const candidates[] = {
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist",
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches",
        "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache",
        "/private/var/containers/Shared/SystemGroup",
        "/private/var/mobile/Library/Caches/com.apple.MobileGestalt.plist",
        "/private/var/preferences/com.apple.MobileGestalt.plist",
    };
    vfs_context_t context = vfs_context_kernel();
    for (unsigned i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        vnode_t vnode = NULLVP;
        const int opened = vnode_open(candidates[i], FREAD | O_NOFOLLOW, 0, 0, &vnode, context);
        if (!opened) vnode_close(vnode, FREAD, context);
        printf("sheng baseband: %s answers %d\n", candidates[i], opened);
    }
    static const char path[] = "/private/var/containers/Shared/SystemGroup/"
        "systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist";
    const int error = unlink1(context, NULLVP, (user_addr_t)(uintptr_t)path, UIO_SYSSPACE, 0);
    printf("sheng baseband: the MobileGestalt cache was removed: %d\n", error);
}

char sheng_baseband_preboot_entry[256];
char sheng_baseband_standalone_entry[256];

void sheng_boot_baseband_firmware(proc_t p, uint64_t scratch)
{
    sheng_reset_mobilegestalt();
    fw_probe(p, (user_addr_t)scratch);
    const uint8_t *addon = NULL;
    uint64_t bytes = 0;
    if (!native_ram_block_gpu_addon || !native_ram_block_gpu_addon(&addon, &bytes) || bytes < 24 ||
        memcmp(addon, "NGPUADD2", 8)) {
        printf("sheng baseband firmware: no addon on this boot\n");
        return;
    }
    const uint32_t count = (uint32_t)fw_le32(addon + 8);
    uint64_t at = 24;
    const uint8_t *packed = NULL;
    uint64_t packedBytes = 0, plainBytes = 0;
    for (uint32_t i = 0; i < count && bytes - at >= 24; ++i) {
        const uint32_t nameBytes = (uint32_t)fw_le32(addon + at);
        const uint64_t size = ((uint64_t)fw_le32(addon + at + 12) << 32) | fw_le32(addon + at + 8);
        const uint64_t blob = ((uint64_t)fw_le32(addon + at + 20) << 32) | fw_le32(addon + at + 16);
        at += 24;
        const uint64_t namePadded = (nameBytes + 7) & ~7ULL, bodyPadded = (blob + 7) & ~7ULL;
        if (bytes - at < namePadded) return;
        const char *name = (const char *)(addon + at);
        if (nameBytes > 8 && strnstr(name, ".bbfw", nameBytes)) { packed = addon + at + namePadded; packedBytes = blob; plainBytes = size; }
        at += namePadded + bodyPadded;
    }
    if (!packed || plainBytes < 22 || plainBytes > (64u << 20) || packedBytes > (64u << 20)) {
        printf("sheng baseband firmware: the package is not in the addon (%llu packed, %llu plain)\n", packedBytes, plainBytes);
        return;
    }
    printf("sheng baseband firmware: %llu packed -> %llu bytes\n", packedBytes, plainBytes);
    uint8_t *zip = kalloc_data((size_t)plainBytes, Z_WAITOK);
    if (!zip) { printf("sheng baseband firmware: no memory for %llu bytes\n", plainBytes); return; }
    z_stream stream = {0};
    stream.zalloc = fw_zalloc;
    stream.zfree = fw_zfree;
    int error = inflateInit(&stream);
    uint64_t written = 0;
    if (error == Z_OK) {
        stream.next_in = (Bytef *)packed; stream.avail_in = (uInt)packedBytes;
        stream.next_out = zip; stream.avail_out = (uInt)plainBytes;
        error = inflate(&stream, Z_FINISH) == Z_STREAM_END && stream.total_out == plainBytes ? Z_OK : Z_DATA_ERROR;
        inflateEnd(&stream);
    }
    if (error == Z_OK) {
        printf("sheng baseband firmware: entry inflated, head %02x %02x %02x %02x\n", zip[0], zip[1], zip[2], zip[3]);
        /* The reference layout: <base>/standalone goes over /usr/standalone/firmware and <base>/preboot over
         * /private/preboot, where the tree below the boot-manifest hash holds the same images. The Data volume
         * accepts these paths here, which is what the reference relies on. */
        /* Two trees, written directly: the standalone one is union-mounted over /usr/standalone/firmware, and
         * the preboot one is the real path ABM reads the personalized images from. No bind of the folder is
         * made: this port's bindfs does not compose a bind onto a subdirectory of a tree that is then mounted. */
        static const char standalone[] = "/private/var/coachz/baseband/standalone/Baseband/Mav16";
        strlcpy(sheng_baseband_standalone_entry, standalone, sizeof sheng_baseband_standalone_entry);
        const int made = fw_mkdirs(p, scratch, standalone);
        printf("sheng baseband firmware: the standalone tree answers %d\n", made);
        if (!made) written = fw_unpack_zip(p, scratch, zip, (size_t)plainBytes, standalone);
        /* The port publishes the boot manifest hash itself during its SystemOS mount; that 48-byte value is the
         * name of the real preboot directory, so the preboot mountpoint is built under it. */
        DTEntry chosen = NULL;
        const void *hashValue = NULL;
        unsigned hashSize = 0;
        /* the patch's unions mount onto these two paths */
        unsigned char published[48] = {0};
        extern int fuxi_boot_manifest_hash(unsigned char *, unsigned);
        SecureDTLookupEntry(NULL, "/chosen", &chosen);
        if (!(SecureDTGetProperty(chosen, "boot-manifest-hash", &hashValue, &hashSize) == kSuccess &&
              hashSize && hashSize <= 48) &&
            fuxi_boot_manifest_hash(published, sizeof published)) {
            hashValue = published;
            hashSize = 48;
            printf("sheng baseband firmware: the port's published boot manifest hash is read through the IORegistry\n");
        }
        if (hashSize && hashSize <= 48) {
            char hex[97] = "";
            for (unsigned i = 0; i < hashSize; ++i) snprintf(hex + 2 * i, 3, "%02X", ((const unsigned char *)hashValue)[i]);
            /* A tree of our own on Data, named after the port's manifest hash; the patch unions it over
             * /private/preboot, so /private/preboot stays the sealed one (which cannot be written). */
            snprintf(sheng_baseband_preboot_entry, sizeof sheng_baseband_preboot_entry,
                     "/private/var/coachz/baseband/preboot/%s/usr/local/standalone/firmware/Baseband/Mav16", hex);
            const int prebootMade = fw_mkdirs(p, scratch, sheng_baseband_preboot_entry);
            printf("sheng baseband firmware: the preboot tree answers %d (%s)\n", prebootMade, hex);
            if (!prebootMade) written += fw_unpack_zip(p, scratch, zip, (size_t)plainBytes, sheng_baseband_preboot_entry);
        } else {
            printf("sheng baseband firmware: the port published no boot manifest hash\n");
        }

    }
    kfree_data(zip, (size_t)plainBytes);
    printf("sheng baseband firmware: %llu of the package's images written, status %d\n", written, error);
}

int sheng_boot_watchdog_policy(void *process, uint64_t scratch) {
 proc_t p=process;
 int enabled=0,error=0,fd=-1;int32_t result=0;
 PE_parse_boot_argn("sheng-disable-watchdogd",&enabled,sizeof(enabled));
 if(!PE_parse_boot_argn("sheng-disable-jobs",jobs,sizeof(jobs)))jobs[0]=0;jobs[sizeof(jobs)-1]=0;
 int lite=0;PE_parse_boot_argn("sheng-lite",&lite,sizeof(lite));if(lite!=1)lite=0;
 if(enabled!=1)enabled=0;
 for(const char *c=jobs;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='-'||*c==','))return EINVAL;
 int setup=0,reset=0;
 PE_parse_boot_argn("sheng-setup",&setup,sizeof(setup));
 PE_parse_boot_argn("sheng-data-reset",&reset,sizeof(reset));   // 2 = erase Data and show the Hello page; 1 = erase Data, Setup stays skipped
 if(reset==2)setup=1;
 if(setup==1 && (error=sheng_boot_setup_reset(p,scratch))) {
  sheng_watchdog_policy_status=error;
  printf("sheng setup flags reset failed: %d\n",error);
  return error;
 }
 if(setup==1)printf("sheng setup flags reset: Hello page on this boot\n");
 if(!enabled && !jobs[0] && !lite)return 0;
 policy[0]=0;strlcat(policy,policy_head,sizeof(policy));
 if(enabled)strlcat(policy,policy_watchdogd,sizeof(policy));
 if(lite)for(const char *c=lite_jobs;*c;) {if((error=append_job(&c)))return error;}
 for(const char *c=jobs;*c;) {if((error=append_job(&c)))return error;}
 strlcat(policy,policy_tail,sizeof(policy));
 const char *directory="/private/var/db/com.apple.xpc.launchd";
 const char *path="/private/var/db/com.apple.xpc.launchd/disabled.plist";
 if((error=copyout(directory,scratch,strlen(directory)+1)))goto done;
 struct mkdir_args md={.path=scratch,.mode=0755};
 error=mkdir(p,&md,&result);
 if(error && error!=EEXIST)goto done;
 if((error=copyout(path,scratch,strlen(path)+1)))goto done;
 // With persistent Data the previous boot's file is there: this policy is rewritten in place on every boot (it must follow the
 // boot arguments). Symlinks are still refused (O_NOFOLLOW) and the fstat check below insists on a root-owned regular file.
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0644};
 if((error=open(p,&op,&result)))goto done;
 fd=result;
 // The scratch page keeps the cache-mount environment slots at +0x400, so the policy (the lite list makes it longer than 0x200 bytes) goes
 // out in pieces through the 0x200 bytes at +0x200.
 for(size_t sent=0,total=strlen(policy);sent<total;) {
  size_t piece=total-sent<0x1f0?total-sent:0x1f0;
  if((error=copyout(policy+sent,scratch+0x200,piece)))goto done;
  struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=piece};
  user_ssize_t written=0;
  if((error=write(p,&wr,&written)))goto done;
  if(written!=(user_ssize_t)piece){error=EIO;goto done;}
  sent+=piece;
 }
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 if((error=fchown(p,&owner,&result)))goto done;
 struct fchmod_args mode={.fd=fd,.mode=0644};
 if((error=fchmod(p,&mode,&result)))goto done;
 struct fsync_args sync={.fd=fd};
 if((error=fsync(p,&sync,&result)))goto done;
 // Preserve the cache-mount environment slots at +0x400, +0x500, +0x600.
 struct fstat64_args sa={.fd=fd,.ub=scratch+0x800};struct stat64 st={0};
 if((error=sys_fstat64(p,&sa,&result)) || (error=copyin(sa.ub,&st,sizeof(st))))goto done;
 if(st.st_uid!=0 || st.st_gid!=0 || !S_ISREG(st.st_mode) || (st.st_mode&0777)!=0644 || st.st_size!=(off_t)(strlen(policy)))error=EINVAL;
 done:
 if(fd>=0){struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;}
 // A real empty migration marker selects the qualified modern override loader.
 if(!error) {
  const char *marker="/private/var/db/com.apple.xpc.launchd/disabled.migrated";
  error=copyout(marker,scratch,strlen(marker)+1);
  if(!error) {
   struct open_args mark={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
   error=open(p,&mark,&result);
   if(!error){struct close_args cl={.fd=result};error=sys_close(p,&cl,&result);}
   else if(error==EEXIST)error=0;
  }
 }
 sheng_watchdog_policy_status=error;
 printf("sheng bring-up watchdogd launch policy: %d (hardware watchdog not implemented)\n",error);
 return error;
}
