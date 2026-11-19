/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **C++ on Kosmos** (`docs/webkit.md` W1d): the first C++ in the tree, in
 * an image of its own - `cxxtest.elf`, beside `apptest.elf` - and in no
 * other. What WebKit will lean on, each exercised once:
 *
 * - a global with a constructor, run before anything asked for the kit
 *   (`.init_array`, W1b);
 * - an exception thrown three frames down and caught with its message -
 *   libc++abi's throw, libunwind walking `.eh_frame` by the names
 *   `user/user.ld` gives it;
 * - `std::vector`, `std::map`, `std::string` and `std::unique_ptr`, which
 *   are `operator new` on Kosmos's heap;
 * - a `thread_local`, counted by two threads that each see their own;
 * - and since W4a (`docs/w4.md`), the standard library's threads on the
 *   POSIX layer: `std::thread`, a `std::mutex` four of them share, a
 *   `std::condition_variable` handing values over, a function's static
 *   that four threads race to make and only one makes, and
 *   `steady_clock` across a `sleep_for`;
 * - and since libc++'s locales were turned on (`docs/gpu3d.md` G2), its
 *   streams over Kosmos's one locale: numbers formatted through an
 *   `ostringstream` - in hexadecimal, padded, as a word - parsed back
 *   through an `istringstream`, and a file written with `ofstream` and
 *   read back with `ifstream` through the POSIX layer.
 *
 * No Lua here, and no Kosmos header: C++ sees only the C library and
 * libc++, and `cxxtest_kit.c` is the door - it is C, as every kit's is,
 * and starts the second thread, whose variables have to be made before a
 * function that uses them begins (`tls.h`).
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <mutex>
#include <thread>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int constructed_value;

struct Constructed {
    Constructed() { constructed_value = 7; }
};

Constructed constructed;

/* noinline so the three frames are three frames the unwinder walks. */
[[gnu::noinline]] int three_down(int depth)
{
    if (depth == 3) {
        throw std::runtime_error("three frames down");
    }
    return three_down(depth + 1) + 1;
}

thread_local int counted = 5;

}

/* Adds `times` to this thread's own count, and says what it came to. */
extern "C" int kosmos_cxxtest_count(int times)
{
    for (int i = 0; i < times; i++) {
        counted++;
    }
    return counted;
}

/*
 * "constructed 7, caught 'three frames down', unique 55": the constructor
 * ran, the exception arrived with its message, and the containers added
 * one to ten.
 */
extern "C" int kosmos_cxxtest_run(char *out, unsigned long size)
{
    std::string caught = "nothing";

    try {
        (void)three_down(0);
    } catch (const std::exception &e) {
        caught = e.what();
    }

    std::vector<int> numbers;
    for (int i = 1; i <= 10; i++) {
        numbers.push_back(i);
    }

    std::map<std::string, int> sums;
    for (int n : numbers) {
        sums["all"] += n;
    }

    auto word = std::make_unique<std::string>("unique");
    std::string said = *word + " " + std::to_string(sums["all"]);

    return std::snprintf(out, size, "constructed %d, caught '%s', %s",
                         constructed_value, caught.c_str(), said.c_str());
}

namespace {

std::atomic<int> made_count{0};

struct MadeOnce {
    MadeOnce() { made_count++; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
};

const MadeOnce &made_once()
{
    static MadeOnce one;        /* libc++abi's guard, on the layer's futex */
    return one;
}

}

/*
 * "40000 under a mutex, 100 handed over, made once, slept 20 ms, own 4":
 * four threads each adding 10000 under one mutex, a producer and a
 * consumer through a condition variable, one static made by four racing
 * threads, steady_clock agreeing with sleep_for, and each std::thread's
 * own thread_local.
 */
extern "C" int kosmos_cxxtest_std(char *out, unsigned long size)
{
    std::mutex lock;
    long total = 0;
    std::vector<std::thread> four;

    for (int t = 0; t < 4; t++) {
        four.emplace_back([&] {
            for (int i = 0; i < 10000; i++) {
                std::lock_guard<std::mutex> held(lock);
                total++;
            }
        });
    }

    for (auto &t : four) {
        t.join();
    }

    std::condition_variable ready;
    std::deque<int> queue;
    long handed = 0, sum = 0;
    bool done = false;

    std::thread consumer([&] {
        std::unique_lock<std::mutex> held(lock);

        for (;;) {
            ready.wait(held, [&] { return !queue.empty() || done; });

            while (!queue.empty()) {
                sum += queue.front();
                queue.pop_front();
                handed++;
            }

            if (done) {
                return;
            }
        }
    });

    for (int i = 1; i <= 100; i++) {
        {
            std::lock_guard<std::mutex> held(lock);
            queue.push_back(i);
        }
        ready.notify_one();
    }

    {
        std::lock_guard<std::mutex> held(lock);
        done = true;
    }
    ready.notify_one();
    consumer.join();

    std::vector<std::thread> racers;
    for (int t = 0; t < 4; t++) {
        racers.emplace_back([] { (void)made_once(); });
    }
    for (auto &t : racers) {
        t.join();
    }

    auto began = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    long slept = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - began).count();

    std::atomic<int> own{0};
    std::vector<std::thread> counters;
    for (int t = 0; t < 4; t++) {
        counters.emplace_back([&, t] {
            if (kosmos_cxxtest_count(t) == 5 + t) {
                own++;
            }
        });
    }
    for (auto &t : counters) {
        t.join();
    }

    return std::snprintf(out, size, "%ld under a mutex, %ld handed over%s, made %s, "
                         "slept %s, own %d", total, handed, sum == 5050 ? "" : " (wrong sum)",
                         made_count.load() == 1 ? "once" : "more than once",
                         slept >= 20 ? "20 ms" : "less than 20 ms", own.load());
}

/*
 * "wrote '42 ff 3.25 00007 true', read 12 0.5 word, file 'line one' 99":
 * what an ostringstream formats, what an istringstream parses, and a line
 * and a number through a file on /Home.
 */
extern "C" int kosmos_cxxtest_streams(char *out, unsigned long size)
{
    std::ostringstream wrote;
    wrote << 42 << ' ' << std::hex << 255 << std::dec << ' ' << 3.25 << ' '
          << std::setw(5) << std::setfill('0') << 7 << ' ' << std::boolalpha << true;

    std::istringstream parsed("12 0.5 word");
    int whole = 0;
    double half = 0;
    std::string word;
    parsed >> whole >> half >> word;

    const char *path = "/Home/cxxtest-streams.txt";
    {
        std::ofstream file(path);
        file << "line one\n" << 99 << "\n";
    }
    std::ifstream file(path);
    std::string line;
    int number = 0;
    std::getline(file, line);
    file >> number;
    file.close();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    return std::snprintf(out, size, "wrote '%s', read %d %.1f %s, file '%s' %d",
                         wrote.str().c_str(), whole, half, word.c_str(), line.c_str(), number);
}
