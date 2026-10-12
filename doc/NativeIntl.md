# Static ICU Intl provider

Emscripten, Linux and Apple builds can select the bundled provider with
`HERMES_ENABLE_INTL=ON` and `HERMES_USE_STATIC_INTL_PROVIDER=ON`. Builds without that option
retain their existing platform providers. Host compiler executables do not need
Intl enabled to compile JavaScript that uses the target's native Intl objects.
Native runtime validation can enable it to execute the same provider and data as
the Wasm target, including when the host has a different platform ICU installed.

The adapter and ICU source derive from Microsoft's Hermes fork at
`680ae7651c2f55cbbacaed0cc11519fe6c253f72`. The static port excludes Windows
provider discovery and fallback. The original Microsoft MIT notices and
`external/icu-small/LICENSE` apply. The imported ICU metadata retains its
upstream provenance; Windows export/resource templates are unused by this build.

The provider includes ICU 78.2, CLDR 48 and timezone data 2025c. Its complete
locale dataset is authenticated during configuration and embedded in
`libhermesStaticIcu.a`, together with the native code. Consumers link that
archive alongside the Hermes and JSI archives. Changing application JavaScript
does not require regenerating the data object or rebuilding ICU.

The runtime exposes DateTimeFormat, NumberFormat, Collator and
getCanonicalLocales. Number and date formatters include parts and ranges.
Option reads, coercion and locale resolution follow the native binding rather
than a JavaScript formatter shim. This is not a complete ECMA-402 implementation:
additional Intl constructor families are absent, and output compatibility still
depends on the reference engine, ICU/CLDR and timezone-data versions.

The provider sets the default locale to en-US and default timezone to UTC once
per Wasm instance, before its first use. Host locale and timezone preferences do
not select these defaults. Explicit supported locale and timezone options still
resolve through ICU data.

The provider owns ICU for the lifetime of the Wasm instance. Multiple Hermes
runtimes may be created and destroyed while ICU caches remain alive. Full
`u_cleanup()` is only valid after all runtimes are destroyed and immediately
before discarding the instance; reinitialization after cleanup is not supported.
In native builds, the provider and its caches instead belong to the process;
the same restriction on cleanup and reinitialization applies.

`HERMES_ICU_FAIL_STOP_ALLOCATION` defaults to ON. Failed nonzero ICU allocation,
reallocation and overflowing allocation sizes trap immediately. A host must
discard a trapped instance without invoking guest cleanup or executing more
JavaScript. Returning null from ICU allocation is not a supported recovery
contract. Disabling this option is reserved for allocation diagnostics.
