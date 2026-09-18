// Sysfs implementation. See sysfs.h.

#include "sysfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace cpupower
{

//------------------------------------------------------------------------
Sysfs::Sysfs(std::string root) : mRoot(std::move(root))
{
    // Normalise away a trailing slash so absolute() never produces a double one. A root of "/" is
    // left alone, since stripping it would produce a relative path.
    while (mRoot.size() > 1 && mRoot.back() == '/')
        mRoot.pop_back();
}

//------------------------------------------------------------------------
std::string Sysfs::absolute(const std::string &rel) const
{
    if (mRoot == "/")
        return "/" + rel;
    return mRoot + "/" + rel;
}

//------------------------------------------------------------------------
std::optional<std::string> Sysfs::read(const std::string &rel) const
{
    const std::string path = absolute(rel);

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;

    std::string out;
    out.resize(kMaxAttr);
    size_t total = 0;

    while (total < kMaxAttr) {
        const ssize_t n = ::read(fd, &out[total], kMaxAttr - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            // A sysfs attribute can fail at read() even though open() succeeded — a write-only
            // knob, or one whose backing device has gone away. Absent and unreadable are the same
            // answer to the caller: this machine does not offer it.
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0)
            break;
        total += static_cast<size_t>(n);
    }
    ::close(fd);

    out.resize(total);

    // sysfs terminates almost every attribute with exactly one newline. Strip one, not all: an
    // attribute whose value legitimately ends in a blank line would otherwise be altered.
    if (!out.empty() && out.back() == '\n')
        out.pop_back();

    return out;
}

//------------------------------------------------------------------------
std::optional<long long> Sysfs::readInt(const std::string &rel) const
{
    const std::optional<std::string> text = read(rel);
    if (!text || text->empty())
        return std::nullopt;

    // strtoll plus a full check of errno, the end pointer and any trailing bytes. This is what
    // rejects intel_pstate's "<unsupported>" (scaling_setspeed) rather than silently reading it
    // as 0 — a value that would look entirely plausible and be entirely wrong.
    errno = 0;
    char *end = nullptr;
    const long long value = std::strtoll(text->c_str(), &end, 10);

    if (errno != 0 || end == text->c_str())
        return std::nullopt;

    // Trailing whitespace is tolerated; trailing anything else is not.
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
        ++end;
    if (*end != '\0')
        return std::nullopt;

    return value;
}

//------------------------------------------------------------------------
std::vector<std::string> Sysfs::readList(const std::string &rel) const
{
    std::vector<std::string> out;

    const std::optional<std::string> text = read(rel);
    if (!text)
        return out;

    // The kernel writes these space-separated with a trailing space, so a naive split on ' '
    // yields an empty final token. Skipping runs of whitespace handles that and tabs and newlines.
    size_t i = 0;
    while (i < text->size()) {
        while (i < text->size() && std::isspace(static_cast<unsigned char>((*text)[i])))
            ++i;
        const size_t start = i;
        while (i < text->size() && !std::isspace(static_cast<unsigned char>((*text)[i])))
            ++i;
        if (i > start)
            out.push_back(text->substr(start, i - start));
    }

    return out;
}

//------------------------------------------------------------------------
bool Sysfs::exists(const std::string &rel) const
{
    struct stat st = {};
    if (::stat(absolute(rel).c_str(), &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode))
        return false;
    return ::access(absolute(rel).c_str(), R_OK) == 0;
}

//------------------------------------------------------------------------
std::vector<std::string> Sysfs::listDirs(const std::string &rel) const
{
    std::vector<std::string> out;

    DIR *dir = ::opendir(absolute(rel).c_str());
    if (!dir)
        return out;

    while (const struct dirent *ent = ::readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0)
            continue;

        // d_type is DT_UNKNOWN on some filesystems, so fall back to stat rather than trusting it.
        // A fixture may live on any filesystem the user happens to have checked the repo out on.
        bool isDir = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            struct stat st = {};
            if (::stat(absolute(rel + "/" + ent->d_name).c_str(), &st) == 0)
                isDir = S_ISDIR(st.st_mode);
        }
        if (isDir)
            out.emplace_back(ent->d_name);
    }
    ::closedir(dir);

    std::sort(out.begin(), out.end());
    return out;
}

//------------------------------------------------------------------------
std::vector<int> parseCpuList(const std::string &list)
{
    std::vector<int> out;

    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() &&
               (std::isspace(static_cast<unsigned char>(list[i])) || list[i] == ','))
            ++i;
        if (i >= list.size())
            break;

        // Each element is either "N" or "N-M". Anything else means a kernel format we do not
        // understand, and the honest answer is to stop rather than to guess at the remainder.
        char *end = nullptr;
        errno = 0;
        const long lo = std::strtol(list.c_str() + i, &end, 10);
        if (errno != 0 || end == list.c_str() + i || lo < 0)
            break;
        i = static_cast<size_t>(end - list.c_str());

        long hi = lo;
        if (i < list.size() && list[i] == '-') {
            ++i;
            errno = 0;
            hi = std::strtol(list.c_str() + i, &end, 10);
            if (errno != 0 || end == list.c_str() + i || hi < lo)
                break;
            i = static_cast<size_t>(end - list.c_str());
        }

        // A malformed range must not be able to make this allocate without bound.
        if (hi - lo > 65535)
            break;

        for (long c = lo; c <= hi; ++c)
            out.push_back(static_cast<int>(c));
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace cpupower
