#include "current-process.hh"
#include "environment-variables.hh"
#include "finally.hh"
#include "logging.hh"
#include "processes.hh"
#include "strings.hh"
#include "serialise.hh"
#include "signals.hh"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
# include <sys/syscall.h>
#endif

#ifdef __linux__
# include <sys/prctl.h>
# include <sys/mman.h>
#endif


namespace nix {

Pid::Pid()
{
}


Pid::Pid(Pid && other) : pid(other.pid), separatePG(other.separatePG), killSignal(other.killSignal)
{
    other.pid = -1;
}


Pid & Pid::operator=(Pid && other)
{
    Pid tmp(std::move(other));
    std::swap(pid, tmp.pid);
    std::swap(separatePG, tmp.separatePG);
    std::swap(killSignal, tmp.killSignal);
    return *this;
}


Pid::~Pid() noexcept(false)
{
    if (pid != -1) kill();
}


int Pid::kill()
{
    assert(pid != -1);

    debug("killing process %1%", pid);

    /* Send the requested signal to the child.  If it has its own
       process group, send the signal to every process in the child
       process group (which hopefully includes *all* its children). */
    if (::kill(separatePG ? -pid : pid, killSignal) != 0) {
        /* On BSDs, killing a process group will return EPERM if all
           processes in the group are zombies (or something like
           that). So try to detect and ignore that situation. */
#if __FreeBSD__ || __APPLE__
        if (errno != EPERM || ::kill(pid, 0) != 0)
#endif
            logError(SysError("killing process %d", pid).info());
    }

    return wait();
}


int Pid::wait()
{
    assert(pid != -1);
    while (1) {
        int status;
        int res = waitpid(pid, &status, 0);
        if (res == pid) {
            pid = -1;
            return status;
        }
        if (errno != EINTR)
            throw SysError("cannot get exit status of PID %d", pid);
        checkInterrupt();
    }
}


void Pid::setSeparatePG(bool separatePG)
{
    this->separatePG = separatePG;
}


void Pid::setKillSignal(int signal)
{
    this->killSignal = signal;
}


pid_t Pid::release()
{
    pid_t p = pid;
    pid = -1;
    return p;
}


void killUser(uid_t uid)
{
    debug("killing all processes running under uid '%1%'", uid);

    assert(uid != 0); /* just to be safe... */

    /* The system call kill(-1, sig) sends the signal `sig' to all
       users to which the current process can send signals.  So we
       fork a process, switch to uid, and send a mass kill. */

    Pid pid{startProcess([&]() {

        if (setuid(uid) == -1)
            throw SysError("setting uid");

        while (true) {
#ifdef __APPLE__
            /* OSX's kill syscall takes a third parameter that, among
               other things, determines if kill(-1, signo) affects the
               calling process. In the OSX libc, it's set to true,
               which means "follow POSIX", which we don't want here
                 */
            if (syscall(SYS_kill, -1, SIGKILL, false) == 0) break;
#else
            if (kill(-1, SIGKILL) == 0) break;
#endif
            if (errno == ESRCH || errno == EPERM) break; /* no more processes */
            if (errno != EINTR)
                throw SysError("cannot kill processes for uid '%1%'", uid);
        }

        _exit(0);
    })};

    int status = pid.wait();
    if (status != 0)
        throw Error("cannot kill processes for uid '%1%': %2%", uid, statusToString(status));

    /* !!! We should really do some check to make sure that there are
       no processes left running under `uid', but there is no portable
       way to do so (I think).  The most reliable way may be `ps -eo
       uid | grep -q $uid'. */
}


//////////////////////////////////////////////////////////////////////


static pid_t doFork(std::function<void()> fun)
{
    pid_t pid = fork();
    if (pid != 0) return pid;
    fun();
    abort();
}

#if __linux__
static int childEntry(void * arg)
{
    auto main = static_cast<std::function<void()> *>(arg);
    (*main)();
    return 1;
}
#endif


Pid startProcess(std::function<void()> fun, const ProcessOptions & options)
{
    std::function<void()> wrapper = [&]() {
        logger = makeSimpleLogger();
        try {
#if __linux__
            if (options.dieWithParent && prctl(PR_SET_PDEATHSIG, SIGKILL) == -1)
                throw SysError("setting death signal");
#endif
            fun();
        } catch (std::exception & e) {
            try {
                std::cerr << options.errorPrefix << e.what() << "\n";
            } catch (...) { }
        } catch (...) { }
        if (options.runExitHandlers)
            exit(1);
        else
            _exit(1);
    };

    pid_t pid = -1;

    if (options.cloneFlags) {
        #ifdef __linux__
        // Not supported, since then we don't know when to free the stack.
        assert(!(options.cloneFlags & CLONE_VM));

        size_t stackSize = 1 * 1024 * 1024;
        auto stack = static_cast<char *>(mmap(0, stackSize,
            PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));
        if (stack == MAP_FAILED) throw SysError("allocating stack");

        Finally freeStack([&]() { munmap(stack, stackSize); });

        pid = clone(childEntry, stack + stackSize, options.cloneFlags | SIGCHLD, &wrapper);
        #else
        throw Error("clone flags are only supported on Linux");
        #endif
    } else
        pid = doFork(wrapper);

    if (pid == -1) throw SysError("unable to fork");

    return Pid{pid};
}

std::string runProgram(Path program, bool searchPath, const Strings & args, bool isInteractive)
{
    auto res = runProgram(RunOptions {.program = program, .searchPath = searchPath, .args = args, .isInteractive = isInteractive});

    if (!statusOk(res.first))
        throw ExecError(res.first, "program '%1%' %2%", program, statusToString(res.first));

    return res.second;
}

// Output = error code + "standard out" output stream
std::pair<int, std::string> runProgram(RunOptions && options)
{
    options.captureStdout = true;

    int status = 0;
    std::string stdout;

    try {
        auto proc = runProgram2(options);
        Finally const _wait([&] { proc.wait(); });
        stdout = proc.getStdout()->drain();
    } catch (ExecError & e) {
        status = e.status;
    }

    return {status, std::move(stdout)};
}

RunningProgram::RunningProgram(PathView program, Pid pid, AutoCloseFD stdout)
    : program(program)
    , pid(std::move(pid))
    , stdoutSource(stdout ? std::make_unique<FdSource>(stdout.get()) : nullptr)
    , stdout_(std::move(stdout))
{
}

RunningProgram::~RunningProgram()
{
    if (pid) {
        // we will not kill a subprocess because we *can't* kill a subprocess
        // reliably without placing it in its own process group, and cleaning
        // up a subprocess only when `separatePG` is set is a loaded footgun.
        assert(false && "destroying un-wait()ed running process");
        std::terminate();
    }
}

void RunningProgram::wait()
{
    if (std::uncaught_exceptions() == 0) {
        int status = pid.wait();
        if (status)
            throw ExecError(status, "program '%1%' %2%", program, statusToString(status));
    } else {
        pid.kill();
        debug("killed subprocess %1% during exception handling", program);
    }
}

RunningProgram runProgram2(const RunOptions & options)
{
    checkInterrupt();

    /* Create a pipe. */
    Pipe out;
    if (options.captureStdout) out.create();

    ProcessOptions processOptions;

    std::optional<Finally<std::function<void()>>> resumeLoggerDefer;
    if (options.isInteractive) {
        logger->pause();
        resumeLoggerDefer.emplace(
            []() {
                logger->resume();
            }
        );
    }

    /* Fork. */
    Pid pid{startProcess([&]() {
        if (options.environment)
            replaceEnv(*options.environment);
        if (options.captureStdout && dup2(out.writeSide.get(), STDOUT_FILENO) == -1)
            throw SysError("dupping stdout");
        if (options.mergeStderrToStdout)
            if (dup2(STDOUT_FILENO, STDERR_FILENO) == -1)
                throw SysError("cannot dup stdout into stderr");

        if (options.chdir && chdir((*options.chdir).c_str()) == -1)
            throw SysError("chdir failed");
        if (options.gid && setgid(*options.gid) == -1)
            throw SysError("setgid failed");
        /* Drop all other groups if we're setgid. */
        if (options.gid && setgroups(0, 0) == -1)
            throw SysError("setgroups failed");
        if (options.uid && setuid(*options.uid) == -1)
            throw SysError("setuid failed");

        Strings args_(options.args);
        args_.push_front(options.program);

        restoreProcessContext();

        if (options.searchPath)
            execvp(options.program.c_str(), stringsToCharPtrs(args_).data());
            // This allows you to refer to a program with a pathname relative
            // to the PATH variable.
        else
            execv(options.program.c_str(), stringsToCharPtrs(args_).data());

        throw SysError("executing '%1%'", options.program);
    }, processOptions)};

    out.writeSide.close();

    return RunningProgram{
        options.program,
        std::move(pid),
        options.captureStdout ? std::move(out.readSide) : AutoCloseFD{}
    };
}

std::string statusToString(int status)
{
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status))
            return fmt("failed with exit code %1%", WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
#if HAVE_STRSIGNAL
            const char * description = strsignal(sig);
            return fmt("failed due to signal %1% (%2%)", sig, description);
#else
            return fmt("failed due to signal %1%", sig);
#endif
        }
        else
            return "died abnormally";
    } else return "succeeded";
}


bool statusOk(int status)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}
