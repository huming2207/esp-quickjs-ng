# Bytecode

Compile a script once and skip parsing every time it runs. Compile it on a PC, or on the chip and
cache it.

## On a PC

```sh
python tools/qjs_esp_bc.py compile main.js -o main.qbc
python tools/qjs_esp_bc.py compile main.js -o main.qbc --strip source   # drop source text
python tools/qjs_esp_bc.py compile main.js -o main.qbc --strip debug    # line numbers too
```

The first run builds `tools/qjsc-esp` from the `quickjs/` submodule (needs cmake and a C
compiler), so the bytecode always matches the firmware's QuickJS. Output is byte-identical to what
the chip makes from the same source (checked on macOS arm64 vs S3 and S31).

- One ES module per file. Bundle your local imports first (esbuild, `bun build`).
- Imports with a `:` in them (`qjs:os`, `esp:ws`, your C modules) are left alone; the device
  resolves them when the module runs.
- Keep debug info unless you're short on flash, or every error loses its `file:line`.

## On the chip

```c
size_t len;
uint8_t *c = qjs_esp_bc_compile(ctx, src, src_len, "main.js", 0, &len);   // js_free() it
```

Imports must resolve at compile time here, so register your modules first.

## Running it

```c
JSModuleDef *mod;
JSValue ret = qjs_esp_bc_eval(ctx, blob, len, &mod);   // mod may be NULL
```

It's like `JS_Eval()` of the source: you get the module's promise back. `mod` is there for
`JS_GetModuleNamespace()`, to call what the module exports; it lives as long as the context.

## The container

12 bytes of header, then the bytecode:

```
"QJBC" | u8 format (1) | u8 flags (0) | u16 0 | u32 length (LE) | bytecode...
```

`flags` is reserved, for signatures one day.

## Trust

**QuickJS doesn't check bytecode.** A damaged or hostile blob can corrupt any memory, and the arena
won't save you. The container check only catches the wrong file or a cut-off one, and QuickJS
refuses bytecode from another version (`BC_VERSION`). There's no signature yet, so only load
bytecode from somewhere nobody else can write.

## Kconfig

- `QJS_ESP_BC_COMPILE`: compiling on the chip. Turn it off if bytecode always comes from a PC.
- `QJS_ESP_BC_LOAD`: `qjs_esp_bc_eval()`.
