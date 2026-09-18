// HelperSession — starting the one process that runs as root, and talking to it.
//
// THIS FILE IS THE SHARPEST EDGE IN THE PROJECT. It is the single place where something this
// code chooses becomes a root process, so every rule of the privilege contract lands here:
//
//   * The helper's path is a COMPILE-TIME ABSOLUTE CONSTANT (CPUPOWER_HELPER_PATH). Never
//     argv[0], never /proc/self/exe, never $PATH, never an environment override, never anything
//     relative. This is the difference between a tool and a privilege-escalation vector.
//
//   * execve with an argv array. NO SHELL, ANYWHERE, EVER -- no system(), no popen(), no
//     `sudo sh -c`. There is no string for a metacharacter to live in.
//
//   * setsid() in the child before the exec, so the helper stays out of the launching shell's
//     foreground process group and has no controlling terminal. It is SAFE under polkit, which
//     was measured rather than assumed: pkexec authorises its PARENT via getppid(), and setsid()
//     does not reparent. (It had a second justification once -- keeping sudo's use_pty from
//     putting a pty on the protocol channel -- which went away with the sudo rung. The remaining
//     reason stands on its own, and the helper's isatty() refusal is still the backstop.)
//
//   * The helper is lstat'ed before it is spawned: regular file, uid 0, not a symlink, not group-
//     or world-writable. The real gate is that $prefix/libexec is root-owned; this catches a
//     broken install loudly instead of running something unexpected as root.
//
//   * The child is exec'd with an EMPTY environment. Nothing is forwarded -- not DISPLAY, not
//     XAUTHORITY. polkit is reached over the system bus and needs neither, and a privileged
//     process on the user's display is the thing this design removed.
//
// HOW ROOT IS REACHED, and why it is a ladder rather than one mechanism: privilege is a
// CAPABILITY PROBE, in the same spirit as the cpufreq model. The rung that
// worked is reported, never assumed, so a machine that lands on a different one says so.
//
// The session is LONG-LIVED: one helper process per GUI session, spoken to over a socketpair.
// On the polkit path that is not about prompts -- there are none -- but it still matters: each
// new session would re-snapshot a different pre-state, and the restore contract (below) depends
// on one snapshot per session.
//
// Closing the socket is the shutdown signal. The helper restores the machine on EOF, so there is
// no path -- including this process being SIGKILLed -- that leaves the machine changed. THAT IS
// THE ONLY SHUTDOWN TRIGGER THERE IS: pkexec sets PR_SET_PDEATHSIG, but it never reaches the
// helper, because pkexec clears it again at its own setregid/setreuid before the exec. Measured,
// not inferred. Nothing here may be weakened on the assumption something else would catch it.

#pragma once

#include <string>
#include <vector>

#include <sys/types.h>

namespace cpupower
{

class HelperSession
{
public:
    enum class Method {
        None,         // not started
        Direct,       // we are already root
        Polkit,       // pkexec, authorised by the installed action: no prompt for a locally
                      // seated user
        Unprivileged, // testing only: the helper run directly, unprivileged
    };

    // A short stable token for the chosen rung -- "polkit" or "direct". The gates assert on this,
    // because a privilege path that is used but never asserted is one that can silently change
    //.
    static const char *methodName(Method m);

    HelperSession() = default;
    ~HelperSession();

    HelperSession(const HelperSession &) = delete;
    HelperSession &operator=(const HelperSession &) = delete;

    // Start the privileged helper, choosing the least intrusive route that works:
    //
    //   1. euid == 0               -> exec the helper directly
    //   2. polkit ALREADY says yes -> pkexec (no prompt at all for a locally seated user)
    //   3. neither                 -> fail with a message naming what to do about it
    //
    // Rung 2 is "polkit already says yes", NOT "pkexec exists". The difference is the whole of
    // the no-password promise: if the action is not registered, pkexec does not fail, it falls
    // back to org.freedesktop.policykit.exec and the session's own agent puts a root password
    // dialog on screen. So pkcheck is asked first and only exit 0 is taken as yes.
    //
    // THERE IS ONE WAY IN AND NO FALLBACK, deliberately. A `sudo -n` rung existed and was removed
    // rather than repaired. It served machines without polkit, where the remedy is installing one
    // package, and it cost a second privilege path with DIFFERENT PROPERTIES from the first: a
    // sudoers grant is not limited to the person at the machine, so it also applies over SSH and
    // from any process that user can start. Two doors with different locks is a worse security
    // story than one door, and a much longer one to explain.
    //
    // There is no interactive rung either. This program handles no passwords: the binary that
    // used to draw a password window was deleted rather than hardened.
    //
    // Returns true on success. On failure, error() explains it in terms a user can act on --
    // never a bare errno, and never a silent no-op.
    bool start();

    // Testing only, and never used by the GUI: run a specific helper binary directly, without
    // any privilege escalation at all, optionally against a captured sysfs tree. This is the ONLY
    // entry point that accepts a path, it is unprivileged by construction, and it exists so the
    // gates can drive the real protocol against fixtures.
    //
    // It is also where the pure-EOF restore claim is proven, because it involves no pkexec and so
    // no possibility of a second shutdown trigger confusing the result.
    bool startUnprivileged(const std::string &helperPath, const std::string &sysroot);

    bool running() const
    {
        return mPid > 0 && mFd >= 0;
    }

    Method method() const
    {
        return mMethod;
    }

    const std::string &error() const
    {
        return mError;
    }

    // The helper's greeting line, captured by start().
    const std::string &greeting() const
    {
        return mGreeting;
    }

    // Send one protocol command. A newline is appended here; the caller never supplies one.
    // Returns false if the helper has gone away.
    bool send(const std::string &command);

    // Read one response line. timeoutMs < 0 blocks. Returns false on timeout, EOF or error.
    bool readLine(std::string &out, int timeoutMs = 5000);

    // Send a command and collect response lines until one of them is "OK..." or "ERR...".
    // Returns false if the terminating line was an ERR (which is still returned in `lines`).
    bool exchange(const std::string &command, std::vector<std::string> &lines,
                  int timeoutMs = 5000);

    // Close the pipe and reap. The helper restores the machine on EOF; this waits for it so the
    // restore has actually happened by the time this returns.
    void stop();

    // The compiled-in helper path, for diagnostics and for the gates to assert.
    static const char *helperPath();

    // The polkit action id this build was compiled against, and the argv[1] its exec.argv1
    // annotation pins. Both come from the same build variables that generate the .policy file,
    // so a drift between the two is a build error rather than a silent fallback to a password
    // prompt.
    static const char *polkitActionId();
    static const char *pkexecArgv1();

private:
    bool spawn(const std::vector<std::string> &argv);
    // One rung: spawn, wait for the greeting, tear down and explain on failure. `dismissed` says
    // the person at the keyboard refused, which is the one failure the ladder does not climb past.
    bool tryRung(Method how, const std::vector<std::string> &argv, std::string &why,
                 bool &dismissed);
    bool checkHelperBinary(const std::string &path);

    pid_t mPid = -1;
    int mFd = -1;
    Method mMethod = Method::None;
    int mLastStatus = -1; // waitpid status from the most recent stop(), for diagnosing a failure
    std::string mError;
    std::string mGreeting;
    std::string mPending; // buffered bytes not yet forming a whole line
};

} // namespace cpupower
