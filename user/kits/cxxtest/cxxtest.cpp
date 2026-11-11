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
 * - a `thread_local`, counted by two threads that each see their own.
 *
 * No Lua here, and no Kosmos header: C++ sees only the C library and
 * libc++, and `cxxtest_kit.c` is the door - it is C, as every kit's is,
 * and starts the second thread, whose variables have to be made before a
 * function that uses them begins (`tls.h`).
 */

#include <cstdio>
#include <map>
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
