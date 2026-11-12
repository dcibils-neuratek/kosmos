/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **WTF on Kosmos** (`docs/w4.md` W4c): what JavaScriptCore stands on,
 * asked for one thing each and answered as one sentence of plain ASCII,
 * which `tools/run_wtf.py` holds to the sentence it must be.
 *
 * - **text**: a String made from UTF-8, measured, upper-cased and joined
 *   with a number - WTF's own strings, which are UTF-16 where they must be;
 * - **a hash map** of a thousand strings to their squares, every one found;
 * - **threads**: four WTF threads counting into one atomic;
 * - **a thread stopped**: a WTF thread spinning, suspended by Nebula through
 *   the POSIX layer (`pthread_suspend_np`, patch 0002), its registers read
 *   while it stands - its stack pointer inside its own stack - and let go.
 *   That the counter *moves* is measured before the suspension and after
 *   it, so "held" means something: a suspension that did nothing would be
 *   seen moving;
 * - **the run loop**: three functions queued, a fourth sent from another
 *   thread, which sets a timer - so the order and the wait say the queue is
 *   first come first served, a wake crosses threads and a timer waits;
 * - **rounding**: a thread rounding down, and the thread that started it
 *   still rounding to nearest - the mode is the thread's (`fenv.c`);
 * - **files**, through WTF's FileSystem and so libc++'s std::filesystem on
 *   the POSIX layer: folders made, a file written and copied (`sendfile`),
 *   listed (`opendir`), measured, a temporary file made (`mkostemp`), and
 *   the whole tree removed (`openat`, `fdopendir`, `unlinkat`).
 */

#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

#include <atomic>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

using namespace WTF;

static void wait_ms(unsigned ms)
{
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };

    nanosleep(&t, nullptr);
}

static String text()
{
    const char *utf8 = "Kosmos runs WebKit, se\xc3\xb1or";
    String s = String::fromUTF8(std::span<const char>(utf8, strlen(utf8)));

    return makeString("text="_s, s.length(), '/', s.utf8().length(), ' ',
                      s.convertToASCIIUppercase().left(18), ' ', String::number(382));
}

static String hashmap()
{
    HashMap<String, unsigned> squares;
    unsigned long long sum = 0;
    unsigned found = 0;

    for (unsigned i = 0; i < 1000; i++) {
        squares.add(makeString("key"_s, i), i * i);
    }

    for (unsigned i = 0; i < 1000; i++) {
        auto it = squares.find(makeString("key"_s, i));
        if (it != squares.end()) {
            found++;
            sum += it->value;
        }
    }

    return makeString(" map="_s, squares.size(), '/', found, '/', sum,
                      squares.contains("key1000"_s) ? "/stray"_s : ""_s);
}

static String threads()
{
    std::atomic<unsigned> count { 0 };
    Vector<Ref<Thread>> workers;

    for (int t = 0; t < 4; t++) {
        workers.append(Thread::create("wtftest counter"_s, [&] {
            for (int i = 0; i < 100000; i++) {
                count.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    for (auto& w : workers) {
        w->waitForCompletion();
    }

    return makeString(" threads="_s, count.load());
}

/* Whether `count` moves from where it is, within two seconds: waiting
 * for the thing rather than for a number of milliseconds. 30 ms was the
 * number, and in a gate of 117 suites sharing the Mac the spinner was not
 * given its processor inside them once (`testing.md` 18.783). */
static bool moves(std::atomic<unsigned long>& count)
{
    unsigned long from = count.load();

    for (int ms = 0; ms < 2000; ms++) {
        wait_ms(1);
        if (count.load() != from)
            return true;
    }
    return false;
}

static String suspended()
{
    std::atomic<unsigned long> count { 0 };
    std::atomic<bool> done { false };
    bool moved_before, held, moved_after, inside = false;

    Ref<Thread> spinner = Thread::create("wtftest spinner"_s, [&] {
        while (!done.load(std::memory_order_relaxed)) {
            count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (count.load() == 0) {
        wait_ms(1);
    }

    moved_before = moves(count);

    {
        ThreadSuspendLocker locker;
        auto result = spinner->suspend(locker);

        if (!result) {
            done = true;
            spinner->waitForCompletion();
            return " suspend=refused"_s;
        }

        unsigned long b = count.load();
        wait_ms(30);
        held = count.load() == b;

        PlatformRegisters regs;
        spinner->getRegisters(locker, regs);
#if defined(__x86_64__)
        void *sp = reinterpret_cast<void *>(regs.rsp);
#else
        void *sp = reinterpret_cast<void *>(regs.sp);
#endif
        inside = spinner->stack().contains(sp);

        spinner->resume(locker);
    }

    moved_after = moves(count);

    done = true;
    spinner->waitForCompletion();

    return makeString(" suspend="_s, moved_before ? "moved"_s : "still"_s, '/',
                      held ? "held"_s : "ran"_s, '/', inside ? "sp-in-stack"_s : "sp-elsewhere"_s, '/',
                      moved_after ? "moved"_s : "still"_s);
}

static String runloop()
{
    StringBuilder order;
    MonotonicTime started;
    Seconds waited;
    RefPtr<RunLoop::DispatchTimer> timer;
    RunLoop& main = RunLoop::currentSingleton();

    main.dispatch([&] { order.append('a'); });
    main.dispatch([&] { order.append('b'); });
    main.dispatch([&] { order.append('c'); });

    Ref<Thread> sender = Thread::create("wtftest sender"_s, [&] {
        RunLoop::mainSingleton().dispatch([&] {
            order.append('x');
            started = MonotonicTime::now();
            timer = RunLoop::mainSingleton().dispatchAfter(30_ms, [&] {
                waited = MonotonicTime::now() - started;
                order.append('t');
                RunLoop::mainSingleton().stop();
            });
        });
    });

    RunLoop::run();
    sender->waitForCompletion();

    return makeString(" runloop="_s, order.toString(), '/', waited >= 30_ms ? "waited"_s : "early"_s);
}

static String rounding()
{
    // 1/10, because its nearest double lies above it, so rounding down
    // gives the one below; 1/3's and 2/3's nearest lie below them, and
    // rounding down agrees with nearest there.
    volatile double one = 1.0, ten = 10.0;
    double near = one / ten, down = 0;
    int mode_there = -1;

    Ref<Thread> t = Thread::create("wtftest rounding"_s, [&] {
        fesetround(FE_DOWNWARD);
        mode_there = fegetround();
        down = one / ten;
    });
    t->waitForCompletion();

    bool mine_kept = fegetround() == FE_TONEAREST && one / ten == near;

    return makeString(" rounding="_s, mode_there == FE_DOWNWARD && down < near ? "down"_s : "same"_s,
                      '/', mine_kept ? "nearest-here"_s : "leaked"_s);
}

static String files()
{
    String top = "/Temporary/wtftest"_s;
    String deep = FileSystem::pathByAppendingComponent(
        FileSystem::pathByAppendingComponent(top, "a"_s), "b"_s);
    String one = FileSystem::pathByAppendingComponent(deep, "one.txt"_s);
    String two = FileSystem::pathByAppendingComponent(top, "two.txt"_s);
    const char words[] = "Kosmos is an operating system.";
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(words), sizeof(words) - 1);

    FileSystem::deleteNonEmptyDirectory(top);

    bool made = FileSystem::makeAllDirectories(deep);
    auto wrote = FileSystem::overwriteEntireFile(one, bytes);
    bool copied = FileSystem::copyFile(two, one);
    auto size = FileSystem::fileSize(two);
    auto back = FileSystem::readEntireFile(two);
    bool same = back && back->size() == bytes.size()
        && memcmp(back->span().data(), bytes.data(), bytes.size()) == 0;

    auto names = FileSystem::listDirectory(top);
    std::sort(names.begin(), names.end(), [](const String& x, const String& y) {
        return codePointCompareLessThan(x, y);
    });
    StringBuilder listed;
    for (auto& n : names) {
        if (!listed.isEmpty()) {
            listed.append(',');
        }
        listed.append(n);
    }

    auto kind = FileSystem::fileType(FileSystem::pathByAppendingComponent(top, "a"_s));
    String temporary = FileSystem::createTemporaryFile("wtftest"_s);
    bool temp_made = !temporary.isNull() && FileSystem::fileExists(temporary);
    if (temp_made) {
        FileSystem::deleteFile(temporary);
    }

    bool removed = FileSystem::deleteNonEmptyDirectory(top) && !FileSystem::fileExists(top);

    return makeString(" files="_s, made ? "made"_s : "unmade"_s, '/',
                      wrote ? *wrote : 0, '/', copied ? "copied"_s : "uncopied"_s, '/',
                      size ? *size : 0, '/', same ? "same"_s : "different"_s, '/',
                      listed.toString(), '/',
                      kind && *kind == FileSystem::FileType::Directory ? "dir"_s : "notdir"_s, '/',
                      temp_made ? "temporary"_s : "notemporary"_s, '/',
                      removed ? "removed"_s : "left"_s);
}

extern "C" int kosmos_wtftest_run(char *out, unsigned long size)
{
    WTF::initializeMainThread();

    String said = makeString("wtf "_s, text(), hashmap(), threads(), suspended(), runloop(),
                             rounding(), files());
    CString utf8 = said.utf8();

    snprintf(out, size, "%s", utf8.data());
    return 0;
}
