/*
 * cpu-power-helper — the ONLY part of this project that runs as root.
 *
 * It is plain C against libc and nothing else. That is deliberate: there is less of it to audit,
 * and there is no library that could carry a surprise into a privileged process.
 *
 * ==========================================================================================
 * THE CONTRACT. Every line of this file is bound by it.
 * ==========================================================================================
 *
 *   * NO ARGUMENT IS EVER A PATH, and no argument is ever concatenated into one. The protocol
 *     carries a VERB plus an integer policy index. The attribute name is a compiled-in string
 *     literal selected by the verb, so the only variable component of any path we open is an
 *     integer that has been range-checked against the policies we discovered ourselves.
 *
 *   * NAMES ARE VALIDATED AGAINST THE KERNEL'S OWN LIST, re-read at the moment of use — never
 *     against a table compiled in here. If the kernel does not list a governor, we do not write
 *     it, whatever the caller says and whatever this file thinks it knows.
 *
 *   * ONE DIRFD, HELD FOR THE PROCESS LIFETIME. It is opened O_DIRECTORY|O_NOFOLLOW and proved
 *     to be sysfs with fstatfs(). Every later access is an openat() anchored to it, so the mount
 *     cannot be swapped underneath us mid-session.
 *
 *   * NO exec, NO shell, NO network, NO config file, NO getenv. PR_SET_NO_NEW_PRIVS makes the
 *     first of those structural rather than a promise. The one socket this process listens on
 *     is a local rendezvous for re-attaching, reachable only by the uid that started the
 *     session and answering nothing but the same closed protocol.
 *
 *   * RESTORE ON EVERY UNPLANNED EXIT PATH. EOF without a preceding DETACH, SIGTERM, SIGINT,
 *     SIGHUP. The GUI being SIGKILLed still closes the channel, so a client that dies badly
 *     always puts the machine back, and no path depends on an orderly shutdown.
 *
 *     DETACH is the single exception and it is EXPLICIT, which is the whole of its safety: a
 *     crash cannot be mistaken for a request to persist, because a crash cannot send a verb.
 *     After it, this process outlives the window on purpose and exits when the settings are
 *     switched off instead -- so a root process exists exactly while the machine is overridden.
 *     That is a real weakening of "this cannot outlive the GUI", taken deliberately: the PM-QoS
 *     latency request cannot outlive this process by any means (pm_qos_interface.rst:71-78), so
 *     the alternative was not a safer tool but a DAW mode that ends when you close the window.
 *
 *   * STDOUT CARRIES PROTOCOL RESPONSES AND NOTHING ELSE. Every diagnostic goes to stderr. A
 *     stray byte on fd 1 desynchronises the protocol.
 *
 * ==========================================================================================
 * TWO BUILDS FROM THIS ONE SOURCE
 * ==========================================================================================
 *
 *   cpu-power-helper        installed, privileged. CPUPOWER_TEST_BUILD is NOT defined, so the
 *                           --sysroot option DOES NOT EXIST IN THE BINARY. An option that is not
 *                           compiled in cannot be abused, which is stronger than validating it.
 *
 *   cpu-power-helper-test   never installed, never privileged. --sysroot only. Used by the gates
 *                           to drive the protocol against captured fixtures.
 *
 * The two are mutually exclusive by construction: the test build refuses to run as root, and the
 * privileged build has no way to be pointed anywhere but /sys.
 *
 * ==========================================================================================
 * PROTOCOL — newline-delimited, at most MAX_LINE bytes per line.
 * ==========================================================================================
 *
 *   SET gov   <policy> <name>     stage a governor write
 *   SET epp   <policy> <name>     stage an energy-performance-preference write
 *   SET turbo <0|1>               stage turbo INTENT (1 = turbo enabled). This process owns the
 *                                 inversion and the choice of knob; the raw value never crosses
 *                                 the boundary.
 *   COMMIT                        apply everything staged, IN THE ORDER IT WAS STAGED
 *   SNAPSHOT                      report the machine's current state
 *   LATENCY <us> | LATENCY OFF    /dev/cpu_dma_latency PM-QoS hold
 *   RESTORE                       put back what was captured at startup
 *   DETACH                        keep everything applied and outlive this client; the reply is
 *                                 "OK <socket>", which is where the next client re-attaches
 *   QUIT                          restore and exit
 *
 * Responses: "OK", "OK <detail>", or "ERR <reason>". Never anything else.
 *
 * ORDERING IS THE CALLER'S JOB, NOT OURS. COMMIT applies staged writes in the order received.
 * The kernel's ordering constraints (chiefly: EPP writes are rejected while the governor is
 * `performance`) are encoded once, in src/core/profile.cpp, where tools/coretest asserts them
 * against captured sysfs trees from five machine shapes. Duplicating that logic here would give
 * it two homes and one of them would drift. This process validates and executes; it does not
 * second-guess the order.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/magic.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------------------- */
/* Bounds. Every one of these is a refusal point, not a truncation point: input that exceeds a
 * limit is rejected with an error, never silently cut down to fit. */

#define MAX_LINE 256      /* protocol line, including the newline */
#define MAX_VALUE 64      /* a governor or EPP name */
#define MAX_POLICIES 1024 /* generous; a 512-core machine has 512 */
#define MAX_STAGED 4096   /* two writes per policy plus turbo, with room to spare */
#define MAX_ATTR 4096     /* a sysfs attribute read */
#define MAX_PATH_REL 128  /* "cpufreq/policy1023/energy_performance_available_preferences" */

/* Exit codes, distinguished so the GUI can say something useful rather than "it failed". */
#define EX_OK_ 0
#define EX_NOPERM_ 77
#define EX_USAGE_ 64
#define EX_PROTO_ 65
#define EX_ENV_ 78

/* ---------------------------------------------------------------------------------------- */

enum turbo_knob {
    TURBO_NONE = 0,
    TURBO_INTEL_NO_TURBO, /* intel_pstate/no_turbo  -- INVERTED */
    TURBO_GLOBAL_BOOST,   /* cpufreq/boost */
    TURBO_POLICY_BOOST,   /* policyN/boost */
    TURBO_POLICY_CPB      /* policyN/cpb */
};

enum op { OP_GOV = 0, OP_EPP, OP_TURBO };

struct staged {
    enum op op;
    int policy; /* -1 for a machine-global knob */
    char value[MAX_VALUE];
};

struct policy_state {
    int index;
    char governor[MAX_VALUE];
    char epp[MAX_VALUE];   /* empty if this machine has no EPP */
    char turbo[MAX_VALUE]; /* raw per-policy turbo value, empty if not per-policy */
};

static int g_cpufd = -1; /* dirfd on /sys/devices/system/cpu, held for the process lifetime */
static int g_latency_fd = -1;
static long g_latency_us = -1; /* the value currently held, or -1 for none */

/* The protocol channel. Not hardwired to stdin/stdout any more: after DETACH the original
 * channel is gone and a re-attaching client arrives on an accepted socket instead. Everything
 * that reads or writes protocol goes through these two. */
static int g_in_fd = STDIN_FILENO;
static int g_out_fd = STDOUT_FILENO;

/* Residency. g_detached says the GUI asked us to outlive it; g_listen_fd is how the next one
 * finds us again; g_owner_uid is the ONLY uid allowed to do so. */
static int g_detached = 0;
static int g_listen_fd = -1;
static uid_t g_owner_uid = (uid_t)-1;
static char g_sock_path[108]; /* sun_path is 108 bytes and that is the real limit */
static enum turbo_knob g_turbo = TURBO_NONE;
static char g_turbo_global[MAX_VALUE]; /* raw value at startup, for a global knob */

static struct policy_state g_snapshot[MAX_POLICIES];
static int g_npolicies = 0;

static struct staged g_staged[MAX_STAGED];
static int g_nstaged = 0;

static volatile sig_atomic_t g_signalled = 0;

#ifdef CPUPOWER_TEST_BUILD
static const char *g_sysroot = NULL;
#endif

/* ---------------------------------------------------------------------------------------- */
/* Output. stdout is the protocol channel and carries nothing else; stderr is for humans.
 * Both use a full-write loop, because write() may be partial and a short write here would
 * corrupt the protocol rather than merely lose a message. */

static void full_write(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return; /* the peer is gone; there is nowhere left to report that to */
        }
        if (n == 0)
            return;
        off += (size_t)n;
    }
}

static void say(const char *fmt, ...)
{
    char buf[MAX_ATTR];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0 || g_out_fd < 0)
        return;
    full_write(g_out_fd, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    full_write(g_out_fd, "\n", 1);
}

static void warn_(const char *fmt, ...)
{
    char buf[MAX_ATTR];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    full_write(STDERR_FILENO, "cpu-power-helper: ", 18);
    full_write(STDERR_FILENO, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    full_write(STDERR_FILENO, "\n", 1);
}

/* ---------------------------------------------------------------------------------------- */
/* sysfs access, all anchored to the held dirfd. */

/* Build "cpufreq/policyN/<attr>". attr is ALWAYS a compiled-in literal chosen by a verb; the
 * only variable component is an integer that has already been matched against a discovered
 * policy. Returns 0 on success, -1 if it would not fit (which cannot happen with the literals
 * this file uses, and is checked anyway). */
static int policy_rel(char *out, size_t outlen, int index, const char *attr)
{
    if (index < 0 || index > 65535)
        return -1;
    const int n = snprintf(out, outlen, "cpufreq/policy%d/%s", index, attr);
    return (n > 0 && (size_t)n < outlen) ? 0 : -1;
}

static int read_rel(const char *rel, char *buf, size_t buflen)
{
    const int fd = openat(g_cpufd, rel, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;

    size_t total = 0;
    while (total + 1 < buflen) {
        const ssize_t n = read(fd, buf + total, buflen - 1 - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }
    close(fd);

    buf[total] = '\0';
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
        buf[--total] = '\0';
    return 0;
}

static int write_rel(const char *rel, const char *value)
{
    const int fd = openat(g_cpufd, rel, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;

    const size_t len = strlen(value);
    ssize_t n;
    do {
        n = write(fd, value, len);
    } while (n < 0 && errno == EINTR);

    /* Drop anything the previous, longer value left behind.
     *
     * On real sysfs this is a no-op and the open above deliberately matches what the kernel's own
     * cpupower does byte for byte (plain O_WRONLY, no O_TRUNC -- see
     * third_party/cpupower/lib/cpufreq.c:46): a sysfs store handler is given the buffer and its
     * length, so there is no residual tail to worry about.
     *
     * On a REGULAR file there certainly is, and that is not hypothetical: the fixtures are regular
     * files, and without this, writing the 9-byte "powersave" over the 11-byte "performance" left
     * "powersavece" behind. That corrupted the fixture the next assertion would read, which makes
     * every test downstream of it meaningless -- a far worse failure than the one it looks like.
     *
     * ftruncate rather than O_TRUNC on the open, so the sysfs path keeps the exact flags the
     * reference implementation uses and this stays a best-effort tidy-up whose failure is
     * ignored, rather than a change to how the privileged write itself is performed. */
    if (n > 0) {
        /* Deliberately ignored: it fails harmlessly on sysfs, where there is nothing to trim. */
        const int trimmed = ftruncate(fd, n);
        (void)trimmed;
    }

    const int saved = errno;
    close(fd);

    if (n < 0 || (size_t)n != len) {
        errno = saved;
        return -1;
    }
    return 0;
}

/* Is `needle` one of the whitespace-separated tokens in the attribute at `rel`?
 *
 * This is THE validation primitive. Every governor and EPP name we write has passed through it
 * against the kernel's own list, re-read at the moment of use. */
static int in_kernel_list(const char *rel, const char *needle)
{
    char buf[MAX_ATTR];
    if (read_rel(rel, buf, sizeof(buf)) != 0)
        return 0;

    const size_t nlen = strlen(needle);
    if (nlen == 0)
        return 0;

    const char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            ++p;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            ++p;
        const size_t len = (size_t)(p - start);
        if (len == nlen && memcmp(start, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

/* Copy into a bounded destination, REFUSING rather than truncating.
 *
 * snprintf() would silently cut an over-long value down to fit, and a TRUNCATED governor name is
 * precisely the silent wrongness this project forbids: it would either fail validation for a
 * reason nobody could see, or -- worse -- match something shorter. A value that does not fit in
 * MAX_VALUE is not a governor or EPP name, so the honest answer is "unreadable", which every
 * caller already handles by leaving its destination empty. */
static int copy_bounded(char *dst, size_t dstlen, const char *src)
{
    const size_t len = strlen(src);
    if (len >= dstlen)
        return -1;
    memcpy(dst, src, len + 1);
    return 0;
}

static int rel_exists(const char *rel)
{
    struct stat st;
    if (fstatat(g_cpufd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return 0;
    return S_ISREG(st.st_mode);
}

/* ---------------------------------------------------------------------------------------- */
/* Parsing. Every integer goes through here: strtol plus a full check of errno, the end pointer
 * and any trailing bytes. "12x", "", " ", "0x10", "+5" and a value that overflows are all
 * rejected rather than silently accepted as something plausible. */
static int parse_int(const char *s, long lo, long hi, long *out)
{
    if (!s || !*s)
        return -1;
    if (!((*s >= '0' && *s <= '9') || *s == '-'))
        return -1; /* no leading '+', no whitespace, no "0x" */
    if (s[0] == '0' && s[1] != '\0')
        return -1; /* no leading zeros: "010" must not read as 10 or as octal 8 */

    errno = 0;
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

/* A governor or EPP name from the protocol. Rejected unless it is a short run of the characters
 * the kernel actually uses for these names. This runs BEFORE the name is compared against the
 * kernel's list, so that nothing containing a slash or a dot can reach a comparison at all. */
static int valid_name(const char *s)
{
    const size_t len = strlen(s);
    if (len == 0 || len >= MAX_VALUE)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = s[i];
        const int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-';
        if (!ok)
            return 0;
    }
    return 1;
}

static int policy_known(int index)
{
    for (int i = 0; i < g_npolicies; ++i) {
        if (g_snapshot[i].index == index)
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Discovery. This process does its OWN capability probe rather than trusting the caller to
 * describe the machine — the caller is not trusted, and the machine is the authority. */

static int cmp_int(const void *a, const void *b)
{
    const int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int discover(void)
{
    const int fd = openat(g_cpufd, "cpufreq", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        warn_("no cpufreq interface: %s", strerror(errno));
        return -1;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return -1;
    }

    int indices[MAX_POLICIES];
    int n = 0;
    const struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < MAX_POLICIES) {
        if (strncmp(ent->d_name, "policy", 6) != 0)
            continue;
        long idx;
        if (parse_int(ent->d_name + 6, 0, 65535, &idx) != 0)
            continue;

        char rel[MAX_PATH_REL];
        if (policy_rel(rel, sizeof(rel), (int)idx, "scaling_driver") != 0)
            continue;
        if (!rel_exists(rel))
            continue;

        indices[n++] = (int)idx;
    }
    closedir(dir);

    if (n == 0) {
        warn_("no cpufreq policies found");
        return -1;
    }

    /* Numerically. Directory order is arbitrary, and a string sort puts policy10 before policy9,
     * which would make every report this feeds look scrambled. */
    qsort(indices, (size_t)n, sizeof(indices[0]), cmp_int);

    for (int i = 0; i < n; ++i)
        g_snapshot[i].index = indices[i];
    g_npolicies = n;

    /* Turbo, in the order the kernel documentation fixes. See src/core/backend.h for the
     * quotations; the order must match that file, and scripts/sec-gate.sh checks that it does. */
    if (rel_exists("intel_pstate/no_turbo"))
        g_turbo = TURBO_INTEL_NO_TURBO;
    else if (rel_exists("cpufreq/boost"))
        g_turbo = TURBO_GLOBAL_BOOST;
    else {
        char rel[MAX_PATH_REL];
        if (policy_rel(rel, sizeof(rel), g_snapshot[0].index, "boost") == 0 && rel_exists(rel))
            g_turbo = TURBO_POLICY_BOOST;
        else if (policy_rel(rel, sizeof(rel), g_snapshot[0].index, "cpb") == 0 && rel_exists(rel))
            g_turbo = TURBO_POLICY_CPB;
        else
            g_turbo = TURBO_NONE;
    }
    return 0;
}

static const char *turbo_global_rel(void)
{
    switch (g_turbo) {
        case TURBO_INTEL_NO_TURBO:
            return "intel_pstate/no_turbo";
        case TURBO_GLOBAL_BOOST:
            return "cpufreq/boost";
        default:
            return NULL;
    }
}

static const char *turbo_policy_attr(void)
{
    switch (g_turbo) {
        case TURBO_POLICY_BOOST:
            return "boost";
        case TURBO_POLICY_CPB:
            return "cpb";
        default:
            return NULL;
    }
}

static const char *turbo_name(void)
{
    switch (g_turbo) {
        case TURBO_NONE:
            return "none";
        case TURBO_INTEL_NO_TURBO:
            return "intel_pstate/no_turbo";
        case TURBO_GLOBAL_BOOST:
            return "cpufreq/boost";
        case TURBO_POLICY_BOOST:
            return "policy/boost";
        case TURBO_POLICY_CPB:
            return "policy/cpb";
    }
    return "none";
}

/* intel_pstate's no_turbo is inverted: 1 means turbo OFF. Resolved in exactly one place, here,
 * so the protocol can carry intent and nothing else has to know. */
static const char *turbo_raw_for(int intent_on)
{
    if (g_turbo == TURBO_INTEL_NO_TURBO)
        return intent_on ? "0" : "1";
    return intent_on ? "1" : "0";
}

static void snapshot_state(void)
{
    char rel[MAX_PATH_REL];
    char buf[MAX_ATTR];

    for (int i = 0; i < g_npolicies; ++i) {
        struct policy_state *p = &g_snapshot[i];
        p->governor[0] = p->epp[0] = p->turbo[0] = '\0';

        if (policy_rel(rel, sizeof(rel), p->index, "scaling_governor") == 0 &&
            read_rel(rel, buf, sizeof(buf)) == 0)
            (void)copy_bounded(p->governor, sizeof(p->governor), buf);

        if (policy_rel(rel, sizeof(rel), p->index, "energy_performance_preference") == 0 &&
            read_rel(rel, buf, sizeof(buf)) == 0)
            (void)copy_bounded(p->epp, sizeof(p->epp), buf);

        const char *attr = turbo_policy_attr();
        if (attr && policy_rel(rel, sizeof(rel), p->index, attr) == 0 &&
            read_rel(rel, buf, sizeof(buf)) == 0)
            (void)copy_bounded(p->turbo, sizeof(p->turbo), buf);
    }

    g_turbo_global[0] = '\0';
    const char *grel = turbo_global_rel();
    if (grel && read_rel(grel, buf, sizeof(buf)) == 0)
        (void)copy_bounded(g_turbo_global, sizeof(g_turbo_global), buf);
}

/* ---------------------------------------------------------------------------------------- */
/* Restore. Best-effort by design: it reports what it could not put back
 * and keeps going, rather than aborting half way and leaving the machine in a third state that
 * is neither where it started nor where it was asked to be.
 *
 * The order mirrors apply, and for the same reason: turbo, then governor, then EPP. An EPP write
 * is skipped when the governor being restored is `performance`, because the driver rejects it in
 * that configuration (intel_pstate.rst:130-133) — attempting it would produce a guaranteed error
 * on every restore from the Maximum setting. */
/* Write only if the current value actually differs.
 *
 * This is not merely tidiness. A redundant write to sysfs is NOT a no-op: writing
 * scaling_governor re-initialises the governor even when the value is unchanged. Restore should
 * put back what was changed and touch nothing else, so a session that changed nothing leaves the
 * machine genuinely untouched rather than merely equal. */
static int restore_attr(const char *rel, const char *want, int *failures)
{
    char cur[MAX_ATTR];
    if (read_rel(rel, cur, sizeof(cur)) == 0 && strcmp(cur, want) == 0)
        return 0; /* already correct; do not touch it */
    if (write_rel(rel, want) != 0) {
        warn_("restore: %s <- \"%s\": %s", rel, want, strerror(errno));
        ++(*failures);
        return -1;
    }
    return 0;
}

static int restore_state(void)
{
    int failures = 0;
    char rel[MAX_PATH_REL];

    const char *grel = turbo_global_rel();
    if (grel && g_turbo_global[0])
        (void)restore_attr(grel, g_turbo_global, &failures);

    const char *tattr = turbo_policy_attr();
    if (tattr) {
        for (int i = 0; i < g_npolicies; ++i) {
            if (!g_snapshot[i].turbo[0])
                continue;
            if (policy_rel(rel, sizeof(rel), g_snapshot[i].index, tattr) == 0)
                (void)restore_attr(rel, g_snapshot[i].turbo, &failures);
        }
    }

    for (int i = 0; i < g_npolicies; ++i) {
        if (!g_snapshot[i].governor[0])
            continue;
        if (policy_rel(rel, sizeof(rel), g_snapshot[i].index, "scaling_governor") == 0)
            (void)restore_attr(rel, g_snapshot[i].governor, &failures);
    }

    for (int i = 0; i < g_npolicies; ++i) {
        if (!g_snapshot[i].epp[0])
            continue;
        if (strcmp(g_snapshot[i].governor, "performance") == 0)
            continue; /* the driver forces EPP here and rejects the write */
        if (policy_rel(rel, sizeof(rel), g_snapshot[i].index, "energy_performance_preference") == 0)
            (void)restore_attr(rel, g_snapshot[i].epp, &failures);
    }

    return failures;
}

static void release_latency(void)
{
    if (g_latency_fd >= 0) {
        close(g_latency_fd);
        g_latency_fd = -1;
    }
    g_latency_us = -1;
}

/* ---------------------------------------------------------------------------------------- */
/* RESIDENCY AND RE-ATTACH.
 *
 * The GUI is a window you open to change a setting and then close. The settings have to outlive
 * it -- that is the whole point of the tool -- and one of them CANNOT outlive this process no
 * matter what we do: the PM-QoS latency request exists for exactly as long as the descriptor is
 * open. "As long as the device node is held open that process has a registered request on the
 * parameter [...] To remove the user mode request for a target value simply close the device
 * node." (pm_qos_interface.rst:71-78.) So either something stays resident or DAW mode ends when
 * the window does.
 *
 * Hence DETACH: the GUI says "keep going without me", and this process stops restoring on EOF
 * and starts listening instead. It exits when the settings are switched off, not when the
 * window closes -- so a root process exists exactly while the machine is overridden, and never
 * otherwise.
 *
 * WHY THE SNAPSHOT IS WHY WE STAY. It would be tempting to exit as soon as nothing needs an open
 * descriptor, since a governor write is just sysfs and persists on its own. That would be wrong:
 * this process is also holding the state the machine had BEFORE any of this, which is what
 * "off" puts back. Exit while the slider is still applied and the next session snapshots the
 * ALREADY-MODIFIED machine as its baseline -- after which switching off restores what is already
 * there and silently does nothing. A control that appears to work and does not is the one
 * outcome this design refuses everywhere else, so it is refused here too.
 *
 * WHO MAY RE-ATTACH. The listening socket is the only way back in to a root process, so the
 * question "who is on the other end" is answered by the kernel, not by anything the peer says.
 * SO_PEERCRED on the accepted connection gives credentials the kernel recorded at connect time;
 * they cannot be forged by the peer and nothing in the protocol can influence them. The same
 * call on the ORIGINAL channel is where the owning uid comes from in the first place: the GUI
 * creates the socketpair, so the kernel has already recorded who it belongs to. No environment
 * variable is consulted for this. pkexec does export PKEXEC_UID, and it is honest -- it is set
 * from pkexec's own getuid() after the environment has been cleared (pkexec.c:941) -- but an
 * inherited string is a thing to trust and a peer credential is a thing to check. */

/* The owning uid, taken from the kernel's record of who created the protocol channel. Falls
 * back to the real uid when stdin is not a socket, which is the direct-launch case the gates
 * use; it is never taken from the environment or from the protocol. */
static void learn_owner_uid(void)
{
    struct ucred cr;
    socklen_t len = sizeof(cr);
    if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 && len == sizeof(cr))
        g_owner_uid = cr.uid;
    else
        g_owner_uid = getuid();
}

/* The directory the rendezvous socket lives in. A compile-time constant, never derived from the
 * environment or from anything the protocol said -- the same rule as the helper's own path. */
static const char *runtime_dir(void)
{
#ifdef CPUPOWER_TEST_BUILD
    static char buf[64];
    if (g_sysroot) {
        const int n = snprintf(buf, sizeof(buf), "%s/run", g_sysroot);
        if (n > 0 && (size_t)n < sizeof(buf))
            return buf;
    }
#endif
    return CPUPOWER_RUNTIME_DIR;
}

/* Create the runtime directory, or prove the one already there is ours to use. A socket in a
 * directory somebody else can rename or replace is a socket somebody else can stand in front
 * of, so the checks are on the OPENED directory rather than on its name: O_NOFOLLOW refuses a
 * symlink outright, and the ownership and mode come from fstat on the descriptor we hold. */
static int open_runtime_dir(void)
{
    const char *dir = runtime_dir();
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        warn_("cannot create %s: %s", dir, strerror(errno));
        return -1;
    }

    const int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0) {
        warn_("cannot open %s: %s", dir, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(dfd, &st) != 0) {
        warn_("cannot stat %s: %s", dir, strerror(errno));
        close(dfd);
        return -1;
    }
    /* Owned by the user we are running as, and writable by nobody else. Anything looser and the
     * socket below could be replaced between our unlink and our bind. */
    if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        warn_("%s is not owned by us or is writable by others; refusing to use it", dir);
        close(dfd);
        return -1;
    }
    return dfd;
}

/* Bind and listen. Returns 0 on success. The socket is mode 0600 and owned by the user who
 * started us, because connecting to a unix socket needs write permission on it: that is the
 * filesystem half of the answer to "who may re-attach", and SO_PEERCRED on accept is the other
 * half. Neither is trusted alone. */
static int start_listening(void)
{
    if (g_listen_fd >= 0)
        return 0;

    const int dfd = open_runtime_dir();
    if (dfd < 0)
        return -1;

    char name[64];
    int n = snprintf(name, sizeof(name), "helper-%lu.sock", (unsigned long)g_owner_uid);
    if (n <= 0 || (size_t)n >= sizeof(name)) {
        close(dfd);
        return -1;
    }

    n = snprintf(g_sock_path, sizeof(g_sock_path), "%s/%s", runtime_dir(), name);
    if (n <= 0 || (size_t)n >= (int)sizeof(g_sock_path)) {
        warn_("rendezvous socket path does not fit in sun_path");
        close(dfd);
        g_sock_path[0] = '\0';
        return -1;
    }

    /* A socket left behind by a helper that was killed. Removing it is safe precisely because
     * the directory above is ours alone: nobody else could have put it there. */
    (void)unlinkat(dfd, name, 0);

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        warn_("socket: %s", strerror(errno));
        close(dfd);
        g_sock_path[0] = '\0';
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)copy_bounded(addr.sun_path, sizeof(addr.sun_path), g_sock_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        warn_("bind %s: %s", g_sock_path, strerror(errno));
        close(fd);
        close(dfd);
        g_sock_path[0] = '\0';
        return -1;
    }

    /* umask is 077 here, so the bind already produced 0600; set it explicitly anyway rather than
     * depend on a umask set several hundred lines away, then hand it to the owner so they can
     * connect to it without being root. */
    if (fchmodat(dfd, name, 0600, 0) != 0 || fchownat(dfd, name, g_owner_uid, (gid_t)-1, 0) != 0) {
        warn_("cannot set mode/owner on %s: %s", g_sock_path, strerror(errno));
        (void)unlinkat(dfd, name, 0);
        close(fd);
        close(dfd);
        g_sock_path[0] = '\0';
        return -1;
    }

    if (listen(fd, 1) != 0) {
        warn_("listen: %s", strerror(errno));
        (void)unlinkat(dfd, name, 0);
        close(fd);
        close(dfd);
        g_sock_path[0] = '\0';
        return -1;
    }

    close(dfd);
    g_listen_fd = fd;
    return 0;
}

static void stop_listening(void)
{
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_sock_path[0]) {
        (void)unlink(g_sock_path);
        g_sock_path[0] = '\0';
    }
}

/* Accept one client and prove who it is. Returns the fd, or -1 if there was nobody to take or
 * the peer was not the owner. Being refused is logged: a connection from another uid is either
 * a bug or somebody trying, and neither should be silent. */
static int accept_client(void)
{
    const int fd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
        return -1;

    struct ucred cr;
    socklen_t len = sizeof(cr);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0 || len != sizeof(cr)) {
        warn_("cannot read peer credentials; refusing the connection");
        close(fd);
        return -1;
    }
    if (cr.uid != g_owner_uid && cr.uid != 0) {
        warn_("refused re-attach from uid %lu (this session belongs to uid %lu)",
              (unsigned long)cr.uid, (unsigned long)g_owner_uid);
        close(fd);
        return -1;
    }
    return fd;
}

/* The one exit path. Everything that ends this process goes through here. */
static void shutdown_and_exit(int code)
{
    const int failures = restore_state();
    if (failures > 0)
        warn_("restore completed with %d failure(s)", failures);
    release_latency();
    stop_listening();
    if (g_cpufd >= 0)
        close(g_cpufd);
    _exit(code);
}

static void on_signal(int sig)
{
    g_signalled = sig;
}

/* ---------------------------------------------------------------------------------------- */
/* PM-QoS. The kernel drops the constraint when the last fd closes, so HOLDING the descriptor is
 * the only way to make it stick and closing it is the only way to release it. That is why DAW
 * mode cannot get stuck on: there is no state to leak, only a descriptor whose lifetime the
 * kernel already manages for us. */
static int set_latency(const char *arg)
{
    if (strcmp(arg, "OFF") == 0) {
        release_latency();
        return 0;
    }

    long us;
    if (parse_int(arg, 0, 2000000, &us) != 0) {
        /* parse_int leaves errno alone on a rejected string, so the caller's strerror(errno)
         * would print whatever the last syscall left there -- "Success" for a value out of
         * range, which is worse than no message. Say what actually happened. */
        errno = EINVAL;
        return -1;
    }

    if (g_latency_fd < 0) {
        g_latency_fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        if (g_latency_fd < 0)
            return -1;
    }

    const int32_t value = (int32_t)us;
    ssize_t n;
    do {
        n = write(g_latency_fd, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof(value))
        g_latency_us = us;

    if (n != (ssize_t)sizeof(value)) {
        release_latency();
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Commands. */

static void cmd_snapshot(void)
{
    char rel[MAX_PATH_REL];
    char buf[MAX_ATTR];

    for (int i = 0; i < g_npolicies; ++i) {
        const int idx = g_snapshot[i].index;
        char gov[MAX_VALUE] = "";
        char epp[MAX_VALUE] = "";
        long cur = -1;

        if (policy_rel(rel, sizeof(rel), idx, "scaling_governor") == 0 &&
            read_rel(rel, buf, sizeof(buf)) == 0)
            (void)copy_bounded(gov, sizeof(gov), buf);

        if (policy_rel(rel, sizeof(rel), idx, "energy_performance_preference") == 0 &&
            read_rel(rel, buf, sizeof(buf)) == 0)
            (void)copy_bounded(epp, sizeof(epp), buf);

        if (policy_rel(rel, sizeof(rel), idx, "scaling_cur_freq") == 0 &&
            read_rel(rel, buf, sizeof(buf)) == 0)
            (void)parse_int(buf, 0, LONG_MAX, &cur);

        say("STATE policy=%d gov=%s epp=%s khz=%ld", idx, gov[0] ? gov : "-", epp[0] ? epp : "-",
            cur);
    }

    /* Turbo reported as INTENT, not as the raw value, so the caller never has to know which of
     * four knobs this machine has or whether it is inverted. */
    int on = -1;
    const char *grel = turbo_global_rel();
    const char *tattr = turbo_policy_attr();
    if (grel && read_rel(grel, buf, sizeof(buf)) == 0) {
        on = (buf[0] == '1');
        if (g_turbo == TURBO_INTEL_NO_TURBO)
            on = !on;
    } else if (tattr && policy_rel(rel, sizeof(rel), g_snapshot[0].index, tattr) == 0 &&
               read_rel(rel, buf, sizeof(buf)) == 0) {
        on = (buf[0] == '1');
    }
    say("TURBO knob=%s on=%d", turbo_name(), on);
    say("LATENCY held=%d", g_latency_fd >= 0 ? 1 : 0);
    say("OK");
}

static void cmd_commit(void)
{
    int applied = 0, failed = 0, skipped = 0;
    char rel[MAX_PATH_REL];

    for (int i = 0; i < g_nstaged; ++i) {
        const struct staged *s = &g_staged[i];
        const char *value = s->value;

        switch (s->op) {
            case OP_GOV:
                if (policy_rel(rel, sizeof(rel), s->policy, "scaling_governor") != 0)
                    goto bad;
                break;
            case OP_EPP:
                if (policy_rel(rel, sizeof(rel), s->policy, "energy_performance_preference") != 0)
                    goto bad;
                break;
            case OP_TURBO: {
                const char *grel = turbo_global_rel();
                if (grel) {
                    (void)copy_bounded(rel, sizeof(rel), grel);
                } else {
                    const char *attr = turbo_policy_attr();
                    if (!attr || policy_rel(rel, sizeof(rel), s->policy, attr) != 0)
                        goto bad;
                }
                break;
            }
            default:
                goto bad;
        }

        if (write_rel(rel, value) != 0) {
            /* Report which write failed and why. A silent failure here is the exact bug the
             * ordering rules exist to prevent: the machine on one setting and the window
             * claiming another. */
            warn_("commit: %s <- \"%s\": %s", rel, value, strerror(errno));
            ++failed;
            continue;
        }
        ++applied;
        continue;

    bad:
        ++skipped;
    }

    g_nstaged = 0;
    say("OK applied=%d failed=%d skipped=%d", applied, failed, skipped);
}

/* Split a line into at most `max` whitespace-separated tokens, in place. */
static int tokenize(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        if (*p)
            *p++ = '\0';
    }
    return n;
}

static void cmd_set(char **argv, int argc)
{
    if (argc < 3) {
        say("ERR set: too few arguments");
        return;
    }
    if (g_nstaged >= MAX_STAGED) {
        say("ERR set: too many staged writes");
        return;
    }

    const char *what = argv[1];

    if (strcmp(what, "turbo") == 0) {
        if (argc != 3) {
            say("ERR set turbo: expected exactly one argument");
            return;
        }
        long intent;
        if (parse_int(argv[2], 0, 1, &intent) != 0) {
            say("ERR set turbo: value must be 0 or 1");
            return;
        }
        if (g_turbo == TURBO_NONE) {
            say("ERR set turbo: no turbo control on this machine");
            return;
        }

        const char *raw = turbo_raw_for(intent != 0);
        if (turbo_global_rel()) {
            struct staged *s = &g_staged[g_nstaged++];
            s->op = OP_TURBO;
            s->policy = -1;
            (void)copy_bounded(s->value, sizeof(s->value), raw);
        } else {
            /* Per-policy knob: one staged write per policy, so a machine whose boost is
             * per-policy is not left half boosted. */
            for (int i = 0; i < g_npolicies && g_nstaged < MAX_STAGED; ++i) {
                struct staged *s = &g_staged[g_nstaged++];
                s->op = OP_TURBO;
                s->policy = g_snapshot[i].index;
                (void)copy_bounded(s->value, sizeof(s->value), raw);
            }
        }
        say("OK");
        return;
    }

    if (argc != 4) {
        say("ERR set %s: expected <policy> <name>", what);
        return;
    }

    long policy;
    if (parse_int(argv[2], 0, 65535, &policy) != 0) {
        say("ERR set %s: bad policy index", what);
        return;
    }
    if (!policy_known((int)policy)) {
        say("ERR set %s: no such policy %ld", what, policy);
        return;
    }
    if (!valid_name(argv[3])) {
        say("ERR set %s: bad name", what);
        return;
    }

    char rel[MAX_PATH_REL];
    enum op op;

    if (strcmp(what, "gov") == 0) {
        op = OP_GOV;
        /* Validated against the KERNEL'S list, read right now. Not against anything compiled
         * into this file. */
        if (policy_rel(rel, sizeof(rel), (int)policy, "scaling_available_governors") != 0 ||
            !in_kernel_list(rel, argv[3])) {
            say("ERR set gov: \"%s\" is not in scaling_available_governors for policy %ld", argv[3],
                policy);
            return;
        }
    } else if (strcmp(what, "epp") == 0) {
        op = OP_EPP;
        if (policy_rel(rel, sizeof(rel), (int)policy, "energy_performance_available_preferences") !=
                0 ||
            !in_kernel_list(rel, argv[3])) {
            say("ERR set epp: \"%s\" is not in energy_performance_available_preferences "
                "for policy %ld",
                argv[3], policy);
            return;
        }
    } else {
        say("ERR set: unknown target \"%s\"", what);
        return;
    }

    struct staged *s = &g_staged[g_nstaged++];
    s->op = op;
    s->policy = (int)policy;
    (void)copy_bounded(s->value, sizeof(s->value), argv[3]);
    say("OK");
}

static void dispatch(char *line)
{
    char *argv[8];
    const int argc = tokenize(line, argv, 8);
    if (argc == 0)
        return; /* a blank line is not an error */

    if (strcmp(argv[0], "SET") == 0) {
        cmd_set(argv, argc);
    } else if (strcmp(argv[0], "COMMIT") == 0) {
        cmd_commit();
    } else if (strcmp(argv[0], "SNAPSHOT") == 0) {
        cmd_snapshot();
    } else if (strcmp(argv[0], "LATENCY") == 0) {
        if (argc != 2)
            say("ERR latency: expected <us> or OFF");
        else if (set_latency(argv[1]) != 0)
            say("ERR latency: %s", strerror(errno));
        else
            say("OK");
    } else if (strcmp(argv[0], "RESTORE") == 0) {
        g_nstaged = 0;
        say("OK failures=%d", restore_state());
    } else if (strcmp(argv[0], "DETACH") == 0) {
        /* Listen BEFORE answering. If we cannot, the honest answer is ERR and staying attached:
         * a client told "OK" would close its end believing the settings were safe, and the EOF
         * that follows would restore them instead -- the exact opposite of what it asked for. */
        if (start_listening() != 0) {
            say("ERR detach: cannot create the rendezvous socket");
        } else {
            g_detached = 1;
            say("OK %s", g_sock_path);
        }
    } else if (strcmp(argv[0], "QUIT") == 0) {
        say("OK");
        shutdown_and_exit(EX_OK_);
    } else {
        say("ERR unknown command");
    }
}

/* ---------------------------------------------------------------------------------------- */
/* The loop. Reads bounded lines from the current protocol channel. An overlong line is a fatal
 * protocol error rather than something to truncate and act on: acting on the first 256 bytes of
 * a longer command is exactly the kind of partial interpretation that turns malformed input into
 * a wrong write.
 *
 * Returns when the channel ends. EOF means the client went away WITHOUT saying DETACH -- it
 * exited, was killed, or crashed -- and the caller restores. That is what keeps reversibility
 * structural: a client that dies badly still puts the machine back, and no path depends on an
 * orderly shutdown. DETACH is the one way to end a channel without that happening, and it is
 * explicit precisely so that a crash can never be mistaken for it. */
enum serve_result { SERVE_EOF, SERVE_DETACHED };

static enum serve_result serve(void)
{
    char line[MAX_LINE];
    size_t used = 0;

    for (;;) {
        if (g_signalled) {
            warn_("caught signal %d; restoring", (int)g_signalled);
            shutdown_and_exit(EX_OK_);
        }

        /* While attached, the listening socket is still polled so that a SECOND client gets a
         * refusal instead of a silent hang: it connects, is told the session is busy, and goes
         * away. Only one client owns the channel at a time. */
        struct pollfd pfd[2];
        pfd[0].fd = g_in_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = g_listen_fd;
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        const int nfds = g_listen_fd >= 0 ? 2 : 1;

        const int r = poll(pfd, (nfds_t)nfds, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            warn_("poll: %s", strerror(errno));
            shutdown_and_exit(EX_ENV_);
        }

        if (nfds == 2 && (pfd[1].revents & POLLIN) != 0) {
            const int busy = accept_client();
            if (busy >= 0) {
                full_write(busy, "ERR busy\n", 9);
                close(busy);
            }
        }

        if ((pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            continue;

        char chunk[MAX_LINE];
        const ssize_t n = read(g_in_fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            warn_("read: %s", strerror(errno));
            shutdown_and_exit(EX_ENV_);
        }
        if (n == 0)
            return SERVE_EOF;

        for (ssize_t i = 0; i < n; ++i) {
            const char c = chunk[i];

            if (c == '\n') {
                line[used] = '\0';
                /* Defensive: a pty would give us CR LF. The isatty() check at startup should
                 * make that impossible, and this costs one comparison. */
                if (used > 0 && line[used - 1] == '\r')
                    line[used - 1] = '\0';
                dispatch(line);
                if (g_detached)
                    return SERVE_DETACHED; /* the OK has already been written */
                used = 0;
                continue;
            }

            if (c == '\0') {
                say("ERR embedded NUL in command");
                shutdown_and_exit(EX_PROTO_);
            }

            if (used + 1 >= sizeof(line)) {
                say("ERR command too long");
                warn_("protocol line exceeded %zu bytes; closing", sizeof(line));
                shutdown_and_exit(EX_PROTO_);
            }
            line[used++] = c;
        }
    }
}

/* ---------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int serve_flag = 0;

#ifdef CPUPOWER_TEST_BUILD
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--serve") == 0)
            serve_flag = 1;
        else if (strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc)
            g_sysroot = argv[++i];
        else {
            dprintf(STDERR_FILENO, "usage: %s --serve [--sysroot DIR]\n", argv[0]);
            return EX_USAGE_;
        }
    }
    /* The test build must never be the privileged one. If it somehow is, that is a packaging
     * mistake and it stops here rather than becoming a way to write to real sysfs with the
     * --sysroot code path compiled in. */
    if (geteuid() == 0) {
        dprintf(STDERR_FILENO, "cpu-power-helper-test: refusing to run as root\n");
        return EX_NOPERM_;
    }
#else
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--serve") == 0) {
            serve_flag = 1;
        } else {
            /* No --sysroot here. It does not exist in this binary, and an
             * unrecognised option is refused rather than ignored. */
            dprintf(STDERR_FILENO, "usage: %s --serve\n", argv[0]);
            return EX_USAGE_;
        }
    }

    if (geteuid() != 0) {
        dprintf(STDERR_FILENO, "cpu-power-helper: must run as root\n");
        return EX_NOPERM_;
    }

#endif

    if (!serve_flag) {
        dprintf(STDERR_FILENO, "usage: %s --serve\n", argv[0]);
        return EX_USAGE_;
    }

    /* A tty on the protocol channel means line discipline: echo, CR/LF translation, ^D. That
     * would corrupt this protocol silently, and differently depending on how the GUI happened to
     * be launched -- working from a desktop menu and misbehaving from a shell, or the reverse.
     *
     * The caller setsid()s before the exec, so nothing it launches has a controlling terminal
     * and no launcher can interpose a pseudo-terminal. This check is the backstop for the case
     * where that did not happen for a reason nobody anticipated: fail loudly here rather than
     * misbehave quietly.
     *
     * This check guards the PROTOCOL CHANNEL, not privilege, so it applies to both builds -- and
     * being in both is what lets scripts/sec-gate.sh assert it without needing root. */
    if (isatty(STDIN_FILENO)) {
        dprintf(STDERR_FILENO,
                "cpu-power-helper: stdin is a terminal; refusing to serve.\n"
                "  The protocol needs a pipe, not a terminal. The caller must setsid() before\n"
                "  the exec so that no pseudo-terminal can be interposed.\n");
        return EX_ENV_;
    }

    /* No core file may contain a snapshot of this process, and no non-root process may ptrace
     * it. PR_SET_DUMPABLE does both. */
    const struct rlimit no_core = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    (void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    /* This process never execs. Make that structural rather than a promise: with NO_NEW_PRIVS
     * set, even a hypothetical exec could not gain privilege through setuid or file caps. */
    (void)prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    /* Nothing above fd 2 is ours. Close the lot so nothing inherited from the parent — or from
     * whatever escalated us — is reachable from here. */
    (void)close_range(3, ~0U, 0);

    umask(077);
#ifndef CPUPOWER_TEST_BUILD
    (void)setgroups(0, NULL);
#endif

    const char *root = "/sys/devices/system/cpu";
    char rootbuf[PATH_MAX];
#ifdef CPUPOWER_TEST_BUILD
    if (g_sysroot) {
        const int n = snprintf(rootbuf, sizeof(rootbuf), "%s/devices/system/cpu", g_sysroot);
        if (n <= 0 || (size_t)n >= sizeof(rootbuf)) {
            dprintf(STDERR_FILENO, "cpu-power-helper-test: sysroot path too long\n");
            return EX_USAGE_;
        }
        root = rootbuf;
    }
#else
    (void)rootbuf;
#endif

    g_cpufd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (g_cpufd < 0) {
        dprintf(STDERR_FILENO, "cpu-power-helper: cannot open %s: %s\n", root, strerror(errno));
        return EX_ENV_;
    }

#ifndef CPUPOWER_TEST_BUILD
    /* Prove it is really sysfs. This is what makes the --sysroot option safe to have in the
     * OTHER build: the test binary can be pointed anywhere, and this one cannot be, because a
     * bind-mounted or chrooted fake is not sysfs and fstatfs says so. */
    struct statfs sfs;
    if (fstatfs(g_cpufd, &sfs) != 0 || (unsigned long)sfs.f_type != (unsigned long)SYSFS_MAGIC) {
        dprintf(STDERR_FILENO, "cpu-power-helper: %s is not sysfs (f_type=0x%lx, expected 0x%lx)\n",
                root, (unsigned long)sfs.f_type, (unsigned long)SYSFS_MAGIC);
        close(g_cpufd);
        return EX_ENV_;
    }
#endif

    if (discover() != 0) {
        close(g_cpufd);
        return EX_ENV_;
    }
    snapshot_state();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);
    /* A write to a closed pipe must return EPIPE to our own error handling, not kill us before
     * restore_state() has run. */
    (void)signal(SIGPIPE, SIG_IGN);

    char driver[MAX_VALUE] = "";
    char rel[MAX_PATH_REL];
    char buf[MAX_ATTR];
    if (policy_rel(rel, sizeof(rel), g_snapshot[0].index, "scaling_driver") == 0 &&
        read_rel(rel, buf, sizeof(buf)) == 0)
        (void)copy_bounded(driver, sizeof(driver), buf);

    learn_owner_uid();

    say("READY driver=%s policies=%d turbo=%s latency=%ld", driver[0] ? driver : "-",
        g_npolicies, turbo_name(), g_latency_us);

    /* The lifetime of this process is the lifetime of the SETTINGS, not of the window.
     *
     *   EOF with no DETACH  -> the client died. Restore and exit.
     *   DETACH              -> the window closed on purpose. Keep everything, wait for the next.
     *   QUIT                -> switched off. Restore and exit. (Handled in dispatch.)
     *
     * So a root process exists exactly while the machine is overridden. There is no state on
     * disk anywhere in this: what the machine looked like beforehand is held right here, in
     * memory, for as long as there is something to put back. */
    for (;;) {
        if (serve() == SERVE_EOF)
            shutdown_and_exit(EX_OK_);

        /* Detached. The old channel belongs to a process that is on its way out; drop it so a
         * stale descriptor cannot be read from or written to, and so the client's close() is not
         * waiting on us. */
        if (g_in_fd > STDERR_FILENO)
            close(g_in_fd);
        g_in_fd = -1;
        g_out_fd = -1;

        int client = -1;
        while (client < 0) {
            if (g_signalled) {
                warn_("caught signal %d; restoring", (int)g_signalled);
                shutdown_and_exit(EX_OK_);
            }
            struct pollfd lp = {.fd = g_listen_fd, .events = POLLIN, .revents = 0};
            const int r = poll(&lp, 1, -1);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                warn_("poll: %s", strerror(errno));
                shutdown_and_exit(EX_ENV_);
            }
            client = accept_client();
        }

        g_in_fd = client;
        g_out_fd = client;
        g_detached = 0;
        g_nstaged = 0; /* nothing half-staged may survive a change of client */

        /* The same greeting a fresh client gets, so re-attaching and starting are the same code
         * path on the other side. */
        say("READY driver=%s policies=%d turbo=%s latency=%ld", driver[0] ? driver : "-",
            g_npolicies, turbo_name(), g_latency_us);
    }
}
