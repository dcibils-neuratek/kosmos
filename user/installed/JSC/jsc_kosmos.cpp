/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **JavaScriptCore, driven** (`docs/w4.md` W4d): a script run by WebKit's
 * engine - its parser, its bytecode and the LLInt that interprets it - in
 * a global object of its own, through JavaScriptCore's public C API, the
 * one an embedder uses. What the script prints with `print` is collected
 * and handed back; an exception it throws is handed back as a sentence.
 *
 * **Why not `jsc.cpp`**, WebKit's own shell: it is 4,700 lines of WebKit's
 * test harness - command-line flags, `$vm`, signal handlers, environment
 * variables a test runner sets - and almost nothing in it is about running
 * a script. The browser will embed the engine, not the shell; this is that
 * embedding at its smallest. test262 (W4e) asks for `$262`, which is a few
 * functions more here, and its runner is the last part of this file.
 *
 * Promises' reactions - what `async` and `await` become - run when the
 * outermost call into the engine returns, so a script's `then` has printed
 * by the time `JSEvaluateScript` comes back.
 */

#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/Options.h>
#include <wtf/FileSystem.h>
#include <wtf/kosmos/KosmosJITPool.h>
#include <wtf/text/WTFString.h>

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>

static std::string said;

static void append(JSContextRef ctx, JSValueRef value, JSValueRef *exception)
{
    JSStringRef text = JSValueToStringCopy(ctx, value, exception);

    if (!text) {
        return;
    }

    size_t most = JSStringGetMaximumUTF8CStringSize(text);
    std::string bytes(most, '\0');
    size_t used = JSStringGetUTF8CString(text, bytes.data(), most);

    said.append(bytes.data(), used ? used - 1 : 0);
    JSStringRelease(text);
}

static JSValueRef print(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t count,
                        const JSValueRef values[], JSValueRef *exception)
{
    for (size_t i = 0; i < count; i++) {
        if (i) {
            said += ' ';
        }
        append(ctx, values[i], exception);
    }
    said += '\n';
    return JSValueMakeUndefined(ctx);
}

/* The thrown value as a sentence: its own text, and the line it was on. */
static void thrown(JSContextRef ctx, JSValueRef exception)
{
    said += "exception: ";
    append(ctx, exception, nullptr);

    if (JSValueIsObject(ctx, exception)) {
        JSStringRef name = JSStringCreateWithUTF8CString("line");
        JSValueRef line = JSObjectGetProperty(ctx, (JSObjectRef)exception, name, nullptr);
        JSStringRelease(name);

        if (JSValueIsNumber(ctx, line)) {
            said += " (line " + std::to_string((long)JSValueToNumber(ctx, line, nullptr)) + ")";
        }
    }
    said += '\n';
}

// Whether a script has run, after which JavaScriptCore's options are fixed.
static bool ran;

/* Runs `source`, named `where` in its messages; 0 when it threw nothing. */
static int run(const std::string& source, const char *where, char **out, unsigned long *length)
{
    said.clear();
    ran = true;

    JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    JSStringRef name = JSStringCreateWithUTF8CString("print");
    JSObjectRef function = JSObjectMakeFunctionWithCallback(ctx, name, print);

    JSObjectSetProperty(ctx, global, name, function, kJSPropertyAttributeNone, nullptr);
    JSStringRelease(name);

    JSStringRef script = JSStringCreateWithUTF8CString(source.c_str());
    JSStringRef url = JSStringCreateWithUTF8CString(where);
    JSValueRef exception = nullptr;

    JSEvaluateScript(ctx, script, nullptr, url, 1, &exception);
    if (exception) {
        thrown(ctx, exception);
    }

    JSStringRelease(script);
    JSStringRelease(url);
    JSGlobalContextRelease(ctx);

    *out = said.data();
    *length = said.size();
    return exception ? 1 : 0;
}

// The JIT off, for timing the same script on the interpreter alone
// (`jsc --no-jit`). JavaScriptCore reads its options from the environment
// elsewhere, and Kosmos has none, so it is a call - before the first VM,
// after which the options are frozen.
extern "C" int kosmos_jsc_interpret_only(void)
{
    if (ran) {
        return -1;
    }
    // Options are read once a process; the first reading takes this
    // callback, and JavaScriptCore's own, at the first VM, finds them read.
    bool set = false;
    JSC::Options::initialize([&] {
        JSC::Options::AllowUnfinalizedAccessScope scope;
        set = JSC::Options::setOptions("useJIT=false");
    });
    return set ? 0 : -1;
}

// The JIT's pool's pages written so far - code made - or -1 with no pool
// (`docs/jit.md`): what says the JIT ran, counted by Nebula.
extern "C" long kosmos_jsc_jit_pages(void)
{
    return kosmosJITPoolPagesWritten();
}

// And the bytes the JIT has written into it, every compilation's.
extern "C" unsigned long kosmos_jsc_jit_bytes(void)
{
    return kosmosJITPoolBytesWritten();
}

extern "C" int kosmos_jsc_eval(const char *source, unsigned long size, char **out, unsigned long *length)
{
    return run(std::string(source, size), "eval", out, length);
}

extern "C" int kosmos_jsc_run(const char *path, char **out, unsigned long *length)
{
    auto bytes = FileSystem::readEntireFile(String::fromUTF8(path));

    if (!bytes) {
        said = "jsc: cannot read ";
        said += path;
        said += '\n';
        *out = said.data();
        *length = said.size();
        return 2;
    }

    std::string source(reinterpret_cast<const char *>(bytes->span().data()), bytes->size());
    return run(source, path, out, length);
}

/*
 * **test262** (`docs/w4.md` W4e). The host composes nothing: it hands a
 * bundle - the harness's files once, then each test with what its
 * front matter said - and the cases are run here, one at a time, each in
 * a global object of its own, in a VM all of them share. A `$262` is put
 * in every global, as the harness asks:
 *
 *   createRealm()          a new global, in the same VM, and its `$262`
 *   evalScript(text)       a script run in that `$262`'s global
 *   detachArrayBuffer(b)   by `ArrayBuffer.prototype.transfer`, which
 *                          detaches the source - the C API has no other door
 *   gc()                   a collection
 *   global                 the global object
 *
 * `agent` is not there: it is threads with VMs of their own, and the one
 * language test that asks for it fails, counted.
 *
 * A bundle's records, each a line and then that many bytes and a newline:
 *
 *   H <name> <bytes>                                     a harness file
 *   T <name> <flags> <phase> <type> <includes> <bytes>   a test
 *
 * tab-separated; `flags` is a sum - 1 onlyStrict, 2 noStrict, 4 raw,
 * 8 async - and `-` stands for an empty field.
 */

/* A collection that has finished when it returns. `JSGarbageCollect` only
 * says that garbage was left behind, and leaves when to the heap. */
extern "C" void JSSynchronousGarbageCollectForDebugging(JSContextRef);

namespace {

struct Case {
    std::string name;
    unsigned flags = 0;
    std::string phase, type;
    std::vector<std::string> includes;
    std::string source;
};

std::map<std::string, std::string> harness;
std::vector<Case> cases;
JSContextGroupRef group;
std::vector<JSGlobalContextRef> realms;
JSClassRef realmFunction;

enum { ONLY_STRICT = 1, NO_STRICT = 2, RAW = 4, ASYNC = 8 };
enum { COLLECT_EVERY = 20 };

std::string text(JSContextRef ctx, JSValueRef value)
{
    std::string kept;
    kept.swap(said);
    append(ctx, value, nullptr);
    std::string got;
    got.swap(said);
    said.swap(kept);
    return got;
}

JSValueRef get(JSContextRef ctx, JSValueRef object, const char *key)
{
    if (!JSValueIsObject(ctx, object)) {
        return JSValueMakeUndefined(ctx);
    }
    JSStringRef name = JSStringCreateWithUTF8CString(key);
    JSValueRef value = JSObjectGetProperty(ctx, (JSObjectRef)object, name, nullptr);
    JSStringRelease(name);
    return value;
}

void set(JSContextRef ctx, JSObjectRef object, const char *key, JSValueRef value)
{
    JSStringRef name = JSStringCreateWithUTF8CString(key);
    JSObjectSetProperty(ctx, object, name, value, kJSPropertyAttributeDontEnum, nullptr);
    JSStringRelease(name);
}

/* What a thrown value is called: its constructor's name, as test262's own
 * runners read it. */
std::string kind(JSContextRef ctx, JSValueRef exception)
{
    JSValueRef name = get(ctx, get(ctx, exception, "constructor"), "name");
    return JSValueIsString(ctx, name) ? text(ctx, name) : std::string("(not an object)");
}

JSValueRef evaluate(JSContextRef ctx, const std::string& source, JSValueRef *exception)
{
    JSStringRef script = JSStringCreateWithUTF8CString(source.c_str());
    JSValueRef value = JSEvaluateScript(ctx, script, nullptr, nullptr, 1, exception);
    JSStringRelease(script);
    return value ? value : JSValueMakeUndefined(ctx);
}

/* Three of `$262`'s functions, told apart by what was kept in them. */
enum Which { CREATE_REALM, EVAL_SCRIPT, GC };

struct Door {
    Which which;
    JSGlobalContextRef realm;
};

JSObjectRef make262(JSGlobalContextRef realm);

JSValueRef call262(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t count,
                   const JSValueRef values[], JSValueRef *exception)
{
    Door *door = static_cast<Door *>(JSObjectGetPrivate(function));

    switch (door->which) {
    case CREATE_REALM: {
        JSGlobalContextRef realm = JSGlobalContextCreateInGroup(group, nullptr);
        realms.push_back(realm);
        return make262(realm);
    }
    case EVAL_SCRIPT: {
        std::string source = count ? text(ctx, values[0]) : std::string();
        JSValueRef thrown = nullptr;
        JSValueRef value = evaluate(door->realm, source, &thrown);
        if (thrown) {
            *exception = thrown;
            return JSValueMakeUndefined(ctx);
        }
        return value;
    }
    case GC:
        JSSynchronousGarbageCollectForDebugging(ctx);
        return JSValueMakeUndefined(ctx);
    }
    return JSValueMakeUndefined(ctx);
}

void finalize262(JSObjectRef function)
{
    delete static_cast<Door *>(JSObjectGetPrivate(function));
}

/* `print` and `$262` in a global; `$262` returned. */
JSObjectRef make262(JSGlobalContextRef realm)
{
    if (!realmFunction) {
        JSClassDefinition definition = kJSClassDefinitionEmpty;
        definition.className = "Function262";
        definition.callAsFunction = call262;
        definition.finalize = finalize262;
        realmFunction = JSClassCreate(&definition);
    }

    JSObjectRef global = JSContextGetGlobalObject(realm);
    JSStringRef name = JSStringCreateWithUTF8CString("print");
    JSObjectSetProperty(realm, global, name, JSObjectMakeFunctionWithCallback(realm, name, print),
                        kJSPropertyAttributeDontEnum, nullptr);
    JSStringRelease(name);

    JSObjectRef dollar = JSObjectMake(realm, nullptr, nullptr);
    set(realm, dollar, "createRealm", JSObjectMake(realm, realmFunction, new Door { CREATE_REALM, realm }));
    set(realm, dollar, "evalScript", JSObjectMake(realm, realmFunction, new Door { EVAL_SCRIPT, realm }));
    set(realm, dollar, "gc", JSObjectMake(realm, realmFunction, new Door { GC, realm }));
    set(realm, dollar, "global", global);

    /* Taken now, so a test that replaces `transfer` cannot unmake it. */
    set(realm, dollar, "detachArrayBuffer",
        evaluate(realm, "(function (transfer) { return function detachArrayBuffer(buffer) {"
                        " transfer.call(buffer); return null; }; })(ArrayBuffer.prototype.transfer)",
                 nullptr));

    set(realm, global, "$262", dollar);
    return dollar;
}

/* One run of a case: "" when it passed, else why not. */
std::string attempt(const Case& c, bool strict)
{
    std::string source = strict ? "\"use strict\";\n" : "";

    if (!(c.flags & RAW)) {
        source += harness["assert.js"];
        source += harness["sta.js"];
        if (c.flags & ASYNC) {
            source += harness["doneprintHandle.js"];
        }
        for (auto& include : c.includes) {
            auto found = harness.find(include);
            if (found == harness.end()) {
                return "no harness file " + include;
            }
            source += found->second;
        }
    }
    source += c.source;

    said.clear();
    JSGlobalContextRef ctx = JSGlobalContextCreateInGroup(group, nullptr);
    make262(ctx);

    JSStringRef script = JSStringCreateWithUTF8CString(source.c_str());
    JSValueRef exception = nullptr;
    std::string why;

    if (c.phase == "parse") {
        if (JSCheckScriptSyntax(ctx, script, nullptr, 1, &exception)) {
            why = "parsed, and should have thrown " + c.type;
        } else if (kind(ctx, exception) != c.type) {
            why = "threw " + kind(ctx, exception) + " at parse, not " + c.type + ": " + text(ctx, exception);
        }
    } else {
        JSEvaluateScript(ctx, script, nullptr, nullptr, 1, &exception);

        if (!c.phase.empty()) {
            if (!exception) {
                why = "ran, and should have thrown " + c.type;
            } else if (kind(ctx, exception) != c.type) {
                why = "threw " + kind(ctx, exception) + ", not " + c.type + ": " + text(ctx, exception);
            }
        } else if (exception) {
            why = "threw " + text(ctx, exception);
        } else if (c.flags & ASYNC) {
            size_t failed = said.find("Test262:AsyncTestFailure:");
            if (failed != std::string::npos) {
                why = said.substr(failed + 25, said.find('\n', failed) - failed - 25);
            } else if (said.find("Test262:AsyncTestComplete") == std::string::npos) {
                why = "never said it was done";
            }
        }
    }

    JSStringRelease(script);
    JSGlobalContextRelease(ctx);
    for (auto realm : realms) {
        JSGlobalContextRelease(realm);
    }
    realms.clear();

    for (char& ch : why) {
        if (ch == '\n' || ch == '\r') {
            ch = ' ';
        }
    }
    return why.size() > 300 ? why.substr(0, 300) : why;
}

std::vector<std::string> fields(const std::string& line)
{
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        size_t tab = line.find('\t', at);
        out.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) {
            return out;
        }
        at = tab + 1;
    }
}

std::string orNone(const std::string& field)
{
    return field == "-" ? std::string() : field;
}

}

/* Reads a bundle: how many tests are in it, or -1. */
extern "C" long kosmos_jsc_t262_open(const char *path)
{
    auto bytes = FileSystem::readEntireFile(String::fromUTF8(path));

    if (!bytes) {
        return -1;
    }

    std::string all(reinterpret_cast<const char *>(bytes->span().data()), bytes->size());
    size_t at = 0;
    harness.clear();
    cases.clear();

    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) {
            return -1;
        }
        auto f = fields(all.substr(at, end - at));
        at = end + 1;

        size_t size = strtoul(f.back().c_str(), nullptr, 10);
        if (at + size > all.size()) {
            return -1;
        }
        std::string body = all.substr(at, size);
        at += size + 1;

        if (f[0] == "H" && f.size() == 3) {
            harness[f[1]] = body;
        } else if (f[0] == "T" && f.size() == 7) {
            Case c;
            c.name = f[1];
            c.flags = strtoul(f[2].c_str(), nullptr, 10);
            c.phase = orNone(f[3]);
            c.type = orNone(f[4]);
            std::string list = orNone(f[5]);
            for (size_t from = 0; from < list.size();) {
                size_t comma = list.find(',', from);
                c.includes.push_back(list.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
                from = comma == std::string::npos ? list.size() : comma + 1;
            }
            c.source = std::move(body);
            cases.push_back(std::move(c));
        } else {
            return -1;
        }
    }

    if (!group) {
        group = JSContextGroupCreate();
    }
    return (long)cases.size();
}

/* Runs the case numbered `index`, from 1, in each mode its flags allow:
 * 0 when every run passed, and why in `out` when one did not. */
extern "C" int kosmos_jsc_t262_case(long index, char **out, unsigned long *length)
{
    static std::string why;

    if (index < 1 || (size_t)index > cases.size()) {
        why = "no such case";
    } else {
        const Case& c = cases[index - 1];
        why.clear();

        if (!(c.flags & ONLY_STRICT)) {
            std::string sloppy = attempt(c, false);
            if (!sloppy.empty()) {
                why = "(sloppy) " + sloppy;
            }
        }
        if (why.empty() && !(c.flags & (NO_STRICT | RAW))) {
            std::string strict = attempt(c, true);
            if (!strict.empty()) {
                why = "(strict) " + strict;
            }
        }
    }

    /* **And the VM collected, every few cases.** Each case leaves a global
     * object and everything its script made, and the heap, told only that
     * there is garbage, grew past what a 512 MB machine has: one case in
     * about 800 ended the program, and each of those passed alone. */
    static unsigned since;
    if (++since == COLLECT_EVERY) {
        JSGlobalContextRef ctx = JSGlobalContextCreateInGroup(group, nullptr);
        JSSynchronousGarbageCollectForDebugging(ctx);
        JSGlobalContextRelease(ctx);
        since = 0;
    }

    *out = why.data();
    *length = why.size();
    return why.empty() ? 0 : 1;
}
