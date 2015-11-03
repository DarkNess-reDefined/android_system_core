/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <functional>
#include <memory>
#include <vector>

#include "sysdeps.h"

#include "adb.h"
#include "adb_client.h"
#include "adb_io.h"
#include "adb_utils.h"
#include "file_sync_service.h"
#include "line_printer.h"

#include <base/file.h>
#include <base/strings.h>
#include <base/stringprintf.h>

struct syncsendbuf {
    unsigned id;
    unsigned size;
    char data[SYNC_DATA_MAX];
};

class SyncConnection {
  public:
    SyncConnection() : total_bytes(0), start_time_ms_(CurrentTimeMs()) {
        max = SYNC_DATA_MAX; // TODO: decide at runtime.

        std::string error;
        fd = adb_connect("sync:", &error);
        if (fd < 0) {
            Error("connect failed: %s", error.c_str());
        }
    }

    ~SyncConnection() {
        if (!IsValid()) return;

        if (SendQuit()) {
            // We sent a quit command, so the server should be doing orderly
            // shutdown soon. But if we encountered an error while we were using
            // the connection, the server might still be sending data (before
            // doing orderly shutdown), in which case we won't wait for all of
            // the data nor the coming orderly shutdown. In the common success
            // case, this will wait for the server to do orderly shutdown.
            ReadOrderlyShutdown(fd);
        }
        adb_close(fd);
    }

    bool IsValid() { return fd >= 0; }

    bool SendRequest(int id, const char* path_and_mode) {
        size_t path_length = strlen(path_and_mode);
        if (path_length > 1024) {
            Error("SendRequest failed: path too long: %zu", path_length);
            errno = ENAMETOOLONG;
            return false;
        }

        // Sending header and payload in a single write makes a noticeable
        // difference to "adb sync" performance.
        std::vector<char> buf(sizeof(SyncRequest) + path_length);
        SyncRequest* req = reinterpret_cast<SyncRequest*>(&buf[0]);
        req->id = id;
        req->path_length = path_length;
        char* data = reinterpret_cast<char*>(req + 1);
        memcpy(data, path_and_mode, path_length);

        return WriteFdExactly(fd, &buf[0], buf.size());
    }

    // Sending header, payload, and footer in a single write makes a huge
    // difference to "adb sync" performance.
    bool SendSmallFile(const char* path_and_mode,
                       const char* rpath,
                       const char* data, size_t data_length,
                       unsigned mtime) {
        Print(rpath);

        size_t path_length = strlen(path_and_mode);
        if (path_length > 1024) {
            Error("SendSmallFile failed: path too long: %zu", path_length);
            errno = ENAMETOOLONG;
            return false;
        }

        std::vector<char> buf(sizeof(SyncRequest) + path_length +
                 sizeof(SyncRequest) + data_length +
                 sizeof(SyncRequest));
        char* p = &buf[0];

        SyncRequest* req_send = reinterpret_cast<SyncRequest*>(p);
        req_send->id = ID_SEND;
        req_send->path_length = path_length;
        p += sizeof(SyncRequest);
        memcpy(p, path_and_mode, path_length);
        p += path_length;

        SyncRequest* req_data = reinterpret_cast<SyncRequest*>(p);
        req_data->id = ID_DATA;
        req_data->path_length = data_length;
        p += sizeof(SyncRequest);
        memcpy(p, data, data_length);
        p += data_length;

        SyncRequest* req_done = reinterpret_cast<SyncRequest*>(p);
        req_done->id = ID_DONE;
        req_done->path_length = mtime;
        p += sizeof(SyncRequest);

        if (!WriteFdExactly(fd, &buf[0], (p - &buf[0]))) return false;

        total_bytes += data_length;
        return true;
    }

    bool CopyDone(const char* from, const char* to) {
        syncmsg msg;
        if (!ReadFdExactly(fd, &msg.status, sizeof(msg.status))) {
            Error("failed to copy '%s' to '%s': no ID_DONE: %s", from, to, strerror(errno));
            return false;
        }
        if (msg.status.id == ID_OKAY) {
            return true;
        }
        if (msg.status.id != ID_FAIL) {
            Error("failed to copy '%s' to '%s': unknown reason %d", from, to, msg.status.id);
            return false;
        }
        return ReportCopyFailure(from, to, msg);
    }

    bool ReportCopyFailure(const char* from, const char* to, const syncmsg& msg) {
        std::vector<char> buf(msg.status.msglen + 1);
        if (!ReadFdExactly(fd, &buf[0], msg.status.msglen)) {
            Error("failed to copy '%s' to '%s'; failed to read reason (!): %s",
                  from, to, strerror(errno));
            return false;
        }
        buf[msg.status.msglen] = 0;
        Error("failed to copy '%s' to '%s': %s", from, to, &buf[0]);
        return false;
    }

    std::string TransferRate() {
        uint64_t ms = CurrentTimeMs() - start_time_ms_;
        if (total_bytes == 0 || ms == 0) return "";

        double s = static_cast<double>(ms) / 1000LL;
        double rate = (static_cast<double>(total_bytes) / s) / (1024*1024);
        return android::base::StringPrintf(" %.1f MB/s (%" PRId64 " bytes in %.3fs)",
                                           rate, total_bytes, s);
    }

    void Print(const std::string& s) {
        // TODO: we actually don't want ELIDE; we want "ELIDE if smart, FULL if dumb".
        line_printer_.Print(s, LinePrinter::ELIDE);
    }

    void Printf(const char* fmt, ...) __attribute__((__format__(ADB_FORMAT_ARCHETYPE, 2, 3))) {
        std::string s;
        va_list ap;
        va_start(ap, fmt);
        android::base::StringAppendV(&s, fmt, ap);
        va_end(ap);

        Print(s);
    }

    void Error(const char* fmt, ...) __attribute__((__format__(ADB_FORMAT_ARCHETYPE, 2, 3))) {
        std::string s = "adb: error: ";

        va_list ap;
        va_start(ap, fmt);
        android::base::StringAppendV(&s, fmt, ap);
        va_end(ap);

        line_printer_.Print(s, LinePrinter::FULL);
    }

    uint64_t total_bytes;

    // TODO: add a char[max] buffer here, to replace syncsendbuf...
    int fd;
    size_t max;

  private:
    uint64_t start_time_ms_;

    LinePrinter line_printer_;

    bool SendQuit() {
        return SendRequest(ID_QUIT, ""); // TODO: add a SendResponse?
    }

    static uint64_t CurrentTimeMs() {
        struct timeval tv;
        gettimeofday(&tv, 0); // (Not clock_gettime because of Mac/Windows.)
        return static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    }
};

typedef void (sync_ls_cb)(unsigned mode, unsigned size, unsigned time, const char* name);

static bool sync_ls(SyncConnection& sc, const char* path,
                    std::function<sync_ls_cb> func) {
    if (!sc.SendRequest(ID_LIST, path)) return false;

    while (true) {
        syncmsg msg;
        if (!ReadFdExactly(sc.fd, &msg.dent, sizeof(msg.dent))) return false;

        if (msg.dent.id == ID_DONE) return true;
        if (msg.dent.id != ID_DENT) return false;

        size_t len = msg.dent.namelen;
        if (len > 256) return false; // TODO: resize buffer? continue?

        char buf[257];
        if (!ReadFdExactly(sc.fd, buf, len)) return false;
        buf[len] = 0;

        func(msg.dent.mode, msg.dent.size, msg.dent.time, buf);
    }
}

static bool sync_finish_stat(SyncConnection& sc, unsigned int* timestamp,
                             unsigned int* mode, unsigned int* size) {
    syncmsg msg;
    if (!ReadFdExactly(sc.fd, &msg.stat, sizeof(msg.stat)) || msg.stat.id != ID_STAT) {
        return false;
    }

    if (timestamp) *timestamp = msg.stat.time;
    if (mode) *mode = msg.stat.mode;
    if (size) *size = msg.stat.size;

    return true;
}

static bool sync_stat(SyncConnection& sc, const char* path,
                      unsigned int* timestamp, unsigned int* mode, unsigned int* size) {
    return sc.SendRequest(ID_STAT, path) && sync_finish_stat(sc, timestamp, mode, size);
}

static bool SendLargeFile(SyncConnection& sc, const char* path_and_mode,
                          const char* lpath, const char* rpath,
                          unsigned mtime) {
    if (!sc.SendRequest(ID_SEND, path_and_mode)) {
        sc.Error("failed to send ID_SEND message '%s': %s", path_and_mode, strerror(errno));
        return false;
    }

    struct stat st;
    if (stat(lpath, &st) == -1) {
        sc.Error("cannot stat '%s': %s", lpath, strerror(errno));
        return false;
    }

    uint64_t total_size = st.st_size;
    uint64_t bytes_copied = 0;

    int lfd = adb_open(lpath, O_RDONLY);
    if (lfd < 0) {
        sc.Error("cannot open '%s': %s", lpath, strerror(errno));
        return false;
    }

    syncsendbuf sbuf;
    sbuf.id = ID_DATA;
    while (true) {
        int ret = adb_read(lfd, sbuf.data, sc.max);
        if (ret <= 0) {
            if (ret < 0) {
                sc.Error("cannot read '%s': %s", lpath, strerror(errno));
                adb_close(lfd);
                return false;
            }
            break;
        }

        sbuf.size = ret;
        if (!WriteFdExactly(sc.fd, &sbuf, sizeof(unsigned) * 2 + ret)) {
            adb_close(lfd);
            return false;
        }
        sc.total_bytes += ret;

        bytes_copied += ret;

        int percentage = static_cast<int>(bytes_copied * 100 / total_size);
        sc.Printf("%s: %d%%", rpath, percentage);
    }

    adb_close(lfd);

    syncmsg msg;
    msg.data.id = ID_DONE;
    msg.data.size = mtime;
    if (!WriteFdExactly(sc.fd, &msg.data, sizeof(msg.data))) {
        sc.Error("failed to send ID_DONE message for '%s': %s", rpath, strerror(errno));
        return false;
    }

    return true;
}

static bool sync_send(SyncConnection& sc, const char* lpath, const char* rpath,
                      unsigned mtime, mode_t mode)
{
    std::string path_and_mode = android::base::StringPrintf("%s,%d", rpath, mode);

    if (S_ISLNK(mode)) {
#if !defined(_WIN32)
        char buf[PATH_MAX];
        ssize_t data_length = readlink(lpath, buf, PATH_MAX - 1);
        if (data_length == -1) {
            sc.Error("readlink '%s' failed: %s", lpath, strerror(errno));
            return false;
        }
        buf[data_length++] = '\0';

        if (!sc.SendSmallFile(path_and_mode.c_str(), rpath, buf, data_length, mtime)) return false;
        return sc.CopyDone(lpath, rpath);
#endif
    }

    if (!S_ISREG(mode)) {
        sc.Error("local file '%s' has unsupported mode: 0o%o", lpath, mode);
        return false;
    }

    struct stat st;
    if (stat(lpath, &st) == -1) {
        sc.Error("failed to stat local file '%s': %s", lpath, strerror(errno));
        return false;
    }
    if (st.st_size < SYNC_DATA_MAX) {
        std::string data;
        if (!android::base::ReadFileToString(lpath, &data)) {
            sc.Error("failed to read all of '%s': %s", lpath, strerror(errno));
            return false;
        }
        if (!sc.SendSmallFile(path_and_mode.c_str(), rpath, data.data(), data.size(), mtime)) {
            return false;
        }
    } else {
        if (!SendLargeFile(sc, path_and_mode.c_str(), lpath, rpath, mtime)) {
            return false;
        }
    }
    return sc.CopyDone(lpath, rpath);
}

static bool sync_recv(SyncConnection& sc, const char* rpath, const char* lpath) {
    sc.Print(rpath);

    unsigned size = 0;
    if (!sync_stat(sc, rpath, nullptr, nullptr, &size)) return false;

    if (!sc.SendRequest(ID_RECV, rpath)) return false;

    adb_unlink(lpath);
    mkdirs(lpath);
    int lfd = adb_creat(lpath, 0644);
    if (lfd < 0) {
        sc.Error("cannot create '%s': %s", lpath, strerror(errno));
        return false;
    }

    uint64_t bytes_copied = 0;
    while (true) {
        syncmsg msg;
        if (!ReadFdExactly(sc.fd, &msg.data, sizeof(msg.data))) {
            adb_close(lfd);
            adb_unlink(lpath);
            return false;
        }

        if (msg.data.id == ID_DONE) break;

        if (msg.data.id != ID_DATA) {
            adb_close(lfd);
            adb_unlink(lpath);
            sc.ReportCopyFailure(rpath, lpath, msg);
            return false;
        }

        if (msg.data.size > sc.max) {
            sc.Error("msg.data.size too large: %u (max %zu)", msg.data.size, sc.max);
            adb_close(lfd);
            adb_unlink(lpath);
            return false;
        }

        char buffer[SYNC_DATA_MAX];
        if (!ReadFdExactly(sc.fd, buffer, msg.data.size)) {
            adb_close(lfd);
            adb_unlink(lpath);
            return false;
        }

        if (!WriteFdExactly(lfd, buffer, msg.data.size)) {
            sc.Error("cannot write '%s': %s", lpath, strerror(errno));
            adb_close(lfd);
            adb_unlink(lpath);
            return false;
        }

        sc.total_bytes += msg.data.size;

        bytes_copied += msg.data.size;

        int percentage = static_cast<int>(bytes_copied * 100 / size);
        sc.Printf("%s: %d%%", rpath, percentage);
    }

    adb_close(lfd);
    return true;
}

bool do_sync_ls(const char* path) {
    SyncConnection sc;
    if (!sc.IsValid()) return false;

    return sync_ls(sc, path, [](unsigned mode, unsigned size, unsigned time,
                                const char* name) {
        printf("%08x %08x %08x %s\n", mode, size, time, name);
    });
}

struct copyinfo
{
    std::string src;
    std::string dst;
    unsigned int time;
    unsigned int mode;
    uint64_t size;
    int flag;
};

static copyinfo mkcopyinfo(const char* spath, const char* dpath, const char* name, bool isdir) {
    copyinfo result;
    result.src = android::base::StringPrintf(isdir ? "%s%s/" : "%s%s", spath, name);
    result.dst = android::base::StringPrintf(isdir ? "%s%s/" : "%s%s", dpath, name);
    result.time = 0;
    result.mode = 0;
    result.size = 0;
    result.flag = 0;
    return result;
}

static bool IsDotOrDotDot(const char* name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static int local_build_list(SyncConnection& sc, std::vector<copyinfo>* filelist,
                            const char* lpath, const char* rpath) {
    std::vector<copyinfo> dirlist;
    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(lpath), closedir);
    if (!dir) {
        sc.Error("cannot open '%s': %s", lpath, strerror(errno));
        return -1;
    }

    dirent* de;
    while ((de = readdir(dir.get()))) {
        if (IsDotOrDotDot(de->d_name)) continue;

        char stat_path[PATH_MAX];
        if (strlen(lpath) + strlen(de->d_name) + 1 > sizeof(stat_path)) {
            sc.Error("skipping long path '%s%s'", lpath, de->d_name);
            continue;
        }
        strcpy(stat_path, lpath);
        strcat(stat_path, de->d_name);

        struct stat st;
        if (!lstat(stat_path, &st)) {
            if (S_ISDIR(st.st_mode)) {
                dirlist.push_back(mkcopyinfo(lpath, rpath, de->d_name, 1));
            } else {
                if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                    sc.Error("skipping special file '%s'", lpath);
                } else {
                    copyinfo ci = mkcopyinfo(lpath, rpath, de->d_name, 0);
                    ci.time = st.st_mtime;
                    ci.mode = st.st_mode;
                    ci.size = st.st_size;
                    filelist->push_back(ci);
                }
            }
        } else {
            sc.Error("cannot lstat '%s': %s", stat_path, strerror(errno));
        }
    }

    // Close this directory and recurse.
    dir.reset();
    for (const copyinfo& ci : dirlist) {
        local_build_list(sc, filelist, ci.src.c_str(), ci.dst.c_str());
    }

    return 0;
}

static bool copy_local_dir_remote(SyncConnection& sc, const char* lpath, const char* rpath,
                                  bool check_timestamps, bool list_only) {
    std::vector<copyinfo> filelist;
    int pushed = 0;
    int skipped = 0;

    if ((lpath[0] == 0) || (rpath[0] == 0)) return false;
    if (lpath[strlen(lpath) - 1] != '/') {
        int  tmplen = strlen(lpath)+2;
        char *tmp = reinterpret_cast<char*>(malloc(tmplen));
        if(tmp == 0) return false;
        snprintf(tmp, tmplen, "%s/",lpath);
        lpath = tmp;
    }
    if (rpath[strlen(rpath) - 1] != '/') {
        int tmplen = strlen(rpath)+2;
        char *tmp = reinterpret_cast<char*>(malloc(tmplen));
        if(tmp == 0) return false;
        snprintf(tmp, tmplen, "%s/",rpath);
        rpath = tmp;
    }

    if (local_build_list(sc, &filelist, lpath, rpath)) {
        return false;
    }

    if (check_timestamps) {
        for (const copyinfo& ci : filelist) {
            if (!sc.SendRequest(ID_STAT, ci.dst.c_str())) return false;
        }
        for (copyinfo& ci : filelist) {
            unsigned int timestamp, mode, size;
            if (!sync_finish_stat(sc, &timestamp, &mode, &size)) return false;
            if (size == ci.size) {
                /* for links, we cannot update the atime/mtime */
                if ((S_ISREG(ci.mode & mode) && timestamp == ci.time) ||
                        (S_ISLNK(ci.mode & mode) && timestamp >= ci.time)) {
                    ci.flag = 1;
                }
            }
        }
    }

    for (const copyinfo& ci : filelist) {
        if (ci.flag == 0) {
            if (list_only) {
                fprintf(stderr, "would push: %s -> %s\n", ci.src.c_str(),
                        ci.dst.c_str());
            } else {
                if (!sync_send(sc, ci.src.c_str(), ci.dst.c_str(), ci.time, ci.mode)) {
                  return false;
                }
            }
            pushed++;
        } else {
            skipped++;
        }
    }

    sc.Printf("%s: %d file%s pushed. %d file%s skipped.%s\n", rpath, pushed,
              (pushed == 1) ? "" : "s", skipped, (skipped == 1) ? "" : "s",
              sc.TransferRate().c_str());
    return true;
}

bool do_sync_push(const std::vector<const char*>& srcs, const char* dst) {
    SyncConnection sc;
    if (!sc.IsValid()) return false;

    bool success = true;
    unsigned mode;
    if (!sync_stat(sc, dst, nullptr, &mode, nullptr)) return false;
    bool dst_isdir = mode != 0 && S_ISDIR(mode);

    if (!dst_isdir) {
        if (srcs.size() > 1) {
            sc.Error("target '%s' is not a directory", dst);
            return false;
        } else {
            size_t dst_len = strlen(dst);
            if (dst[dst_len - 1] == '/') {
                sc.Error("failed to access '%s': Not a directory", dst);
                return false;
            }
        }
    }

    for (const char* src_path : srcs) {
        const char* dst_path = dst;
        struct stat st;
        if (stat(src_path, &st)) {
            sc.Error("cannot stat '%s': %s", src_path, strerror(errno));
            success = false;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            success &= copy_local_dir_remote(sc, src_path, dst, false, false);
            continue;
        }

        std::string path_holder;
        if (mode != 0 && S_ISDIR(mode)) {
            // If we're copying a local file to a remote directory,
            // we really want to copy to remote_dir + "/" + local_filename.
            path_holder = android::base::StringPrintf(
                "%s/%s", dst_path, adb_basename(src_path).c_str());
            dst_path = path_holder.c_str();
        }
        success &= sync_send(sc, src_path, dst_path, st.st_mtime, st.st_mode);
    }

    sc.Print("\n");
    return success;
}

static bool remote_build_list(SyncConnection& sc,
                              std::vector<copyinfo>* filelist,
                              const char* rpath, const char* lpath) {
    std::vector<copyinfo> dirlist;

    // Put the files/dirs in rpath on the lists.
    auto callback = [&](unsigned mode, unsigned size, unsigned time,
                        const char* name) {
        if (S_ISDIR(mode)) {
            // Don't try recursing down "." or "..".
            if (IsDotOrDotDot(name)) return;

            dirlist.push_back(mkcopyinfo(rpath, lpath, name, 1));
        } else if (S_ISREG(mode) || S_ISLNK(mode)) {
            copyinfo ci = mkcopyinfo(rpath, lpath, name, 0);
            ci.time = time;
            ci.mode = mode;
            ci.size = size;
            filelist->push_back(ci);
        } else {
            sc.Print(android::base::StringPrintf("skipping special file '%s'\n",
                                                 name));
        }
    };

    if (!sync_ls(sc, rpath, callback)) {
        return false;
    }

    // Recurse into each directory we found.
    while (!dirlist.empty()) {
        copyinfo current = dirlist.back();
        dirlist.pop_back();
        if (!remote_build_list(sc, filelist, current.src.c_str(),
                               current.dst.c_str())) {
            return false;
        }
    }

    return true;
}

static int set_time_and_mode(const char *lpath, time_t time, unsigned int mode)
{
    struct utimbuf times = { time, time };
    int r1 = utime(lpath, &times);

    /* use umask for permissions */
    mode_t mask = umask(0000);
    umask(mask);
    int r2 = chmod(lpath, mode & ~mask);

    return r1 ? : r2;
}

static bool copy_remote_dir_local(SyncConnection& sc, const char* rpath, const char* lpath,
                                  bool copy_attrs) {
    // Make sure that both directory paths end in a slash.
    std::string rpath_clean(rpath);
    std::string lpath_clean(lpath);
    if (rpath_clean.empty() || lpath_clean.empty()) return false;
    if (rpath_clean.back() != '/') rpath_clean.push_back('/');
    if (lpath_clean.back() != '/') lpath_clean.push_back('/');

    // Recursively build the list of files to copy.
    sc.Print("pull: building file list...");
    std::vector<copyinfo> filelist;
    if (!remote_build_list(sc, &filelist, rpath_clean.c_str(),
                           lpath_clean.c_str())) {
        return false;
    }

    int pulled = 0;
    int skipped = 0;
    for (const copyinfo &ci : filelist) {
        if (ci.flag == 0) {
            sc.Printf("pull: %s -> %s", ci.src.c_str(), ci.dst.c_str());
            if (!sync_recv(sc, ci.src.c_str(), ci.dst.c_str())) {
                return false;
            }

            if (copy_attrs &&
                set_time_and_mode(ci.dst.c_str(), ci.time, ci.mode)) {
                return false;
            }
            pulled++;
        } else {
            skipped++;
        }
    }

    sc.Printf("%s: %d file%s pulled. %d file%s skipped.%s\n", rpath, pulled,
              (pulled == 1) ? "" : "s", skipped, (skipped == 1) ? "" : "s",
              sc.TransferRate().c_str());
    return true;
}

bool do_sync_pull(const std::vector<const char*>& srcs, const char* dst,
                  bool copy_attrs) {
    SyncConnection sc;
    if (!sc.IsValid()) return false;

    bool success = true;
    unsigned mode, time;
    struct stat st;
    if (stat(dst, &st)) {
        // If we're only pulling one file, the destination path might point to
        // a path that doesn't exist yet.
        if (srcs.size() != 1 || errno != ENOENT) {
            sc.Error("cannot stat '%s': %s", dst, strerror(errno));
            return false;
        }
    }

    bool dst_isdir = S_ISDIR(st.st_mode);
    if (!dst_isdir) {
        if (srcs.size() > 1) {
            sc.Error("target '%s' is not a directory", dst);
            return false;
        } else {
            size_t dst_len = strlen(dst);
            if (dst[dst_len - 1] == '/') {
                sc.Error("failed to access '%s': Not a directory", dst);
                return false;
            }
        }
    }

    for (const char* src_path : srcs) {
        const char* dst_path = dst;
        if (!sync_stat(sc, src_path, &time, &mode, nullptr)) return false;
        if (mode == 0) {
            sc.Error("remote object '%s' does not exist", src_path);
            success = false;
            continue;
        }

        if (S_ISREG(mode) || S_ISLNK(mode) || S_ISCHR(mode) || S_ISBLK(mode)) {
            std::string path_holder;
            struct stat st;
            if (stat(dst_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    // If we're copying a remote file to a local directory,
                    // we really want to copy to local_dir + "/" +
                    // basename(remote).
                    path_holder = android::base::StringPrintf(
                        "%s/%s", dst_path, adb_basename(src_path).c_str());
                    dst_path = path_holder.c_str();
                }
            }
            if (!sync_recv(sc, src_path, dst_path)) {
                success = false;
                continue;
            } else {
                if (copy_attrs && set_time_and_mode(dst_path, time, mode)) {
                    success = false;
                    continue;
                }
            }
        } else if (S_ISDIR(mode)) {
            success &= copy_remote_dir_local(sc, src_path, dst_path, copy_attrs);
            continue;
        } else {
            sc.Error("remote object '%s' not a file or directory", src_path);
            success = false;
            continue;
        }
    }

    sc.Print("\n");
    return success;
}

bool do_sync_sync(const std::string& lpath, const std::string& rpath, bool list_only) {
    SyncConnection sc;
    if (!sc.IsValid()) return false;

    return copy_local_dir_remote(sc, lpath.c_str(), rpath.c_str(), true, list_only);
}
