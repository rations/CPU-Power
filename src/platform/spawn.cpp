// HelperSession implementation. See spawn.h.

#include "spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>

#ifndef CPUPOWER_HELPER_PATH
#error "CPUPOWER_HELPER_PATH must be defined at build time -- see CMakeLists.txt. \
The helper path is never derived at runtime."
#endif

#ifndef CPUPOWER_POLKIT_ACTION_ID
#error "CPUPOWER_POLKIT_ACTION_ID must be defined at build time -- see CMakeLists.txt. \
It must be the same literal the installed .policy file carries."
#endif

#ifndef CPUPOWER_PKEXEC_ARGV1
#error "CPUPOWER_PKEXEC_ARGV1 must be defined at build time -- see CMakeLists.txt. \
It must match the .policy file's org.freedesktop.policykit.exec.argv1 annotation."
#endif

namespace cpupower
{

namespace
{

// Absolute path to pkexec. NEVER $PATH: $PATH is attacker-influenceable and
// this program is setuid root, so resolving it through one would defeat the point of pinning the
// helper's own path. The first candidate that exists wins.
const char *findPkexec()
{
    static const char *candidates[] = {"/usr/bin/pkexec", "/bin/pkexec"};
    for (const char *c : candidates) {
        struct stat st = {};
        if (::stat(c, &st) == 0 && S_ISREG(st.st_mode))
            return c;
    }
    return nullptr;
}

// Absolute path to pkcheck, same reasoning as the two above. pkcheck ships with polkitd rather
// than with pkexec -- on this distribution they are separate packages -- so its absence is a
// real case and not a theoretical one.
const char *findPkcheck()
{
    static const char *candidates[] = {"/usr/bin/pkcheck", "/bin/pkcheck"};
    for (const char *c : candidates) {
        struct stat st = {};
        if (::stat(c, &st) == 0 && S_ISREG(st.st_mode))
            return c;
    }
    return nullptr;
}

// This process as "pid,start-time,uid", which is the only form of --process that is not racy.
//
// pkcheck(1) NOTES: "Do not use either the bare pid or pid,start-time syntax forms for --process.
// There are race conditions in both. New code should always use pid,pid-start-time,uid." A pid on
// its own can be recycled between our asking and polkit looking, so the answer could describe a
// different process entirely.
//
// start time is field 22 of /proc/self/stat. Field 2 is the executable name IN PARENTHESES and
// may itself contain a ')', so the scan starts after the LAST one -- the usual trap with this
// file, and the reason this is not a sscanf.
bool selfProcessSpec(std::string &out)
{
    const int fd = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[512];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';

    const char *p = ::strrchr(buf, ')');
    if (!p)
        return false;
    ++p;

    // Field 3 is the first token after ')', so field 22 is the 20th one. Walk them as TOKENS, not
    // as numbers: field 3 is the state, a single letter, so parsing each one as an integer stops
    // dead on the very first. (It did. The failure was at least in the safe direction -- the
    // polkit rung was skipped rather than used blind -- but the tool refused to work at all.)
    const char *tok = nullptr;
    for (int n = 0; n < 20; n++) {
        while (*p == ' ')
            ++p;
        if (*p == '\0')
            return false;
        tok = p;
        while (*p != '\0' && *p != ' ')
            ++p;
    }

    char *end = nullptr;
    const unsigned long long startTime = ::strtoull(tok, &end, 10);
    if (end == tok)
        return false;

    char spec[128];
    const int len = ::snprintf(spec, sizeof(spec), "%ld,%llu,%lu", static_cast<long>(::getpid()),
                               startTime, static_cast<unsigned long>(::getuid()));
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(spec))
        return false;
    out.assign(spec, static_cast<size_t>(len));
    return true;
}

// Will polkit authorise THIS process for `actionId` without asking anybody anything?
//
// THIS IS WHAT STOPS THE TOOL EVER PRODUCING A PASSWORD PROMPT, and it exists because a prompt
// was produced. If the action is not registered -- a wrong install prefix, a policy file polkitd
// cannot parse, a packager who put it somewhere polkitd does not read -- pkexec does not fail.
// It falls back to org.freedesktop.policykit.exec, whose implicit defaults are auth_admin, and
// the desktop's own authentication agent puts a root password dialog on screen. Nothing crashes;
// the tool simply starts asking for something it promises never to ask for.
//
// --disable-internal-agent does not prevent that. It stops pkexec registering ITS OWN text agent
// on the protocol channel, which is a different problem; it says nothing about the agent the
// session already has.
//
// So the question is asked in advance, of the only thing that can answer it. pkcheck(1) returns 0
// for authorised, 1 for not authorised, 2 for "would need authentication", and errors otherwise
// (127 when the action is not registered). WITHOUT --allow-user-interaction IT CANNOT PROMPT --
// that is why it is safe to ask, and the flag is deliberately not passed here.
//
// ONLY 0 IS GOOD ENOUGH. Anything else means pkexec would either fail or ask, and falling through
// to the next rung is better than both.
bool polkitAuthorisesSilently(const char *pkcheck, const char *actionId, const std::string &spec)
{
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;

    if (pid == 0) {
        // Child. This is a question, not an action: its diagnostics are not the user's problem
        // and must not reach the protocol channel.
        const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                ::close(devnull);
        }
        char arg0[] = "pkcheck";
        char arg1[] = "--action-id";
        char arg3[] = "--process";
        std::vector<char> idBuf(actionId, actionId + ::strlen(actionId) + 1);
        std::vector<char> specBuf(spec.begin(), spec.end());
        specBuf.push_back('\0');
        char *const argv[] = {arg0, arg1, idBuf.data(), arg3, specBuf.data(), nullptr};
        char *const envp[] = {nullptr};
        ::execve(pkcheck, argv, envp);
        ::_exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

//------------------------------------------------------------------------
const char *HelperSession::helperPath()
{
    return CPUPOWER_HELPER_PATH;
}

const char *HelperSession::polkitActionId()
{
    return CPUPOWER_POLKIT_ACTION_ID;
}

const char *HelperSession::pkexecArgv1()
{
    return CPUPOWER_PKEXEC_ARGV1;
}

const char *HelperSession::methodName(Method m)
{
    switch (m) {
        case Method::Direct:
            return "direct";
        case Method::Polkit:
            return "polkit";
        case Method::Unprivileged:
            return "unprivileged";
        case Method::Adopted:
            return "adopted";
        case Method::None:
            break;
    }
    return "none";
}

//------------------------------------------------------------------------
HelperSession::~HelperSession()
{
    stop();
}

//------------------------------------------------------------------------
// Defence in depth. The real guarantee is that $prefix/libexec is root-owned and not user
// writable; this turns a broken or tampered install into a refusal with a reason, rather than
// something unexpected being run as root.
bool HelperSession::checkHelperBinary(const std::string &path)
{
    struct stat st = {};
    if (::lstat(path.c_str(), &st) != 0) {
        mError =
            "the privileged helper is not installed at " + path + " (" + ::strerror(errno) + ")";
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        mError = path + " is a symbolic link; refusing to run it as root";
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        mError = path + " is not a regular file; refusing to run it as root";
        return false;
    }
    if (st.st_uid != 0) {
        mError = path + " is not owned by root; refusing to run it as root";
        return false;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        mError = path + " is group- or world-writable; refusing to run it as root";
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
// Child-side failure exit codes. DELIBERATELY NOT 126 OR 127: pkexec uses 126 for "the
// authorisation request was dismissed" and 127 for everything else including "not authorised", so
// reusing them would make a real refusal indistinguishable from a failure to start, and the
// message shown to the user would be wrong rather than merely vague.
enum : int {
    kChildDup2Failed = 111,
    kChildExecFailed = 112,
    kChildChdirFailed = 113,
};

bool HelperSession::spawn(const std::vector<std::string> &argv)
{
    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        mError = std::string("socketpair: ") + ::strerror(errno);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        mError = std::string("fork: ") + ::strerror(errno);
        ::close(sv[0]);
        ::close(sv[1]);
        return false;
    }

    if (pid == 0) {
        // ---- child ----
        ::close(sv[0]);
        if (::dup2(sv[1], STDIN_FILENO) < 0 || ::dup2(sv[1], STDOUT_FILENO) < 0)
            ::_exit(kChildDup2Failed);
        if (sv[1] > STDERR_FILENO)
            ::close(sv[1]);

        // Detach from the controlling terminal before the exec: it keeps the helper out of the
        // launching shell's foreground process group, so a Ctrl-C aimed at the GUI does not also
        // land on the root process.
        //
        // This had a second and louder justification while a `sudo -n` rung existed -- sudoers(5)
        // runs a command in a new pseudo-terminal when sudo is attached to one, and a pty on the
        // protocol channel applies line discipline to it: echo, CR/LF translation, ^D. That made
        // the tool behave differently from a menu than from a shell. The rung is gone and pkexec
        // allocates no pty, so only the process-group reason remains -- but the helper's own
        // isatty() refusal stays, because a loud failure is the right answer if a terminal ever
        // reaches the protocol channel by some route nobody anticipated.
        //
        // It is SAFE under polkit, which was measured rather than assumed, because it is not
        // obvious: pkexec authorises its PARENT, via getppid(), so anything that reparented us
        // would change which session polkit inspects. setsid() starts a new session and process
        // group but does NOT reparent, and pkcheck returns authorised under `setsid -w`.
        if (::setsid() < 0) {
            // The one documented failure is EPERM, when we are already a process group leader.
            // Harmless here, and not worth aborting a spawn over: the goal is to have no
            // controlling terminal, and a process group leader has none to lose. Checked rather
            // than cast away, because a return value dropped on purpose carries its reason.
        }

        // pkexec chdir()s to the target user's home unless --keep-cwd, and TREATS FAILURE AS
        // FATAL. Doing it here instead is deterministic, and leaves the root process pinning no
        // directory that could later be unmounted.
        if (::chdir("/") != 0)
            ::_exit(kChildChdirFailed);

        std::vector<char *> args;
        args.reserve(argv.size() + 1);
        for (const std::string &a : argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);

        // EMPTY ENVIRONMENT. Nothing is forwarded at all.
        //
        // pkexec clearenv()s anyway, but it validates SHELL against /etc/shells FIRST and
        // hard-fails on a mismatch -- so an inherited environment is a way for an unrelated user
        // setting to stop the tool working, with a message about /etc/shells that has nothing to
        // do with CPU power. DISPLAY and XAUTHORITY used to be forwarded so a password window
        // could open; there is no password window any more, polkit is reached over the system
        // bus, and a root process on the user's display is what this design removed.
        char *const envp[] = {nullptr};

        // execve with an argv array. No shell anywhere in this path.
        ::execve(args[0], args.data(), envp);
        ::_exit(kChildExecFailed);
    }

    // ---- parent ----
    ::close(sv[1]);
    mPid = pid;
    mFd = sv[0];
    return true;
}

//------------------------------------------------------------------------
// One rung of the ladder: spawn, wait for the helper's greeting, and on failure tear the child
// down and say why in terms the next layer can print.
//
// `dismissed` is set when the failure was the USER SAYING NO, as distinct from the machine not
// granting it. That distinction decides whether the ladder is allowed to keep climbing: falling
// through to another mechanism after an explicit refusal would be a way of asking the same
// question until it gets the answer it wants.
bool HelperSession::tryRung(Method how, const std::vector<std::string> &argv, std::string &why,
                            bool &dismissed)
{
    dismissed = false;

    if (!spawn(argv)) {
        why = mError; // spawn() has already said which syscall failed
        return false;
    }
    mMethod = how;

    // The helper greets us once it has probed the machine. The timeout is generous because an
    // administrator MAY have overridden our action with a rule that challenges, in which case a
    // real person is being asked a real question by an agent we do not control.
    if (readLine(mGreeting, 120000) && mGreeting.compare(0, 5, "READY") == 0)
        return true;

    // Capture BOTH of these before stop(), which resets mMethod to None and is what made an
    // earlier version of this branch dead code: it tested mMethod after clearing it, so the
    // specific message could never be produced.
    const std::string got = mGreeting;
    stop();
    const int status = mLastStatus;
    mGreeting.clear();

    why = "the privileged helper did not start";
    if (how == Method::Polkit && WIFEXITED(status)) {
        // pkexec's own exit codes. Distinguishing them is the whole reason the child's internal
        // failure codes were moved off 126 and 127.
        switch (WEXITSTATUS(status)) {
            case 126:
                why = "the authorisation request was dismissed.";
                dismissed = true;
                break;
            case 127:
                why = std::string("this session is not authorised to adjust CPU power settings "
                                  "(polkit action ") +
                      polkitActionId() +
                      ").\nThis is expected for a remote login or a session that is not the one "
                      "currently on screen.";
                break;
            case kChildDup2Failed:
            case kChildChdirFailed:
            case kChildExecFailed:
                why = "could not start pkexec.";
                break;
            default:
                break;
        }
    }
    if (!got.empty())
        why += " (it said: " + got + ")";
    return false;
}

//------------------------------------------------------------------------
std::string HelperSession::socketPath()
{
    // The directory is a compile-time constant and the only variable part is our own uid, read
    // from the kernel. Nothing here comes from the environment or from a caller: this is the
    // name of a socket belonging to a root process, and a name somebody else can influence is a
    // name somebody else can stand in front of.
    char buf[128];
    const int n = ::snprintf(buf, sizeof(buf), "%s/helper-%lu.sock", CPUPOWER_RUNTIME_DIR,
                             static_cast<unsigned long>(::getuid()));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf))
        return std::string();
    return std::string(buf);
}

//------------------------------------------------------------------------
bool HelperSession::adopt()
{
    const std::string path = socketPath();
    if (path.empty())
        return false;

    struct sockaddr_un addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
        return false;
    ::memcpy(addr.sun_path, path.c_str(), path.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;

    // A failure here is the ORDINARY case -- no helper is running -- so it is not recorded as an
    // error. Nothing has been started yet, and start() goes on to the privilege ladder.
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }

    mFd = fd;
    mPid = -1; // not our child: it outlived the window that started it
    mMethod = Method::Adopted;
    mPending.clear();

    // The greeting is the proof. A resident helper that is busy with another client answers
    // "ERR busy" instead, and a socket left behind by something else answers nothing at all --
    // in both cases this is not our session and we must not pretend it is.
    if (readLine(mGreeting, 5000) && mGreeting.compare(0, 5, "READY") == 0)
        return true;

    const std::string got = mGreeting;
    ::close(mFd);
    mFd = -1;
    mMethod = Method::None;
    mGreeting.clear();
    mPending.clear();
    if (got.compare(0, 8, "ERR busy") == 0)
        mError = "another CPU-Power window already has the helper.";
    return false;
}

//------------------------------------------------------------------------
bool HelperSession::detach()
{
    if (!running())
        return false;

    std::vector<std::string> lines;
    if (!exchange("DETACH", lines)) {
        mError = "the helper refused to detach";
        if (!lines.empty())
            mError += ": " + lines.back();
        return false;
    }

    // Close WITHOUT waiting. There is deliberately no waitpid here even when the helper is our
    // child: it is supposed to still be running, and waiting for a process that is not going to
    // exit would hang the window on the way out.
    if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
    }
    mPid = -1;
    mMethod = Method::None;
    mPending.clear();
    return true;
}

//------------------------------------------------------------------------
bool HelperSession::start()
{
    mError.clear();

    // A helper left running by an earlier window IS the session, so re-attaching comes before
    // anything else -- including the binary check, which is about starting a new one. Two
    // helpers on one machine would each hold their own idea of the state to restore, and
    // whichever exited last would put back settings the other had already replaced.
    if (adopt())
        return true;
    mError.clear(); // "nothing to adopt" is the ordinary case, not a failure to report

    const std::string helper = helperPath();
    if (!checkHelperBinary(helper))
        return false;

    // ONE WAY IN. Either we are already root, or polkit authorises us for the installed action
    // without asking anybody anything. There is no third route and no fallback, and the route
    // taken is RECORDED rather than assumed, because a privilege path that is used but never
    // asserted is one that can change silently.
    //
    // A `sudo -n` rung lived here and was deleted rather than repaired. It existed for machines
    // with no polkit, where the fix is installing one package, and what it cost was a second
    // privilege path with different properties: a sudoers grant is not limited to the person at
    // the machine, so it reaches that user over SSH and from anything they can start. It also
    // needed a worked NOPASSWD example to be safe, and an example of a root grant is a thing
    // people copy without reading.
    //
    // Its removal took two live bugs with it, both found only by sitting down to document it --
    // a `sudo -n -v` capability probe that sudoers(5) says cannot work for a narrow rule, and a
    // ladder that never actually fell through to the rung. Neither had ever run.
    const char *pkexec = nullptr;
    bool polkitSilent = false; // read again when building the failure message, so it stays in scope
    std::string why;
    bool dismissed = false;

    if (::geteuid() == 0) {
        // 1. Already root: nothing to escalate through.
        if (tryRung(Method::Direct, {helper, pkexecArgv1()}, why, dismissed))
            return true;
    } else {
        pkexec = findPkexec();

        // Ask polkit, BEFORE using pkexec, whether it will authorise this without asking anybody
        // anything. See polkitAuthorisesSilently(): if the answer is no, using pkexec would put a
        // root password dialog on the user's screen, and this program's whole promise is that it
        // never does that. A missing pkcheck counts as no -- it ships with polkitd, so its
        // absence means polkit cannot be answering anyway, and the safe direction is to skip.
        std::string spec;
        const char *pkcheck = findPkcheck();
        if (pkexec != nullptr && pkcheck != nullptr && selfProcessSpec(spec))
            polkitSilent = polkitAuthorisesSilently(pkcheck, polkitActionId(), spec);

        // 2. polkit. For a locally seated user this prompts for nothing at all, because the
        //    installed action's allow_active is yes.
        //
        //    --disable-internal-agent is MANDATORY, not a tidiness flag. Without it, a challenge
        //    result makes pkexec register a text authentication agent that reads stdin and writes
        //    stdout -- which are the protocol channel. That would inject prompt text into the
        //    protocol, and fd 1 carries protocol responses and nothing else. With it, that
        //    cannot happen.
        //
        //    It is NOT a promise that nothing prompts. The flag stops pkexec registering its own
        //    text agent; it says nothing about an agent the desktop session already registered,
        //    and on a desktop there usually is one. If this action ever stops matching, the
        //    auth_admin fallback reaches that agent and a password dialog appears. What keeps
        //    this tool silent is the action matching -- the flag only keeps a prompt off fd 0/1.
        //
        //    --keep-cwd stops pkexec chdir()ing to the target's home, which it treats as a fatal
        //    error if it fails; the child has already done its own chdir("/").
        if (pkexec != nullptr && polkitSilent &&
            tryRung(Method::Polkit,
                    {pkexec, "--disable-internal-agent", "--keep-cwd", helper, pkexecArgv1()}, why,
                    dismissed))
            return true;
    }

    // Nothing worked. Say what to do about it, in terms someone can act on. A bare "permission
    // denied" here would be a dead end for the one person who could fix it.
    mError = "cannot reach the privileged helper, and this program will never run itself as "
             "root.\n";
    if (!why.empty())
        mError += why + "\n";
    if (::geteuid() != 0) {
        if (!pkexec) {
            mError += "pkexec is not installed. Installing polkit is the intended fix: it is what "
                      "lets the person at this machine adjust their own CPU without a password.\n";
        } else if (!polkitSilent) {
            mError += std::string("pkexec is installed, but polkit would not authorise ") +
                      polkitActionId() +
                      " without asking\nfor a password, so it was not used -- this program never "
                      "asks for one.\nUsually that means the action file is not installed where "
                      "polkitd looks (/usr/share/polkit-1/actions),\nor it is there but polkitd "
                      "could not parse it, or this is not a local, active session.\n";
        } else if (!dismissed) {
            mError += std::string("pkexec is installed and this session is authorised for ") +
                      polkitActionId() + ", but starting the helper through it failed.\n";
        }
    }
    // Not an invitation to wire sudo back in: it is the one instruction that works on any machine,
    // for a person who is already an administrator and is standing at it.
    mError +=
        "You can always start the helper yourself, as root:\n  " + helper + " " + pkexecArgv1();
    return false;
}

//------------------------------------------------------------------------
bool HelperSession::startUnprivileged(const std::string &helperPath_, const std::string &sysroot)
{
    mError.clear();

    // Both paths are made ABSOLUTE before the exec, because the child chdir()s to "/" and a
    // relative path would then resolve against the wrong directory -- silently, in the case of
    // --sysroot, which is how this was found: the helper started fine and read the wrong tree.
    //
    // Resolving here rather than skipping the chdir on this path is deliberate. The privileged
    // and unprivileged spawns must do the SAME things to the child, or the gates stop testing
    // what actually ships.
    auto absolute = [](const std::string &in) -> std::string {
        if (in.empty() || in[0] == '/')
            return in;
        char *r = ::realpath(in.c_str(), nullptr);
        if (!r)
            return in; // let the exec or the helper fail with a real error about a real path
        std::string out(r);
        ::free(r);
        return out;
    };

    std::vector<std::string> argv = {absolute(helperPath_), pkexecArgv1()};
    if (!sysroot.empty()) {
        argv.push_back("--sysroot");
        argv.push_back(absolute(sysroot));
    }
    if (!spawn(argv))
        return false;

    mMethod = Method::Unprivileged;

    if (!readLine(mGreeting, 5000) || mGreeting.compare(0, 5, "READY") != 0) {
        const std::string got = mGreeting;
        stop();
        mError =
            "the helper did not start" + (got.empty() ? std::string() : " (it said: " + got + ")");
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
bool HelperSession::send(const std::string &command)
{
    if (mFd < 0)
        return false;

    const std::string line = command + "\n";
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = ::write(mFd, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

//------------------------------------------------------------------------
bool HelperSession::readLine(std::string &out, int timeoutMs)
{
    out.clear();
    if (mFd < 0)
        return false;

    for (;;) {
        const size_t nl = mPending.find('\n');
        if (nl != std::string::npos) {
            out = mPending.substr(0, nl);
            mPending.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r')
                out.pop_back();
            return true;
        }

        struct pollfd pfd = {mFd, POLLIN, 0};
        const int r = ::poll(&pfd, 1, timeoutMs);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false; // timeout

        char buf[1024];
        const ssize_t n = ::read(mFd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false; // the helper exited
        mPending.append(buf, static_cast<size_t>(n));

        // A peer that never sends a newline must not make this grow without bound.
        if (mPending.size() > 1u << 20) {
            mPending.clear();
            return false;
        }
    }
}

//------------------------------------------------------------------------
bool HelperSession::exchange(const std::string &command, std::vector<std::string> &lines,
                             int timeoutMs)
{
    lines.clear();
    if (!send(command))
        return false;

    for (;;) {
        std::string line;
        if (!readLine(line, timeoutMs))
            return false;
        lines.push_back(line);
        if (line.compare(0, 2, "OK") == 0)
            return true;
        if (line.compare(0, 3, "ERR") == 0)
            return false;
    }
}

//------------------------------------------------------------------------
void HelperSession::stop()
{
    if (mFd >= 0) {
        // Closing the socket is the shutdown signal: the helper sees EOF and restores.
        ::close(mFd);
        mFd = -1;
    }
    if (mPid > 0) {
        // Wait, so that by the time this returns the restore has actually happened rather than
        // being merely in progress. An ADOPTED helper has no pid here and cannot be waited for --
        // it is nobody's child -- so its restore completes just after this returns instead.
        int status = 0;
        while (::waitpid(mPid, &status, 0) < 0 && errno == EINTR) {
        }
        mLastStatus = status;
        mPid = -1;
    }
    mMethod = Method::None;
    mPending.clear();
}

} // namespace cpupower
