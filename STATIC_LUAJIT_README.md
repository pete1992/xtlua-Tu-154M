# Prebuilt static LuaJIT in the Visual Studio build

Both x64 configurations in `XTLua/xtlua.vcxproj` link this exact prebuilt archive:

```text
..\lua_sdk\luajit_static.lib
```

In the current workspace this resolves to:

```text
C:\Users\Administrator\source\repos\xtlua-Tu-154\lua_sdk\luajit_static.lib
```

LuaJIT is not compiled, generated, copied or renamed by the project. The
automatic LuaJIT build targets, helper script and their associated test were
removed. The public headers are again taken from `lua_sdk`.

XTLua's existing CRT settings remain `/MT` for Release and `/MTd` for Debug.
These settings do not change the CRT or ABI of the prebuilt archive. The
solution's existing Debug-to-Release mapping is unchanged. The separate
MinGW/Termux CMake configuration is unchanged.

The existing `lua_sdk/luajit_static.lib` archive is used directly and remains
unchanged. Read-only binary inspection found 70 AMD64 COFF object members with
actual Lua API definitions and `LIBCMT`/`OLDNAMES` default-library directives.
This is a static Release-CRT (`/MT`) library, not a Lua DLL import library.
The previous GNU stack-protector and MinGW stdio-import references are absent.

The archive's CRT selection matches XTLua Release. A direct build of the Debug
project configuration still uses `/MTd` and needs a matching Debug archive or
a separately agreed CRT configuration change. The existing solution mapping
of Debug to the Release project avoids that mismatch in normal solution builds.
No runtime settings were silently changed to accommodate the new archive.

A successful native link and runtime test are still pending. There is no
fallback to `lua51.lib`, `libluajit.a`, or a source rebuild.

Only project configuration was checked. No compilation, linking, or simulator
test was performed, and existing plugin binaries were not changed.

## Windows x64 security options

Both Release and Debug explicitly configure these project options:

| Option | Compiler | Linker |
| --- | --- | --- |
| Stack-cookie protection | `/GS` (`BufferSecurityCheck=true`) | CRT support |
| Control Flow Guard | `/guard:cf` (`ControlFlowGuard=Guard`) | `/guard:cf` |
| EH continuation metadata | `/guard:ehcont` | `/guard:ehcont` |
| ASLR, required for CFG | - | `/DYNAMICBASE` (`RandomizedBaseAddress=true`) |

EHCONT is passed through `AdditionalOptions`: the installed ClangCL/LLD MSBuild
integration does not forward the dedicated EHCONT property. The correct option
name is `/guard:ehcont`, not `cfguard:ehcrt`.

`/GS` is the stack protector. The installed ClangCL 22.1.3 maps it to its strong
stack-protector heuristic. `/Gs` (lowercase s) instead controls stack probing.

These settings do not rebuild or modify `lua_sdk/luajit_static.lib`, and do not
retrofit CFG/EHCONT instrumentation into that archive or JIT-generated code.
The archive already contains stack-cookie checks. `/CETCOMPAT` is not added;
hardware shadow-stack compatibility is a separate concern from these options.
Configuration checks are not a native-link or runtime test.
