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
 * functions more here.
 *
 * Promises' reactions - what `async` and `await` become - run when the
 * outermost call into the engine returns, so a script's `then` has printed
 * by the time `JSEvaluateScript` comes back.
 */

#include <JavaScriptCore/JavaScript.h>
#include <wtf/FileSystem.h>
#include <wtf/text/WTFString.h>

#include <stdio.h>
#include <string>

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

/* Runs `source`, named `where` in its messages; 0 when it threw nothing. */
static int run(const std::string& source, const char *where, char **out, unsigned long *length)
{
    said.clear();

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
