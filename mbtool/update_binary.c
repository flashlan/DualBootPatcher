/*
 * Copyright (C) 2014  Xiao-Long Chen <chenxiaolong@cxl.epac.to>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "update_binary.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <archive.h>
#include <archive_entry.h>

#include "main.h"
#include "roms.h"
#include "util/archive.h"
#include "util/command.h"
#include "util/copy.h"
#include "util/delete.h"
#include "util/directory.h"
#include "util/file.h"
#include "util/logging.h"
#include "util/mount.h"
#include "util/properties.h"


const char *BUSYBOX_WRAPPER =
"#!/sbin/busybox_orig sh"                                               "\n"
""                                                                      "\n"
"do_mount() {"                                                          "\n"
"    echo \"mount command disabled in chroot environment\" >&2"         "\n"
"    exit 0"                                                            "\n"
"}"                                                                     "\n"
""                                                                      "\n"
"do_umount() {"                                                         "\n"
"    echo \"umount command disabled in chroot environment\" >&2"        "\n"
"    exit 0"                                                            "\n"
"}"                                                                     "\n"
""                                                                      "\n"
"do_unzip() {"                                                          "\n"
"    /sbin/unzip \"${@}\""                                              "\n"
"    exit \"${?}\""                                                     "\n"
"}"                                                                     "\n"
""                                                                      "\n"
"argv0=\"${0##*/}\""                                                    "\n"
"tool=\"\""                                                             "\n"
""                                                                      "\n"
"if [ \"x${argv0}\" = \"xbusybox\" ]; then"                             "\n"
"    tool=\"${1}\""                                                     "\n"
"    shift"                                                             "\n"
"else"                                                                  "\n"
"    tool=\"${argv0}\""                                                 "\n"
"fi"                                                                    "\n"
""                                                                      "\n"
"case \"${tool}\" in"                                                   "\n"
"    mount) do_mount ;;"                                                "\n"
"    umount) do_umount ;;"                                              "\n"
"    unzip) do_unzip \"${@}\" ;;"                                       "\n"
"esac"                                                                  "\n"
""                                                                      "\n"
"if [ \"x${argv0}\" = \"xbusybox\" ]; then"                             "\n"
"    /sbin/busybox_orig \"${@}\""                                       "\n"
"else"                                                                  "\n"
"    /sbin/busybox_orig \"${tool}\" \"${@}\""                           "\n"
"fi"                                                                    "\n"
"exit \"${?}\""                                                         "\n"
;


/* Lots of paths */

// ZIP_*:  Paths in zip file
// TEMP_*: Temporary paths (eg. for extracted files)

#define CHROOT          "/chroot"

#define HELPER_TOOL     "update-binary-tool"

#define ZIP_UPDATER     "META-INF/com/google/android/update-binary"
#define TEMP_UPDATER    "/tmp/updater"

#define TEMP_INST_ZIP   "/tmp/install.zip"
#define TEMP_SYSTEM_IMG "/tmp/system.img"

#define ZIP_E2FSCK      "multiboot/e2fsck"
#define TEMP_E2FSCK     "/tmp/e2fsck"

#define ZIP_RESIZE2FS   "multiboot/resize2fs"
#define TEMP_RESIZE2FS  "/tmp/resize2fs"

#define ZIP_UNZIP       "multiboot/unzip"
#define TEMP_UNZIP      "/tmp/unzip"

#define ZIP_AROMA       "multiboot/aromawrapper.zip"
#define TEMP_AROMA      "/tmp/aromawrapper.zip"

#define ZIP_DEVICE      "multiboot/device"
#define TEMP_DEVICE     "/tmp/device"


static const char *interface;
static const char *output_fd_str;
static const char *zip_file;

static int output_fd;


static void ui_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    char newfmt[100];
    snprintf(newfmt, 100, "ui_print [MultiBoot] %s\n", fmt);
    vdprintf(output_fd, newfmt, ap);
    dprintf(output_fd, "ui_print\n");

    va_end(ap);
}

#define MKDIR_CHECKED(a, b) \
    if (mkdir(a, b) < 0) { \
        LOGE("Failed to create %s: %s", a, strerror(errno)); \
        goto error; \
    }
#define MOUNT_CHECKED(a, b, c, d, e) \
    if (mount(a, b, c, d, e) < 0) { \
        LOGE("Failed to mount %s (%s) at %s: %s", a, c, b, strerror(errno)); \
        goto error; \
    }
#define MKNOD_CHECKED(a, b, c) \
    if (mknod(a, b, c) < 0) { \
        LOGE("Failed to create special file %s: %s", a, strerror(errno)); \
        goto error; \
    }
#define IS_MOUNTED_CHECKED(a) \
    if (!mb_is_mounted(a)) { \
        LOGE("%s is not mounted", a); \
        goto error; \
    }
#define UNMOUNT_ALL_CHECKED(a) \
    if (mb_unmount_all(a) < 0) { \
        LOGE("Failed to unmount all mountpoints within %s", a); \
        goto error; \
    }
#define RECURSIVE_DELETE_CHECKED(a) \
    if (mb_delete_recursive(a) < 0) { \
        LOGE("Failed to recursively remove %s", a); \
        goto error; \
    }
#define COPY_DIR_CONTENTS_CHECKED(a, b, c) \
    if (mb_copy_dir(a, b, c) < 0) { \
        LOGE("Failed to copy contents of %s/ to %s/", a, b); \
        goto error; \
    }

static int create_chroot(void)
{
    // We'll just call the recovery's mount tools directly to avoid having to
    // parse TWRP and CWM's different fstab formats
    char *mount_system[] = { "mount", "/system", NULL };
    char *mount_cache[] = { "mount", "/cache", NULL };
    char *mount_data[] = { "mount", "/data", NULL };
    mb_run_command(mount_system);
    mb_run_command(mount_cache);
    mb_run_command(mount_data);

    // Make sure everything really is mounted
    IS_MOUNTED_CHECKED("/system");
    IS_MOUNTED_CHECKED("/cache");
    IS_MOUNTED_CHECKED("/data");

    // Unmount everything previously mounted in /chroot
    UNMOUNT_ALL_CHECKED(CHROOT);

    // Remove /chroot if it exists
    RECURSIVE_DELETE_CHECKED(CHROOT);

    // Setup directories
    MKDIR_CHECKED(CHROOT, 0755);
    MKDIR_CHECKED(CHROOT "/dev", 0755);
    MKDIR_CHECKED(CHROOT "/etc", 0755);
    MKDIR_CHECKED(CHROOT "/proc", 0755);
    MKDIR_CHECKED(CHROOT "/sbin", 0755);
    MKDIR_CHECKED(CHROOT "/sys", 0755);
    MKDIR_CHECKED(CHROOT "/tmp", 0755);

    MKDIR_CHECKED(CHROOT "/data", 0755);
    MKDIR_CHECKED(CHROOT "/cache", 0755);
    MKDIR_CHECKED(CHROOT "/system", 0755);

    // Other mounts
    MOUNT_CHECKED("none", CHROOT "/dev",            "tmpfs",     0, "");
    MKDIR_CHECKED(CHROOT "/dev/pts", 0755);
    MOUNT_CHECKED("none", CHROOT "/dev/pts",        "devpts",    0, "");
    MOUNT_CHECKED("none", CHROOT "/proc",           "proc",      0, "");
    MOUNT_CHECKED("none", CHROOT "/sys",            "sysfs",     0, "");
    MOUNT_CHECKED("none", CHROOT "/sys/fs/selinux", "selinuxfs", 0, "");
    MOUNT_CHECKED("none", CHROOT "/tmp",            "tmpfs",     0, "");
    // Copy the contents of sbin since we need to mess with some of the binaries
    // there. Also, for whatever reason, bind mounting /sbin results in EINVAL
    // no matter if it's done from here or from busybox.
    MOUNT_CHECKED("none", CHROOT "/sbin",           "tmpfs",     0, "");
    COPY_DIR_CONTENTS_CHECKED("/sbin", CHROOT "/sbin",
                              MB_COPY_ATTRIBUTES
                              | MB_COPY_XATTRS
                              | MB_COPY_EXCLUDE_TOP_LEVEL);

    // Don't create unnecessary special files in /dev to avoid install scripts
    // from overwriting partitions
    MKNOD_CHECKED(CHROOT "/dev/console", S_IFCHR | 0644, makedev(5, 1));
    MKNOD_CHECKED(CHROOT "/dev/null",    S_IFCHR | 0644, makedev(1, 3));
    MKNOD_CHECKED(CHROOT "/dev/ptmx",    S_IFCHR | 0644, makedev(5, 2));
    MKNOD_CHECKED(CHROOT "/dev/random",  S_IFCHR | 0644, makedev(1, 8));
    MKNOD_CHECKED(CHROOT "/dev/tty",     S_IFCHR | 0644, makedev(5, 0));
    MKNOD_CHECKED(CHROOT "/dev/urandom", S_IFCHR | 0644, makedev(1, 9));
    MKNOD_CHECKED(CHROOT "/dev/zero",    S_IFCHR | 0644, makedev(1, 5));

    // Create a few loopback devices in case we need to use them
    MKNOD_CHECKED(CHROOT "/dev/loop0",   S_IFBLK | 0644, makedev(7, 0));
    MKNOD_CHECKED(CHROOT "/dev/loop1",   S_IFBLK | 0644, makedev(7, 1));
    MKNOD_CHECKED(CHROOT "/dev/loop2",   S_IFBLK | 0644, makedev(7, 2));

    // We need /dev/input/* and /dev/graphics/* for AROMA
#if 1
    COPY_DIR_CONTENTS_CHECKED("/dev/input", CHROOT "/dev/input",
                              MB_COPY_ATTRIBUTES
                              | MB_COPY_XATTRS
                              | MB_COPY_EXCLUDE_TOP_LEVEL);
    COPY_DIR_CONTENTS_CHECKED("/dev/graphics", CHROOT "/dev/graphics",
                              MB_COPY_ATTRIBUTES
                              | MB_COPY_XATTRS
                              | MB_COPY_EXCLUDE_TOP_LEVEL);
#else
    MKDIR_CHECKED(CHROOT "/dev/input", 0755);
    MKDIR_CHECKED(CHROOT "/dev/graphics", 0755);
    MOUNT_CHECKED("/dev/input", CHROOT "/dev/input", "", MS_BIND, "");
    MOUNT_CHECKED("/dev/graphics", CHROOT "/dev/graphics", "", MS_BIND, "");
#endif

    mb_create_empty_file(CHROOT "/.chroot");

    return 0;

error:
    return -1;
}

static int destroy_chroot(void)
{
    // TODO: Copy /images/boot.img to /data/media/0/MultiKernels/
    // chmod 775 the kernel

    umount(CHROOT "/dev/pts");
    umount(CHROOT "/dev");
    umount(CHROOT "/proc");
    umount(CHROOT "/sys/fs/selinux");
    umount(CHROOT "/sys");
    umount(CHROOT "/tmp");
    umount(CHROOT "/sbin");

    umount(CHROOT "/system");
    umount(CHROOT "/cache");
    umount(CHROOT "/data");

    umount(CHROOT TEMP_SYSTEM_IMG);

    // Unmount everything previously mounted in /chroot
    if (mb_unmount_all(CHROOT) < 0) {
        LOGE("Failed to unmount previous mount points in " CHROOT);
        return -1;
    }

    mb_delete_recursive(CHROOT);

    return 0;
}

static char * get_target_device(void)
{
    struct extract_info files[] = {
        { ZIP_DEVICE, TEMP_DEVICE },
        { NULL, NULL }
    };

    if (mb_extract_files2(zip_file, files) < 0) {
        LOGE("Failed to extract " ZIP_DEVICE " from zip file");
        return NULL;
    }

    char *device = NULL;
    if (mb_file_first_line(TEMP_DEVICE, &device) < 0) {
        LOGE("Failed to read " TEMP_DEVICE);
        return NULL;
    }

    return device;
}

static int setup_e2fsprogs(void)
{
    struct extract_info files[] = {
        { ZIP_E2FSCK, TEMP_E2FSCK },
        { ZIP_RESIZE2FS, TEMP_RESIZE2FS },
        { NULL, NULL }
    };

    if (mb_extract_files2(zip_file, files) < 0) {
        LOGE("Failed to extract e2fsprogs from zip file");
        return -1;
    }

    if (chmod(TEMP_E2FSCK, 0555) < 0) {
        LOGE(TEMP_E2FSCK ": Failed to chmod: %s", strerror(errno));
        return -1;
    }

    if (chmod(TEMP_RESIZE2FS, 0555) < 0) {
        LOGE(TEMP_RESIZE2FS ": Failed to chmod: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int setup_unzip(void)
{
    struct extract_info files[] = {
        { ZIP_UNZIP, TEMP_UNZIP },
        { NULL, NULL }
    };

    if (mb_extract_files2(zip_file, files) < 0) {
        LOGE("Failed to extract " ZIP_UNZIP " from zip file");
        return -1;
    }

    remove("/chroot/sbin/unzip");

    if (mb_copy_file(TEMP_UNZIP, CHROOT "/sbin/unzip",
                     MB_COPY_ATTRIBUTES | MB_COPY_XATTRS) < 0) {
        LOGE("Failed to copy %s to %s", TEMP_UNZIP, CHROOT "/sbin/unzip");
        return -1;
    }

    return 0;
}

static int setup_busybox_wrapper(void)
{
    rename(CHROOT "/sbin/busybox", CHROOT "/sbin/busybox_orig");
    FILE *fp = fopen(CHROOT "/sbin/busybox", "wb");
    if (!fp) {
        LOGE("Failed to open " CHROOT "/sbin/busybox: %s", strerror(errno));
        return -1;
    }
    fwrite(BUSYBOX_WRAPPER, 1, strlen(BUSYBOX_WRAPPER), fp);
    fclose(fp);
    chmod(CHROOT "/sbin/busybox", 0555);

    return 0;
}

static int create_or_enlarge_image(const char *path)
{
    // Create a 3GB image image (or extend an existing one) for installing a
    // ROM. There isn't really a point to calculate an approximate size, since
    // either way, if there isn't enough space, the ROM will fail to install
    // anyway. After installation, the image will be shrunken to its minimum
    // size.

#define IMAGE_SIZE "3G"

    if (mb_mkdir_parent(path, S_IRWXU) < 0) {
        LOGE("%s: Failed to create parent directory: %s",
             path, strerror(errno));
        return -1;
    }

    struct stat sb;
    if (stat(path, &sb) < 0) {
        if (errno != ENOENT) {
            LOGE("%s: Failed to stat: %s", path, strerror(errno));
            return -1;
        } else {
            LOGD("%s: Creating new %s ext4 image", path, IMAGE_SIZE);

            // Create new image
            const char *argv[] =
                    { "make_ext4fs", "-l", IMAGE_SIZE, path, NULL };
            if (mb_run_command((char **) argv) != 0) {
                LOGE("%s: Failed to create image", path);
                return -1;
            }
            return 0;
        }
    }

    // Force an fsck to make resize2fs happy
    const char *e2fsck_argv[] = { TEMP_E2FSCK, "-f", "-y", path, NULL };
    if (mb_run_command((char **) e2fsck_argv) != 0) {
        LOGE("%s: Failed to run e2fsck", path);
        return -1;
    }

    LOGD("%s: Enlarging to %s", path, IMAGE_SIZE);

    // Enlarge existing image
    const char *resize2fs_argv[] = { TEMP_RESIZE2FS, path, IMAGE_SIZE, NULL };
    if (mb_run_command((char **) resize2fs_argv) != 0) {
        LOGE("%s: Failed to run resize2fs", path);
        return -1;
    }

    return 0;
}

static int shrink_image(const char *path)
{
    // Force an fsck to make resize2fs happy
    const char *e2fsck_argv[] = { TEMP_E2FSCK, "-f", "-y", path, NULL };
    if (mb_run_command((char **) e2fsck_argv) != 0) {
        LOGE("%s: Failed to run e2fsck", path);
        return -1;
    }

    // Shrink image
    // resize2fs will not go below the worst case scenario, so multiple resizes
    // are needed to achieve the "true" minimum
    const char *resize2fs_argv[] = { TEMP_RESIZE2FS, "-M", path, NULL };
    for (int i = 0; i < 5; ++i) {
        if (mb_run_command((char **) resize2fs_argv) != 0) {
            LOGE("%s: Failed to run resize2fs", path);
            return -1;
        }
    }

    return 0;
}

// Returns -1 on error, 1 if path is AROMA installer, 0 otherwise
static int is_aroma(const char *path)
{
    // Possible strings we can search for
    static const char *AROMA_MAGIC[] = {
        "AROMA Installer",
        "support@amarullz.com",
        "(c) 2013 by amarullz xda-developers",
        "META-INF/com/google/android/aroma-config",
        "META-INF/com/google/android/aroma",
        "AROMA_NAME",
        "AROMA_BUILD",
        "AROMA_VERSION",
        NULL
    };

    struct stat sb;
    void *map = MAP_FAILED;
    int fd = -1;
    const char **magic;
    int found = 0;
    int saved_errno;

    if ((fd = open(path, O_RDONLY)) < 0) {
        goto error;
    }

    if (fstat(fd, &sb) < 0) {
        goto error;
    }

    map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        goto error;
    }

    for (magic = AROMA_MAGIC; *magic; ++magic) {
        if (memmem(map, sb.st_size, *magic, strlen(*magic))) {
            found = 1;
            break;
        }
    }

    if (munmap(map, sb.st_size) < 0) {
        map = MAP_FAILED;
        goto error;
    }

    if (close(fd) < 0) {
        fd = -1;
        goto error;
    }

    return found;

error:
    saved_errno = errno;

    if (map != MAP_FAILED) {
        munmap(map, sb.st_size);
    }

    if (fd >= 0) {
        close(fd);
    }

    errno = saved_errno;

    return -1;
}

static int run_aroma_selection(void)
{
    struct extract_info files[] = {
        { ZIP_AROMA, CHROOT TEMP_AROMA },
        { NULL, NULL }
    };

    if (mb_extract_files2(zip_file, files) < 0) {
        LOGE("Failed to extract " ZIP_AROMA);
        return -1;
    }

    struct extract_info aroma_files[] = {
        { ZIP_UPDATER, CHROOT TEMP_UPDATER },
        { NULL, NULL }
    };

    if (mb_extract_files2(CHROOT TEMP_AROMA, aroma_files) < 0) {
        LOGE("Failed to extract " ZIP_UPDATER);
        return -1;
    }

    if (chmod(CHROOT TEMP_UPDATER, 0555) < 0) {
        LOGE("Failed to chmod " CHROOT TEMP_UPDATER);
        return -1;
    }

    const char *argv[] = {
        TEMP_UPDATER,
        interface,
        output_fd_str,
        TEMP_AROMA,
        NULL
    };

    pid_t parent = getppid();

    kill(parent, SIGSTOP);

    int ret;
    if ((ret = mb_run_command_chroot(CHROOT, (char **) argv)) != 0) {
        kill(parent, SIGCONT);

        if (ret < 0) {
            LOGE("Failed to execute " TEMP_UPDATER ": %s", strerror(errno));
        } else {
            LOGE(TEMP_UPDATER " return non-zero exit status");
        }
        return -1;
    }

    kill(parent, SIGCONT);

    return 0;
}

static int run_real_updater(void)
{
    struct extract_info files[] = {
        { ZIP_UPDATER ".orig", CHROOT TEMP_UPDATER },
        { NULL, NULL }
    };

    if (mb_extract_files2(zip_file, files) < 0) {
        LOGE("Failed to extract original update-binary file");
        return -1;
    }

    if (chmod(CHROOT TEMP_UPDATER, 0555) < 0) {
        LOGE("Failed to chmod " CHROOT TEMP_UPDATER);
        return -1;
    }

    const char *argv[] = {
        TEMP_UPDATER,
        interface,
        output_fd_str,
        TEMP_INST_ZIP,
        NULL
    };

    pid_t parent = getppid();
    int aroma = is_aroma(CHROOT TEMP_UPDATER) >= 0;

    if (aroma) {
        kill(parent, SIGSTOP);
    }

    int ret;
    if ((ret = mb_run_command_chroot(CHROOT, (char **) argv)) != 0) {
        if (aroma) {
            kill(parent, SIGCONT);
        }

        if (ret < 0) {
            LOGE("Failed to execute " TEMP_UPDATER ": %s", strerror(errno));
        } else {
            LOGE(TEMP_UPDATER " return non-zero exit status");
        }
        return -1;
    }

    if (aroma) {
        kill(parent, SIGCONT);
    }

    return 0;
}

static int update_binary(void)
{
    struct roms r;
    mb_roms_init(&r);
    mb_roms_get_builtin(&r);

    ui_print("Creating chroot environment");

    if (create_chroot() < 0) {
        ui_print("Failed to create chroot environment");
        goto error;
    }

    char *device = get_target_device();
    char prop1[MB_PROP_VALUE_MAX] = { 0 };
    char prop2[MB_PROP_VALUE_MAX] = { 0 };
    mb_get_property("ro.product.device", prop1, "");
    mb_get_property("ro.build.product", prop2, "");

    if (!device) {
        ui_print("Failed to determine target device");
        goto error;
    }

    LOGD("ro.product.device = %s", prop1);
    LOGD("ro.build.product = %s", prop2);
    LOGD("Target device = %s", device);

    if (!strstr(prop1, device) && !strstr(prop2, device)) {
        ui_print("The patched zip is for '%s'. This device is '%s'.", device, prop1);
        free(device);
        goto error;
    }

    free(device);

    if (run_aroma_selection() < 0) {
        ui_print("Failed to run AROMA");
        goto error;
    }

    char *install_type;
    if (mb_file_first_line(CHROOT "/tmp/installtype", &install_type) < 0) {
        ui_print("Failed to determine install type");
        goto error;
    }

    if (strcmp(install_type, "cancelled") == 0) {
        ui_print("Cancelled installation");
        free(install_type);
        goto success;
    }

    struct rom *rom = mb_find_rom_by_id(&r, install_type);
    if (!rom) {
        ui_print("Unknown ROM ID: %s", install_type);
        free(install_type);
        goto error;
    }

    free(install_type);

    ui_print("ROM ID: %s", rom->id);
    ui_print("- /system: %s", rom->system_path);
    ui_print("- /cache: %s", rom->cache_path);
    ui_print("- /data: %s", rom->data_path);

    if (setup_e2fsprogs() < 0) {
        ui_print("Failed to extract e2fsprogs");
        goto error;
    }

    if (setup_unzip() < 0) {
        ui_print("Failed to extract unzip tool");
        goto error;
    }

    if (mb_bind_mount(rom->cache_path, 0771, CHROOT "/cache", 0771) < 0) {
        ui_print("Failed to bind mount %s to %s",
                 rom->cache_path, CHROOT "/cache");
        goto error;
    }

    if (mb_bind_mount(rom->data_path, 0771, CHROOT "/data", 0771) < 0) {
        ui_print("Failed to bind mount %s to %s",
                 rom->data_path, CHROOT "/data");
        goto error;
    }

    if (rom->system_uses_image) {
        if (create_or_enlarge_image(rom->system_path) < 0) {
            ui_print("Failed to create or enlarge image %s", rom->system_path);
            goto error;
        }

        // The installer will handle the mounting and unmounting
        mb_create_empty_file(CHROOT TEMP_SYSTEM_IMG);
        MOUNT_CHECKED(rom->system_path, CHROOT TEMP_SYSTEM_IMG,
                      "", MS_BIND, "");
    } else {
        ui_print("Installing ROMs to a directory is no longer supported");
        goto error;
    }

    // Bind-mount zip file
    mb_create_empty_file(CHROOT TEMP_INST_ZIP);
    MOUNT_CHECKED(zip_file, CHROOT TEMP_INST_ZIP, "", MS_BIND, "");

    setup_busybox_wrapper();

    // Copy ourself for the real update-binary to use
    mb_copy_file(mb_self_get_path(), CHROOT HELPER_TOOL,
                 MB_COPY_ATTRIBUTES | MB_COPY_XATTRS);
    chmod(CHROOT HELPER_TOOL, 0555);

    // Run real update-binary
    ui_print("Running real update-binary");
    ui_print("Here we go!");

    int ret;
    if ((ret = run_real_updater()) < 0) {
        ui_print("Failed to run real update-binary");
    }

    if (rom->system_uses_image) {
        umount(CHROOT "/system");
        if (shrink_image(rom->system_path) < 0) {
            ui_print("Failed to shrink system image");
            goto error;
        }
    }

    if (ret < 0) {
        goto error;
    }

success:
    ui_print("Destroying chroot environment");

    umount(CHROOT TEMP_INST_ZIP);

    mb_roms_cleanup(&r);

    if (destroy_chroot() < 0) {
        ui_print("Failed to destroy chroot environment. You should reboot into"
                 " recovery again to avoid flashing issues.");
        return -1;
    }

    return 0;

error:
    ui_print("Destroying chroot environment");

    umount(CHROOT TEMP_INST_ZIP);

    mb_roms_cleanup(&r);

    destroy_chroot();

    ui_print("Failed to flash zip file.");

    return -1;
}

static void update_binary_usage(int error)
{
    FILE *stream = error ? stderr : stdout;

    fprintf(stream,
            "Usage: update-binary [interface version] [output fd] [zip file]\n\n"
            "This tool wraps the real update-binary program by mounting the correct\n"
            "partitions in a chroot environment and then calls the real program.\n"
            "The real update-binary must be " ZIP_UPDATER ".orig\n"
            "in the zip file.\n\n"
            "Note: The interface version argument is completely ignored.\n");
}

int update_binary_main(int argc, char *argv[])
{
    int opt;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int long_index = 0;

    while ((opt = getopt_long(argc, argv, "h", long_options, &long_index)) != -1) {
        switch (opt) {
        case 'h':
            update_binary_usage(0);
            return EXIT_SUCCESS;

        default:
            update_binary_usage(1);
            return EXIT_FAILURE;
        }
    }

    if (argc - optind != 3) {
        update_binary_usage(1);
        return EXIT_FAILURE;
    }

    char *ptr;
    output_fd = strtol(argv[2], &ptr, 10);
    if (*ptr != '\0' || output_fd < 0) {
        fprintf(stderr, "Invalid output fd");
        return EXIT_FAILURE;
    }

    interface = argv[1];
    output_fd_str = argv[2];
    zip_file = argv[3];

    // stdout is messed up when it's appended to /tmp/recovery.log
    mb_log_set_standard_stream_all(stderr);

    return update_binary() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
