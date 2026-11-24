/*
 * qjsc-esp [-s|-ss] [-n name] in.js out.bc
 *
 * Compile one ES module to raw QuickJS bytecode, the same thing
 * qjs_esp_bc_compile() does on the device. Unlike qjsc, imports aren't
 * resolved here: the device resolves si:*, qjs:* and friends when it runs
 * the module. Bundle local files into one first (esbuild/bun build).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static void dump_error(JSContext *ctx)
{
    JSValue ex = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, ex);
    fprintf(stderr, "%s\n", msg ? msg : "(error)");
    JS_FreeCString(ctx, msg);
    JSValue stack = JS_IsObject(ex) ? JS_GetPropertyStr(ctx, ex, "stack") : JS_UNDEFINED;
    if (JS_IsString(stack)) {
        const char *s = JS_ToCString(ctx, stack);
        fprintf(stderr, "%s", s ? s : "");
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, ex);
}

static int dummy_init(JSContext *ctx, JSModuleDef *m)
{
    abort();    /* never run, we only compile */
    return -1;
}

/* "si:target", "qjs:os"...: the device provides it, a placeholder will do.
 * Anything else is a file that should have been bundled in. */
static JSModuleDef *loader(JSContext *ctx, const char *name, void *opaque)
{
    if (strchr(name, ':')) {
        return JS_NewCModule(ctx, name, dummy_init);
    }
    JS_ThrowReferenceError(ctx, "import '%s': bundle local files into one module first", name);
    return NULL;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc(n + 1) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;     /* JS_Eval wants it terminated */
        *len = n;
    }
    return buf;
}

int main(int argc, char **argv)
{
    int strip = 0, i = 1;
    const char *name = NULL;

    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-s")) {
            strip = 1;
        } else if (!strcmp(argv[i], "-ss")) {
            strip = 2;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            name = argv[++i];
        } else {
            break;
        }
    }
    if (argc - i != 2) {
        fprintf(stderr, "usage: qjsc-esp [-s|-ss] [-n name] in.js out.bc\n");
        return 2;
    }
    const char *in = argv[i], *out = argv[i + 1];

    size_t len;
    char *src = read_file(in, &len);
    if (!src) {
        perror(in);
        return 1;
    }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetModuleLoaderFunc(rt, NULL, loader, NULL);
    JSValue fn = JS_Eval(ctx, src, len, name ? name : in, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);
    if (JS_IsException(fn)) {
        dump_error(ctx);
        return 1;
    }

    int flags = JS_WRITE_OBJ_BYTECODE;
    if (strip >= 1) {
        flags |= JS_WRITE_OBJ_STRIP_SOURCE;
    }
    if (strip >= 2) {
        flags |= JS_WRITE_OBJ_STRIP_DEBUG;
    }
    size_t bc_len;
    uint8_t *bc = JS_WriteObject(ctx, &bc_len, fn, flags);
    JS_FreeValue(ctx, fn);
    if (!bc) {
        dump_error(ctx);
        return 1;
    }

    FILE *f = fopen(out, "wb");
    int ok = f && fwrite(bc, 1, bc_len, f) == bc_len;
    ok = f && fclose(f) == 0 && ok;
    if (!ok) {
        perror(out);
    }
    js_free(ctx, bc);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ok ? 0 : 1;
}
