// See config.h.

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cpupower
{

namespace
{

// A preferences file is four lines. Anything appreciably larger is not one, and is not read.
constexpr size_t kMaxConfigBytes = 4096;
constexpr size_t kMaxLine = 128;

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

} // namespace

//------------------------------------------------------------------------
std::string configPath()
{
    if (const char *xdg = ::getenv("XDG_CONFIG_HOME")) {
        if (xdg[0] == '/')
            return std::string(xdg) + "/cpu-power/config";
    }
    if (const char *home = ::getenv("HOME")) {
        if (home[0] == '/')
            return std::string(home) + "/.config/cpu-power/config";
    }
    return std::string();
}

//------------------------------------------------------------------------
Config loadConfig()
{
    Config c;

    const std::string path = configPath();
    if (path.empty())
        return c;

    FILE *f = ::fopen(path.c_str(), "re");
    if (!f)
        return c;

    std::string all;
    char buf[512];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(buf, n);
        if (all.size() > kMaxConfigBytes)
            break;
    }
    ::fclose(f);

    if (all.size() > kMaxConfigBytes) {
        ::fprintf(stderr, "cpu-power: %s is larger than a preferences file should be; ignoring\n",
                  path.c_str());
        return Config();
    }

    size_t pos = 0;
    while (pos < all.size()) {
        const size_t nl = all.find('\n', pos);
        const std::string raw = all.substr(pos, (nl == std::string::npos ? all.size() : nl) - pos);
        pos = (nl == std::string::npos) ? all.size() : nl + 1;

        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line.size() > kMaxLine)
            continue;

        const size_t sp = line.find(' ');
        if (sp == std::string::npos)
            continue;
        const std::string key = line.substr(0, sp);
        const std::string value = trim(line.substr(sp + 1));
        if (value.empty())
            continue;

        if (key == "stop") {
            // stopFromKey rejects rather than defaults. A drifted config must not select a
            // different power level than the one it names.
            Stop s;
            if (stopFromKey(value, s))
                c.stop = s;
            else
                ::fprintf(stderr, "cpu-power: %s: unknown stop \"%s\"; using the default\n",
                          path.c_str(), value.c_str());
        } else if (key == "turbo") {
            c.turbo = (value == "on");
        } else if (key == "daw") {
            c.daw = (value == "on");
        } else if (key == "scale") {
            char *end = nullptr;
            errno = 0;
            const double v = ::strtod(value.c_str(), &end);
            if (errno == 0 && end && *end == '\0' && v >= 0.25 && v <= 4.0)
                c.scale = static_cast<float>(v);
        }
        // Any other key is ignored in silence: a file written by a later version must not make
        // this one complain on every launch.
    }

    return c;
}

//------------------------------------------------------------------------
bool saveConfig(const Config &c)
{
    const std::string path = configPath();
    if (path.empty())
        return false;

    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        const std::string dir = path.substr(0, slash);
        // The parent ($XDG_CONFIG_HOME or ~/.config) is expected to exist already; only our own
        // directory inside it is created.
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            ::fprintf(stderr, "cpu-power: cannot create %s: %s\n", dir.c_str(), ::strerror(errno));
            return false;
        }
    }

    FILE *f = ::fopen(path.c_str(), "we");
    if (!f) {
        ::fprintf(stderr, "cpu-power: cannot write %s: %s\n", path.c_str(), ::strerror(errno));
        return false;
    }

    const int wrote = ::fprintf(f,
                                "# cpu-power preferences.\n"
                                "#\n"
                                "# These are NOT applied at startup: they set where the controls\n"
                                "# sit, and nothing changes until the Active toggle is turned on.\n"
                                "# To apply a stop at boot, put a line like\n"
                                "#     cpu-power-cli --stop %s --hold\n"
                                "# in an init script -- the settings last exactly as long as that\n"
                                "# process does.\n"
                                "stop %s\n"
                                "turbo %s\n"
                                "daw %s\n"
                                "scale %.2f\n",
                                stopKey(c.stop), stopKey(c.stop), c.turbo ? "on" : "off",
                                c.daw ? "on" : "off", static_cast<double>(c.scale));

    const bool ok = (wrote > 0) && (::fclose(f) == 0);
    if (!ok)
        ::fprintf(stderr, "cpu-power: failed to write %s\n", path.c_str());
    return ok;
}

} // namespace cpupower
