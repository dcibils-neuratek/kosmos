/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The CTS's `main` on Kosmos (`docs/gpu3d.md` G2): what `tcuMain.cpp` does,
 * as a function the Vulkan conformance kit (`user/kits/vkcts/`) calls with
 * the suite's own command line - `--deqp-case=dEQP-VK.api.smoke.*` and the
 * rest - and that answers with the run's counts rather than an exit code,
 * so the program that asked can say what passed.
 *
 * The cases' archive - the data files a group reads - is the folder named
 * by `--deqp-archive-dir`, and the log is the file `--deqp-log-filename`
 * names, as on every platform: the namespace is what resolves both.
 */

#include "tcuDefs.hpp"
#include "tcuCommandLine.hpp"
#include "tcuPlatform.hpp"
#include "tcuApp.hpp"
#include "tcuResource.hpp"
#include "tcuTestLog.hpp"
#include "tcuTestPackage.hpp"
#include "deUniquePtr.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

#include <pthread.h>

tcu::Platform *createPlatform(void);

/* What a run counted, in the order `kosmos_vkcts_run` fills it. */
enum
{
    KOSMOS_VKCTS_EXECUTED,
    KOSMOS_VKCTS_PASSED,
    KOSMOS_VKCTS_FAILED,
    KOSMOS_VKCTS_NOT_SUPPORTED,
    KOSMOS_VKCTS_WARNINGS,
    KOSMOS_VKCTS_WAIVED,
    KOSMOS_VKCTS_DEVICE_LOST,
    KOSMOS_VKCTS_COMPLETE,
    KOSMOS_VKCTS_COUNTS
};

/* What one run is given and gives back, across to the thread it runs on. */
struct Run
{
    int argc;
    char **argv;
    int *counts;
    char *why;
    unsigned long why_size;
    int answer;
};

static void *run(void *arg)
{
    Run *r = static_cast<Run *>(arg);

    try
    {
        tcu::CommandLine cmdLine(r->argc, r->argv);
        tcu::DirArchive archive(cmdLine.getArchiveDir());
        tcu::TestLog log(cmdLine.getLogFileName(), cmdLine.getLogFlags());
        de::UniquePtr<tcu::Platform> platform(createPlatform());
        de::UniquePtr<tcu::App> app(new tcu::App(*platform, archive, log, cmdLine));

        while (app->iterate())
            ;

        const tcu::TestRunStatus &result = app->getResult();
        r->counts[KOSMOS_VKCTS_EXECUTED]      = result.numExecuted;
        r->counts[KOSMOS_VKCTS_PASSED]        = result.numPassed;
        r->counts[KOSMOS_VKCTS_FAILED]        = result.numFailed;
        r->counts[KOSMOS_VKCTS_NOT_SUPPORTED] = result.numNotSupported;
        r->counts[KOSMOS_VKCTS_WARNINGS]      = result.numWarnings;
        r->counts[KOSMOS_VKCTS_WAIVED]        = result.numWaived;
        r->counts[KOSMOS_VKCTS_DEVICE_LOST]   = result.numDeviceLost;
        r->counts[KOSMOS_VKCTS_COMPLETE]      = result.isComplete ? 1 : 0;
        r->answer                             = 0;
    }
    catch (const std::exception &e)
    {
        std::snprintf(r->why, r->why_size, "%s", e.what());
        r->answer = -1;
    }
    return nullptr;
}

/*
 * 0 when the run came to its end, and -1 with the exception's sentence in
 * `why` when the framework threw before it did.
 *
 * **On a thread of its own**, because the suite was written for Linux's
 * eight-megabyte main stack: `allocateManyPrimaryBuffers` keeps 10 000
 * command buffers in a 320 KB frame, and a Kosmos process's first thread
 * has 256 KB. A thread's stack is a megabyte, its pages given as they are
 * touched (`kosmos.h`, `USER_THREAD_SLOT`); the stacks' sizes themselves are
 * `roadmap.md`'s to change.
 */
extern "C" int kosmos_vkcts_run(int argc, char **argv, int counts[KOSMOS_VKCTS_COUNTS], char *why,
                                unsigned long why_size)
{
    for (int i = 0; i < KOSMOS_VKCTS_COUNTS; i++)
        counts[i] = 0;
    why[0] = '\0';

    Run r = {argc, argv, counts, why, why_size, -1};
    pthread_t thread;

    if (pthread_create(&thread, nullptr, run, &r) != 0)
    {
        std::snprintf(why, why_size, "no thread to run the suite on");
        return -1;
    }
    pthread_join(thread, nullptr);
    return r.answer;
}
