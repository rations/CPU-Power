/* fdprobe.c -- does pkexec hand our protocol channel through unchanged?
 *
 * NEVER INSTALLED. It exists for one question, asked before the polkit privilege path was
 * written: the GUI talks to the helper over a socketpair dup2'd onto fd 0 and fd 1, and the
 * helper refuses to serve if stdin is a terminal. If pkexec sanitises, closes or replaces those
 * two descriptors, the whole design is wrong and no amount of careful code downstream saves it.
 *
 * Reading the answer out of the binary is not the same as measuring it. `nm -D` on the shipped
 * pkexec shows no fork/vfork/clone/wait symbols and exactly one execv, and its descriptor loop
 * skips anything <= 2 -- but that is one build of one version on one machine, and a number or a
 * behaviour that gates this design is measured, never estimated. This measures them.
 *
 * It is two programs in one binary:
 *
 *   --probe    the child. Exec'd through pkexec. Reports what it can see about fd 0 and 1, then
 *              echoes one line, then waits to be closed.
 *   (default)  the harness. Builds the socketpair exactly the way HelperSession::spawn() does,
 *              execs pkexec, and asserts on what comes back.
 *
 * pkexec matches an action by comparing the .policy file's exec.path annotation against the argv
 * string BYTE FOR BYTE, with no canonicalisation (polkit-126 pkexec.c:629 and :310, captured in
 * third_party/refs/polkit/). So --target names the path the temporary test policy names, which is
 * a ROOT-OWNED copy of this binary and not the one in the build tree: a policy saying allow_active
 * over a user-writable file is a local root hole for as long as it is installed, and the point of
 * this experiment is not to open one. Default: our own path, for a dry run without a policy.
 *
 * --direct is the CONTROL. It runs the same assertions with the probe exec'd straight, no pkexec
 * in the picture. Every check must pass there. If one fails under --direct the harness is wrong,
 * not pkexec -- and without that control a broken assertion reads as a broken design.
 *
 * Usage:  fdprobe [--pkexec <path>] [--target <path>] [--direct] [--probe]
 */

#define _GNU_SOURCE

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PKEXEC_DEFAULT "/usr/bin/pkexec"

static int gPass = 0;
static int gFail = 0;

static void ok_(const char *what)
{
    gPass++;
    printf("  ok    %s\n", what);
}

static void bad_(const char *what)
{
    gFail++;
    printf("  FAIL  %s\n", what);
}

static void check_(int cond, const char *what)
{
    if (cond)
        ok_(what);
    else
        bad_(what);
}

/* ---------------------------------------------------------------------------------------- */
/* The child. Everything it says goes to fd 1, which is the socket -- exactly as the helper's
 * protocol responses do. */

static void say_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say_(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)!write(STDOUT_FILENO, buf, (size_t)n);
}

/* What /proc says this descriptor actually is, e.g. "socket:[12345]". That string is what lets
 * the harness prove the SAME socket arrived, not merely that something is open on fd 0. */
static void fd_target(int fd, char *out, size_t cap)
{
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(link, out, cap - 1);
    if (n < 0)
        snprintf(out, cap, "unreadable:%s", strerror(errno));
    else
        out[n] = '\0';
}

static int run_probe(void)
{
    char f0[128], f1[128];
    fd_target(STDIN_FILENO, f0, sizeof(f0));
    fd_target(STDOUT_FILENO, f1, sizeof(f1));

    struct stat s0, s1;
    const int r0 = fstat(STDIN_FILENO, &s0);
    const int r1 = fstat(STDOUT_FILENO, &s1);

    say_("PROBE pid=%ld ppid=%ld\n", (long)getpid(), (long)getppid());
    say_("FD0 target=%s sock=%d tty=%d\n", f0, (r0 == 0 && S_ISSOCK(s0.st_mode)) ? 1 : 0,
         isatty(STDIN_FILENO) ? 1 : 0);
    say_("FD1 target=%s sock=%d tty=%d\n", f1, (r1 == 0 && S_ISSOCK(s1.st_mode)) ? 1 : 0,
         isatty(STDOUT_FILENO) ? 1 : 0);
    /* Read PR_SET_PDEATHSIG straight back rather than inferring it from whether we die later.
     * pkexec sets it to SIGTERM before it authorises anything (polkit-126 pkexec.c:737); the
     * kernel clears it across an exec that changes credentials. Asking the kernel what the value
     * actually is turns that from an argument into a reading. */
    int pdeath = -1;
    if (prctl(PR_GET_PDEATHSIG, &pdeath, 0, 0, 0) != 0)
        pdeath = -1;
    say_("UID uid=%ld euid=%ld\n", (long)getuid(), (long)geteuid());
    say_("PDEATHSIG value=%d\n", pdeath);

    /* A pdeath signal that arrives and is then ignored or blocked looks exactly like one that was
     * never set. These three lines tell the two apart. SigIgn bit 0xf is SIGTERM (signal 15). */
    char sig[256];
    size_t used = 0;
    sig[0] = '\0';
    FILE *st = fopen("/proc/self/status", "r");
    if (st) {
        char line[256];
        while (fgets(line, sizeof(line), st) && used + 1 < sizeof(sig)) {
            if (strncmp(line, "SigBlk:", 7) != 0 && strncmp(line, "SigIgn:", 7) != 0 &&
                strncmp(line, "SigCgt:", 7) != 0)
                continue;
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            const int w = snprintf(sig + used, sizeof(sig) - used, "%s ", line);
            if (w < 0 || (size_t)w >= sizeof(sig) - used)
                break;
            used += (size_t)w;
        }
        fclose(st);
    }
    say_("SIGNALS %s\n", sig);
    say_("READY probe\n");

    /* Echo one line, the way the helper answers a command, then block until the far end closes.
     * That is the D5 lifetime: EOF is the shutdown signal, not a message. */
    char in[256];
    const ssize_t n = read(STDIN_FILENO, in, sizeof(in) - 1);
    if (n > 0) {
        in[n] = '\0';
        char *nl = strchr(in, '\n');
        if (nl)
            *nl = '\0';
        say_("ECHO %s\n", in);
    }

    char drain[16];
    while (read(STDIN_FILENO, drain, sizeof(drain)) > 0) {
    }

    /* EOF. Say so on STDERR, never on fd 1: fd 1 is the socket whose far end has just closed, and
     * writing to it raises SIGPIPE, whose default action kills us -- so the exit status would read
     * as a crash rather than the clean shutdown it is. The control run caught exactly that. The
     * same trap is live in the real helper, which must restore and exit on EOF without answering.
     */
    (void)!write(STDERR_FILENO, "fdprobe: probe saw EOF, exiting 0\n", 34);
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* The harness. */

/* Read whatever has arrived within a deadline, stopping early once `until` shows up. Not
 * line-buffered: this is a diagnostic, and seeing a partial or malformed answer is as informative
 * as seeing the right one.
 *
 * `fd` MUST be O_NONBLOCK. The probe writes its report and then blocks in read() waiting for a
 * command, so a blocking read here waits forever for a peer that is waiting for us -- which is
 * exactly how the first run of this harness deadlocked. */
static size_t slurp(int fd, char *out, size_t cap, int ms, const char *until)
{
    size_t got = 0;
    out[0] = '\0';
    const long deadline = (long)time(NULL) + (ms / 1000) + 1;
    while (got + 1 < cap && (long)time(NULL) <= deadline) {
        struct timespec ts = {0, 20L * 1000 * 1000};
        const ssize_t n = read(fd, out + got, cap - got - 1);
        if (n > 0) {
            got += (size_t)n;
            out[got] = '\0';
            if (until && strstr(out, until))
                break;
            continue;
        }
        if (n == 0)
            break; /* peer closed */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
        nanosleep(&ts, NULL);
    }
    out[got] = '\0';
    return got;
}

/* The parent end of every channel here is non-blocking, for the reason slurp() gives. */
static int unblock(int fd)
{
    const int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

/* Turn THIS process into the probe, by way of pkexec. Never returns.
 *
 * Exit codes here are deliberately 111-113: pkexec itself exits 126 for a dismissed request and
 * 127 for everything else (polkit-126 pkexec.c:915 and :504), so a spawn failure must not land on
 * either or a genuine refusal becomes unreadable. */
static void become_probe(int sv[2], const char *pkexec, const char *target, int direct)
{
    close(sv[0]);
    if (dup2(sv[1], STDIN_FILENO) < 0 || dup2(sv[1], STDOUT_FILENO) < 0)
        _exit(111);
    if (sv[1] > STDERR_FILENO)
        close(sv[1]);

    (void)!setsid();
    /* pkexec chdir()s to the target's home unless --keep-cwd, and treats FAILURE AS FATAL
     * (pkexec.c:1038-1046). Doing it ourselves is deterministic and pins no directory. */
    if (chdir("/") != 0)
        _exit(113);

    char a0[] = "pkexec";
    char a1[] = "--disable-internal-agent";
    char a2[] = "--keep-cwd";
    char a4[] = "--probe";
    char *const ev[] = {NULL}; /* EMPTY: pkexec validates SHELL against /etc/shells first */

    if (direct) {
        char *const dv[] = {(char *)target, a4, NULL};
        execve(target, dv, ev);
        _exit(112);
    }

    char *const av[] = {a0, a1, a2, (char *)target, a4, NULL};
    execve(pkexec, av, ev);
    _exit(112);
}

/* ---------------------------------------------------------------------------------------- */
/* Does pkexec's PR_SET_PDEATHSIG reach the program it execs? MEASURED ANSWER: NO.
 *
 * pkexec does set it, to SIGTERM, before it authorises anything (polkit-126 pkexec.c:737). It is
 * then cleared again before the exec ever happens, by pkexec's own credential change at
 * pkexec.c:1044-1056 -- setgroups/initgroups/setregid/setreuid. prctl's PR_SET_PDEATHSIG page:
 * "The parent-death signal setting is also cleared upon changes to any of the following thread
 * credentials: effective user ID, effective group ID, filesystem user ID, or filesystem group
 * ID." pkexec is setuid but NOT setgid, so its effective gid is the caller's until setregid
 * changes it, and that alone is enough to clear the setting.
 *
 * This function asserts that, because an earlier reading of the design assumed the opposite --
 * reasoning from the man page's *exec* rule, which is the wrong rule here, since the clearing
 * happens at the credential change and not at the exec.
 *
 * WHY IT MATTERS: there is no second death trigger. Reversibility rests entirely on EOF -- the
 * GUI's end of the socket closing, the helper restoring and exiting. The upside is that a
 * kill-the-GUI test still isolates the EOF path, because nothing else could produce the death.
 *
 * Shape: harness -> middle -> (pkexec -> probe). The harness SIGKILLs the middle and watches the
 * probe WHILE STILL HOLDING ITS END OF THE SOCKET OPEN, so no EOF is possible and a death could
 * only have come from a parent-death signal. */
static void test_pdeathsig(const char *pkexec, const char *target, int direct)
{
    printf("\n-- PR_SET_PDEATHSIG across pkexec's execv --\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        bad_("socketpair for the pdeathsig phase");
        return;
    }

    const pid_t mid = fork();
    if (mid < 0) {
        bad_("fork for the pdeathsig phase");
        close(sv[0]);
        close(sv[1]);
        return;
    }

    if (mid == 0) {
        /* The middle. Its only job is to be the probe's parent and then be killed. */
        close(sv[0]);
        const pid_t gk = fork();
        if (gk == 0)
            become_probe(sv, pkexec, target, direct);
        if (gk < 0)
            _exit(114);
        for (;;)
            pause();
    }

    close(sv[1]);
    if (!unblock(sv[0]))
        bad_("could not make the pdeathsig channel non-blocking");

    char buf[2048];
    slurp(sv[0], buf, sizeof(buf), 6000, "READY probe\n");

    long probePid = 0;
    const char *at = strstr(buf, "PROBE pid=");
    if (at)
        probePid = strtol(at + strlen("PROBE pid="), NULL, 10);
    check_(strstr(buf, "PDEATHSIG value=0\n") != NULL,
           "the kernel reports no parent-death signal set (PR_GET_PDEATHSIG == 0)");
    const char *pd = strstr(buf, "PDEATHSIG value=");
    if (pd)
        printf("        %.*s\n", (int)strcspn(pd, "\n"), pd);

    check_(probePid > 0, "the probe reported its pid over the socket");
    if (probePid <= 0) {
        kill(mid, SIGKILL);
        waitpid(mid, NULL, 0);
        close(sv[0]);
        return;
    }

    char want[64];
    snprintf(want, sizeof(want), "ppid=%ld", (long)mid);
    check_(strstr(buf, want) != NULL, "the probe's parent is the middle process, not pkexec");

    /* Kill the middle only. Our end of the socket stays open for the whole wait below, so the
     * probe cannot be seeing EOF. */
    if (kill(mid, SIGKILL) != 0) {
        bad_("SIGKILL the middle process");
        close(sv[0]);
        return;
    }
    (void)waitpid(mid, NULL, 0);

    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/%ld", probePid);
    int gone = 0;
    for (int i = 0; i < 60; i++) { /* up to 3s */
        if (access(proc, F_OK) != 0) {
            gone = 1;
            break;
        }
        struct timespec ts = {0, 50L * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    const char *sg = strstr(buf, "SIGNALS ");
    if (sg)
        printf("        %.*s\n", (int)strcspn(sg, "\n"), sg);

    check_(!gone, "pkexec's PDEATHSIG does NOT reach the helper -- EOF is the only death trigger");
    if (gone)
        printf("        the probe died with its socket still open. pkexec's PDEATHSIG now DOES\n"
               "        survive, so a kill-the-GUI test can no longer isolate the EOF path.\n");
    else
        kill((pid_t)probePid, SIGKILL); /* ours to clean up; nothing else will reap it */

    close(sv[0]);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *pkexec = PKEXEC_DEFAULT;
    const char *target = NULL;
    int direct = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0)
            return run_probe();
        if (strcmp(argv[i], "--direct") == 0)
            direct = 1;
        if (strcmp(argv[i], "--pkexec") == 0 && i + 1 < argc)
            pkexec = argv[++i];
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
            target = argv[++i];
    }

    char self[PATH_MAX];
    const ssize_t sn = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sn < 0) {
        fprintf(stderr, "fdprobe: cannot find my own path: %s\n", strerror(errno));
        return 2;
    }
    self[sn] = '\0';
    if (!target)
        target = self;

    printf("== pkexec descriptor and process shape ==\n");
    if (direct)
        printf("        pkexec  (none -- CONTROL RUN, exec'd direct)\n");
    else
        printf("        pkexec  %s\n", pkexec);
    printf("        probe   %s\n", target);

    /* The same channel HelperSession::spawn() builds: AF_UNIX SOCK_STREAM, both halves CLOEXEC,
     * one end dup2'd onto the child's fd 0 AND fd 1. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        fprintf(stderr, "fdprobe: socketpair: %s\n", strerror(errno));
        return 2;
    }

    char parentEnd[128];
    {
        char link[64];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", sv[1]);
        const ssize_t n = readlink(link, parentEnd, sizeof(parentEnd) - 1);
        parentEnd[n > 0 ? n : 0] = '\0';
    }

    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fdprobe: fork: %s\n", strerror(errno));
        return 2;
    }

    if (pid == 0)
        become_probe(sv, pkexec, target, direct);

    close(sv[1]);
    if (!unblock(sv[0])) {
        fprintf(stderr, "fdprobe: cannot set O_NONBLOCK: %s\n", strerror(errno));
        return 2;
    }

    char buf[4096];
    const size_t got = slurp(sv[0], buf, sizeof(buf), 6000, "READY probe\n");

    printf("\n-- what came back over the socket --\n");
    if (got == 0)
        printf("(nothing)\n");
    else
        fputs(buf, stdout);
    printf("-- end --\n\n");

    /* 1 + 2: the descriptors arrived unmodified. The probe's fd 0 must name the SAME socket
     * inode as our end's peer, and must not be a tty. */
    char want[160];
    snprintf(want, sizeof(want), "FD0 target=%s sock=1 tty=0", parentEnd);
    check_(strstr(buf, want) != NULL, "fd 0 is the same socket, and is not a tty");
    snprintf(want, sizeof(want), "FD1 target=%s sock=1 tty=0", parentEnd);
    check_(strstr(buf, want) != NULL, "fd 1 is the same socket, and is not a tty");

    /* 3: pkexec did not stay resident. If it had forked, the probe's parent would be pkexec and
     * our waitpid() would reap pkexec rather than the probe. */
    snprintf(want, sizeof(want), "ppid=%ld", (long)getpid());
    check_(strstr(buf, want) != NULL, "the probe's parent is US -- pkexec exec'd, it did not fork");

    check_(strstr(buf, "READY probe") != NULL, "a READY line arrived over the socket");

    /* 4: the channel carries traffic both ways. */
    const char *cmd = "HELLO\n";
    check_(write(sv[0], cmd, strlen(cmd)) == (ssize_t)strlen(cmd), "wrote a command to the child");
    char buf2[1024];
    slurp(sv[0], buf2, sizeof(buf2), 2000, "ECHO HELLO\n");
    check_(strstr(buf2, "ECHO HELLO") != NULL, "the child read our command and answered");

    /* 5: closing our end is the shutdown signal (D5). */
    close(sv[0]);
    int status = 0;
    const pid_t reaped = waitpid(pid, &status, 0);
    check_(reaped == pid, "waitpid reaped the process we forked");
    check_(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "the child exited cleanly after EOF on its stdin");
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        if (WIFSIGNALED(status))
            printf("        it died of signal %d (%s)\n", WTERMSIG(status),
                   strsignal(WTERMSIG(status)));
        else if (WIFEXITED(status))
            printf("        it exited %d\n", WEXITSTATUS(status));
    }

    if (WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127))
        printf("        note: exit %d is pkexec's own (126 dismissed / 127 not authorized)\n",
               WEXITSTATUS(status));

    if (!direct)
        test_pdeathsig(pkexec, target, direct);

    printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
