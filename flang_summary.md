# Flang, WebAssembly, and an interactive notebook frontend

Status snapshot: 2026-09-27
LLVM base: `upstream/main` at `3489ea3b567c719f497bfcf83348e27cfd79cdb7`

## Executive conclusion

Both goals are feasible, but they have different critical paths.

- The best first deliverable is a native Flang cross-compiler that emits a
  WebAssembly object, followed by an Emscripten link:

  ```sh
  flang --target=wasm32-unknown-emscripten -c test.f90 -o test.o
  emcc test.o <wasm32 libflang-rt> -o test.js
  ```

- The runtime-free compiler slice now works on latest main without statically
  specializing Flang to wasm32: scalar and complex `BIND(C)` objects compile,
  link with Emscripten, and run under Node. Serge Guelton's work and Isabel
  Paredes's maintained recipe proved the route; the local experiment now
  separates the small upstreamable compiler fixes from the remaining runtime
  ABI work.
- The first upstream patches should establish correct target-aware FIR/LLVM
  lowering and object emission. `emflang`, direct `test.js` production, and
  Emscripten packaging should come after the compiler/runtime contract works.
- A Flang notebook frontend can use one owned MLIR `builtin.module` as the
  compilation transaction for each cell. The frontend initially produces
  HLFIR/FIR, but the current MLIR/Wasm engine consumes a fully lowered LLVM
  dialect module. Native ORC and the Wasm side-module loader can share the same
  cell/executor contract even though their final loading mechanisms differ.
- FIR/HLFIR is not Flang's counterpart to LFortran ASR for persistent REPL
  semantics. FIR is a good compilation transaction; declarations, name lookup,
  shadowing, and incomplete cell syntax still need a persistent frontend
  session built from Flang parsing and semantics.

The Wasm object milestone is small. A polished, Python-like Fortran REPL is not
small, although it can be delivered in useful tiers.

## Correct build topology

There are three different products, and conflating them obscures the work.

| Product | Runs on | Produces/runs | Emscripten-forge recipe |
|---|---|---|---|
| Native Flang cross-compiler | macOS/Linux host | wasm32/wasm64 objects | `recipes_native/flang_emscripten-wasm` |
| Target Flang runtime | WebAssembly | Runtime support used by Fortran objects | `recipes_wasm/libflang-rt` |
| LLVM/MLIR tools compiled as Wasm | Browser/Wasm VM | Wasm-resident compilation/JIT-like loading | `recipes_wasm/llvm`, MLIR Python binding work |

The immediate compiler experiment is the first row. It does not require Flang
itself to run in the browser.

## First vertical slice

Use the driver spelling below. `-triple` is an internal `flang -fc1` option;
the public driver spelling is `--target=`. The triple uses hyphens and the
correct spelling is `unknown`.

```sh
flang --target=wasm32-unknown-emscripten -c test.f90 -o test.o
emcc test.o /path/to/libflang_rt.runtime.a -o test.js
node test.js
```

Split this into independently testable checkpoints:

1. Compile a runtime-free `BIND(C)` subroutine into a valid wasm32 object.
2. Compile a program that generates Flang runtime calls and inspect every
   declaration for the correct target ABI.
3. Cross-build `libflang-rt` for wasm32 and link it with `emcc`.
4. Run simple I/O under Node and a browser.
5. Add `emflang` as a convenience driver/wrapper once its exact responsibility
   is known.

A good checkpoint-1 input is intentionally runtime-free:

```fortran
subroutine add_one(x) bind(c)
  use iso_c_binding, only: c_int
  integer(c_int), intent(inout) :: x
  x = x + 1
end subroutine
```

The initial acceptance checks should include:

```sh
flang --target=wasm32-unknown-emscripten -c add_one.f90 -o add_one.o
llvm-readobj --file-headers --symbols add_one.o
wasm-ld -r add_one.o -o combined.o
```

Then add array descriptors, `COMMON`, allocation, complex values, and I/O one
feature at a time.

## What the current driver already does

Flang uses Clang's driver framework. Current Clang selects its existing
`WebAssembly` toolchain for a wasm32/wasm64 triple and passes the effective
triple to `flang -fc1`.

A local `-###` trace already produces the expected shape:

```text
flang -fc1 -triple wasm32-unknown-emscripten -emit-obj ...
wasm-ld -m wasm32 ... -lc ...libclang_rt.builtins.a -o test.wasm
```

But the generic WebAssembly link job does not currently add `libflang-rt`, and
direct `wasm-ld` invocation is not equivalent to the full `emcc` link. `emcc`
selects Emscripten system libraries and emits the JavaScript loader/environment
integration needed by ordinary browser programs.

This is why the initial contract should be “Flang emits the object; `emcc`
links it.” It also matches the integration model suggested by Emscripten
maintainers for other LLVM frontends such as Rust.

## Why Flang currently assumes build == host == target

The LLVM backend itself already cross-compiles. The remaining assumption is
mostly in Flang's frontend/lowering description of the runtime ABI.

### Runtime call signatures

`flang/include/flang/Optimizer/Builder/Runtime/RTBuilder.h` converts C++ runtime
function signatures to MLIR types with expressions such as:

```cpp
8 * sizeof(std::size_t)
8 * sizeof(unsigned long)
sizeof(long double)
```

Those are properties of the C++ compiler building Flang, not properties of the
Fortran output target. The file itself has a TODO stating that its `sizeof`
uses assume build, host, and target are the same.

### Descriptor reflection

`flang/include/flang/Optimizer/CodeGen/DescriptorModel.h` destructures the
host-compiled `CFI_cdesc_t` and maps its C++ field types into generated IR. Its
header explicitly calls this incorrect for cross-compilation and points to the
long-standing cross-compilation issue.

For an LP64 native compiler targeting wasm32 ILP32, the most visible mismatch
is `long`, `size_t`, pointer-sized integers, and descriptor indices.

Flang already has `evaluate::TargetCharacteristics` for semantic properties
such as Fortran kind sizes and alignments, and `TargetSetup.h` already receives
an LLVM `TargetMachine`. That is a useful integration point, but it is not yet
a C ABI model: `TargetSetup.h` still has TODOs to populate type data from the
target data layout and still decides kind-16 support using build-time
`HAS_LDBL128`. The runtime builders therefore cannot simply consume the class
unchanged; it must be extended or accompanied by a target C data-model object.

### Intrinsic modules

`iso_c_binding.f90` currently derives some kinds from host-oriented/default
properties. A wasm32 build needs 32-bit `c_long`, `c_size_t`, `c_intptr_t`, and
`c_ptrdiff_t`. Target-specific intrinsic `.mod` files must be installed and
selected by triple; using a host intrinsic module is not safe.

### Target ABI hooks and index width

FIR-to-LLVM has target ABI classes for supported architectures. Wasm needs a
target class for its complex argument/result ABI and must consistently use the
target pointer/index width rather than a hard-coded or host-derived width.

### Runtime implementation

The runtime must itself be compiled for wasm32. It also exposes platform
assumptions such as pthread-backed locks and strict Wasm function signatures
that native linkers may tolerate or diagnose less strongly.

## Audit of the maintained Emscripten-forge patch series

The current recipe has seven patches. Four have been ported onto a clean latest
main experiment, without cloning or merging the authors' branch.

| Patch | Purpose | Latest-main assessment |
|---|---|---|
| 1 | Add FIR CodeGen target support for wasm32/wasm64 | Small candidate, but the downstream hard-coded complex alignment of 4 is wrong for `complex<f64>`; derive it from target data layout and test f32/f64 |
| 2 | Use target width during FIR-to-LLVM conversion | Small, general cross-compilation fix; good independent candidate |
| 3 | Specialize ABI models for wasm32/wasm64 | Proves the build, but is not upstream-ready because one Flang binary is compiled with one static target data model |
| 4 | Define `__wasm__`, `__wasm32__`, and `__wasm64__` | Small target-predefine patch; should follow existing target-macro conventions |
| 5 | Specialize `iso_c_binding` for wasm32 | Correct values, but a source macro alone is insufficient: the build and driver must generate, install, and select target-specific intrinsic modules |
| 6 | Fix complex-reduction C/C++ signature mismatches | Confirmed general runtime correctness bug, independently reproducible with current Emscripten; propose separately with a focused Wasm link test |
| 7 | Disable pthreads/use pseudo-locks on Wasm | Do not upstream as written: current Emscripten compiled, linked, and ran the existing lock implementation without `-pthread`; an unconditional Wasm pseudo-lock would preclude threaded Wasm |

The experimental branch is `flang-wasm-upstream-spike` with these commits:

```text
fd029ffeaa05 [flang] Add CodeGen target support for WebAssembly
ccfdda0a6608 [flang] Use target index width during FIR-to-LLVM lowering
020a0a47d78a [flang][wasm] Specialize C ABI models for the WebAssembly target
942256e26097 [flang] Define WebAssembly target preprocessor macros
```

Commit `020a0a47d78a` is deliberately a spike. Its `FLANG_WASM64` CMake switch
chooses a single data model when Flang is built. That is useful for reproducing
the recipe, but it does not solve:

```sh
flang --target=x86_64-... a.f90
flang --target=wasm32-... b.f90
```

with the same compiler binary. Upstream should derive ABI types from the
current MLIR module's target triple/data layout or from a target-characteristics
object passed into the builders.

The specialization was subsequently removed from the working tree, exactly
restoring its five files to their pre-`020a0a47d78a` contents. Rebuilding and
repeating the end-to-end tests proved that it is not needed for runtime-free
wasm32/wasm64 object emission. It should not be included in an upstream patch
series; the experimental commit remains only as a record of the downstream
approach.

### Prior-work and review-history audit

Serge's current open LLVM pull-request list contains no Flang/Wasm proposal.
The relevant compiler experiments instead live on the public fork branches
`feature/flang-wasm` and `feature/flang-18-wasm`; neither branch was ever used
as the head of an LLVM pull request.

The newer `feature/flang-wasm` branch is six commits ahead of its 2024 base:

- `8dde5ce975a3` adds minimal wasm32 FIR ABI support. It credits George Stagg's
  original patch and is the ancestor of recipe patch 1.
- `6087d7bb251a` replaces selected host `sizeof` uses with fixed wasm32
  constants. It is the ancestor of the build-time specialization in recipe
  patch 3.
- two commits disable Float128 build pieces rather than modeling target
  capabilities;
- `b65aa9936829` emulates Wasm common linkage using weak symbols;
- the remaining commit documents the cross-build procedure.

These commits are valuable prototypes, but the branch contains no focused
Flang tests and statically specializes one compiler build for one target. That
explains why it is useful as evidence but cannot be upstreamed unchanged.
Attribution should be preserved when submitting descendants: recipe patch 1
originates with George Stagg plus Serge's wasm32 adaptation; recipe patch 3 is
based on Serge's specialization; patches 2 and 4 are Isabel Paredes's newer
work.

Serge did submit the common-linkage experiment as LLVM PR 151478. Review
rejected mapping COMMON to weak because weak symbols do not implement the
required largest-size/largest-alignment resolution rule. The PR was closed in
favor of PR 199303, whose proper object-format and linker implementation is now
on main. This is a useful example of why we should inspect rejected review
history rather than merely replay downstream patches.

Several of Serge's adjacent 32-bit/Emscripten runtime fixes are also already on
main: PRs 99465, 99822, 101242, and 105589. They should be treated as existing
prerequisites, not resubmitted.

No Flang- or Wasm-related LLVM PR authored by Isabel was found. Her current
work is represented by the maintained Emscripten-forge recipe series, where
she rebased and extended the older prototypes for LLVM 23.

## What has already landed upstream

The Wasm backend is not static. Current main already contains important work
that older Flang/Wasm descriptions could not rely on. In particular,
`de5e538331d8` (`[WebAssembly] Implement support for Common symbols`, LLVM PR
199303) added end-to-end common-symbol support in LLVM object emission and
`wasm-ld`. This directly helps Fortran `COMMON` blocks.

That reduces backend scope. The main remaining compiler work is in Flang's
target-aware frontend/lowering and in the target runtime—not a new Wasm code
generator.

## Actionable upstream roadmap for Wasm

The ordering below is the proposed commit and review schedule. Each item should
be rebased onto fresh main, committed by the project author, and pushed only
after its diff and commit message have been reviewed. The experimental commits
are evidence, not the final history.

### Stage A: small independent PRs already supported by experiments

| Order | Atomic change | Readiness | Acceptance evidence | Rough implementation effort |
|---:|---|---|---|---:|
| A1 | Make FIR-to-LLVM aggregate/index lowering use the target index width | Nearly ready; polish the materialization assertion and commit message | i386 produces `i32` dynamic-allocation counts; x86-64 produces `i64`; no Wasm conditional | 1-3 days |
| A2 | Add wasm32/wasm64 FIR CodeGen ABI classes | Nearly ready | `complex<f32>` matches Clang at align 4, `complex<f64>` at align 8; mixed C/Fortran Wasm program runs under Node | 1-3 days |
| A3 | Add target-selected Wasm and Emscripten predefines | Nearly ready | focused wasm32/wasm64 preprocessor checks pass and spellings match Clang | 1 day |
| A4 | Correct the complex-reduction C-wrapper declarations and forwarding | Reproduced; fix and upstream test still needed | current sources make `wasm-ld` report real signature mismatches for `CppSumComplex*` and `CppDotProductComplex*` | 2-5 days |

A1 is now isolated on branch `flang-target-index-width`, based on
`upstream/main` at `75811754a2e6`, committed as `c9d7ebd024f4`, and pushed to
`origin/flang-target-index-width`. Its diff contains only `FIROpPatterns.cpp`,
`TypeConverter.cpp`, and the existing
`convert-to-llvm-target.fir` test. The test passes for i386 (`i32`) and for
x86-64, AArch64, and PowerPC64LE (`i64`); both modified C++ translation units
also pass compile-only checks against the refreshed-main headers.

A2 is isolated independently on branch `flang-wasm-codegen-abi`, also based
directly on `75811754a2e6`; it does not contain A1. Its worktree is
`/private/tmp/llvm-flang-a2-wasm-abi`. The uncommitted two-file change adds the
WebAssembly CodeGen target class and a focused wasm32/wasm64 complex ABI test.

Recommended review narrative:

1. A1 is a general cross-compilation correctness fix, not a Wasm feature.
2. A2 is the minimal target ABI hook that enables valid runtime-free Wasm
   objects. It contains no runtime, driver-wrapper, or packaging changes.
3. A3 is frontend target identity and can be reviewed independently of CodeGen.
4. A4 is a pre-existing runtime bug exposed by Wasm's strict function types;
   it should not be presented as a prerequisite for A1-A3.

Preserve authorship in this series. A2 descends from George Stagg's work and
Serge Guelton's Wasm adaptation; A1 and A3 are Isabel Paredes's recipe work;
A4 is Isabel's fix. The exact final attribution (`From`, `Co-authored-by`, or
an explicit “based on” note) should reflect how much each final diff changes.

### Stage B: remove the actual host-equals-target assumptions

This stage needs a short design discussion before code is split into PRs. The
formatted-I/O experiment gives it a precise first failing case:
`OutputAscii(..., std::size_t)` is declared with `i64` by an LP64 host compiler
when the output target is wasm32 and requires `i32`.

| Order | Increment | Deliverable | Rough effort |
|---:|---|---|---:|
| B1 | Target C ABI model | A small module-scoped model derived from the selected triple/data layout, with tests for native, wasm32, and wasm64 | 1-2 weeks including review design |
| B2 | Target-aware runtime signature construction | Make runtime type builders consume that model; convert one semantic category at a time, starting with `size_t`/pointer-sized values | 2-6 weeks |
| B3 | Target-aware descriptor model | Stop reflecting host `CFI_cdesc_t` field widths into target IR; describe descriptor fields semantically and test wasm32 layout | 2-4 weeks |
| B4 | Target intrinsic modules | Generate/install/select target-specific `iso_c_binding` modules and verify `c_long`, `c_size_t`, `c_intptr_t`, and `c_ptrdiff_t` | 2-4 weeks |

The key design constraint is type identity. Once the host compiler has made
`std::size_t`, `unsigned long`, and a fixed-width alias the same C++ type, a
`getModel<T>()` specialization cannot recover which semantic C spelling the
runtime declaration used. Therefore B2 cannot be a simple replacement of
host `sizeof(T)` by target `sizeof(T)`. It needs explicit semantic categories
or a declarative runtime-signature description. A useful first upstream design
note should compare these two implementation routes before touching all
runtime APIs.

Every B-stage test must use one native Flang binary for more than one output
target. A build-time `FLANG_WASM64` or `FLANG_TARGET_SIZEOF_*` configuration is
specifically outside the acceptance criteria.

### Stage C: target runtime and Emscripten integration

| Order | Increment | Definition of done | Rough effort |
|---:|---|---|---:|
| C1 | Cross-build wasm32 `libflang-rt` | Runtime archive is compiled with Emscripten and links with objects from the native Flang cross-compiler | 2-4 weeks after core B work |
| C2 | Runtime feature ladder | Node tests pass for allocation/descriptors, formatted output, complex reduction, and `COMMON`; unsupported areas are documented | 2-6 weeks |
| C3 | `emflang`/recipe integration | Reproducible `flang -c` + `emcc` workflow, target intrinsic-module discovery, and updated emscripten-forge recipes | 1-3 weeks |
| C4 | Browser validation | The same runtime-backed examples execute in a browser/JupyterLite environment | 1-3 weeks |

Start the runtime single-threaded, but do not hard-code pseudo-locks merely
because the architecture is Wasm. Current Emscripten already supplies a valid
non-`-pthread` path for the existing lock code, and threaded Wasm must remain a
possible later configuration.

### Communication and RFC threshold

Do not open a new RFC for A1-A4. They are small, reviewable fixes with focused
tests. Post a short coordination update on the existing Emscripten issue before
opening the first PR: state what works on latest LLVM main, identify the small
upstream patch queue, and name target-aware runtime ABI construction as the
remaining compiler boundary.

Use an LLVM Discourse design note or RFC only for Stage B, once the proposed
representation and migration plan are concrete enough to compare. That
discussion is justified because it changes a cross-cutting Flang interface,
not merely because Wasm is a new target.

Suggested coordination reply on the existing Emscripten issue:

> We have started validating the same direction against current LLVM main.
> With a small target-aware Flang patch set, one native Flang binary can emit
> both wasm32 and wasm64 objects. Runtime-free `BIND(C)` scalar and complex
> examples link with `emcc` and run under Node, and current LLVM main already
> has proper Wasm `COMMON` support. We are preparing the target-index-width,
> Wasm FIR ABI, and target-predefine changes as separate upstream LLVM patches.
> The remaining compiler boundary is the Flang runtime ABI: today host C++
> types leak into generated signatures (for example, an LP64 host emits `i64`
> for `OutputAscii`'s `size_t` argument when wasm32 requires `i32`). The static
> target specialization in the recipe proves the build but is not suitable for
> an upstream multi-target Flang binary. We plan to propose a target-aware
> runtime ABI/intrinsic-module design after the small patches, and would like
> to coordinate with any overlapping work here and preserve the existing
> authorship.

### Explicitly excluded from the upstream series

- The static `FLANG_WASM64` specialization from experimental commit
  `020a0a47d78a`.
- Serge's weak-symbol emulation for `COMMON`; proper Wasm common-symbol support
  is already on main.
- Recipe patch 7's unconditional Wasm pseudo-lock selection.
- An LLVM backend or `wasm-ld` workaround for Flang object emission; the tested
  runtime-free objects are valid already.
- An all-in-one PR mixing compiler lowering, the runtime, `emflang`, and recipe
  changes.

The `llvm-readobj --unwind`/`--stackmap` crash should be filed and fixed in
parallel as a tooling issue. It is not a dependency of any Flang patch because
supported Wasm views (`--file-headers`, `--symbols`, and `--relocations`) work.

## Local validation status

- A latest-main sparse worktree was created at
  `/private/tmp/llvm-flang-main-sparse` on branch
  `flang-wasm-upstream-spike`.
- The four compiler-side recipe patches above were ported as experimental
  commits. No commit was pushed. The static data-model commit was then exactly
  reversed in the working tree to test the other changes independently.
- A `-###` driver trace confirms current Clang driver infrastructure already
  selects the WebAssembly toolchain and forwards the target to `flang -fc1`.
- The first fresh build was configured with `BUILD_SHARED_LIBS=ON`. That was
  unnecessary for this experiment and made `fir-opt` depend on hundreds of
  individual dylibs; under disk and memory pressure even `fir-opt --version`
  stalled before entering `main`. Only that generated build directory was
  removed. No source, test fixture, worktree, or commit was lost.
- The replacement build is `/private/tmp/flang-wasm-lean-build`. It is a
  Release, static build with only the WebAssembly LLVM backend enabled; tests,
  examples, benchmarks, zlib, zstd, libxml2, and terminfo are disabled.
  `fir-opt`, the Flang driver, `llvm-readobj`, and `FileCheck` were built as
  explicit targets. The build occupies about 3.5 GiB.
- `fir-opt` still requires a broad one-time MLIR build because its CMake target
  deliberately registers every available MLIR dialect and extension. This is
  independent of `LLVM_TARGETS_TO_BUILD=WebAssembly`; NVVM, ROCDL, and SPIR-V
  names in the build log were MLIR dialect libraries, not additional LLVM
  machine backends.
- The corrected `fir-opt` built successfully and `fir-opt --version` returned
  normally in about 1.5 seconds (`LLVM version 24.0.0git`, optimized build).
- Focused tests pass for the wasm32 and wasm64 complex ABI rewrites. They check
  `byval` arguments, `sret` results, four-byte alignment for `complex<f32>`,
  and eight-byte alignment for `complex<f64>`.
- Focused FIR-to-LLVM tests pass for both 32-bit and 64-bit target index widths:
  the dynamic allocation count and `llvm.alloca` operand are `i32` for i386
  and `i64` for x86-64. The initial check incorrectly expected the original
  extent SSA value; it was corrected to capture the intervening multiply.
- Focused preprocessor tests pass for wasm32 and wasm64, including
  `__wasm__`, `__wasm32__`/`__wasm64__`, and `__EMSCRIPTEN__`.
- The six focused commands were rerun directly against the final
  no-specialization build: wasm32/wasm64 target rewriting, i386/x86-64 index
  lowering, and wasm32/wasm64 predefines all pass. The lean build intentionally
  has no generated full-suite `lit.site.cfg.py`, so these use the tests' exact
  `fir-opt`/`flang` and `FileCheck` pipelines rather than a source-tree
  `llvm-lit` invocation.
- The full Flang driver built successfully. Its public `--target=` spelling
  forwards `-fc1 -triple wasm32-unknown-emscripten` as expected.
- With the static specialization removed, the aggregate change from the LLVM
  base is seven files, 151 insertions and seven deletions. `git diff --check`
  passes. No new commit or push was made.
- The first attempt to reuse older split LLVM/MLIR builds failed for a useful
  reason: the LLVM build with Wasm lacked a Flang dependency, while the MLIR
  build lacked the Wasm backend, and its older `mlir-tblgen` could not parse
  current FIR TableGen syntax. This validates using a version-matched build for
  the experiment.

### End-to-end results without static target specialization

The same native Flang binary produced both wasm32 and wasm64 objects.

1. A runtime-free `BIND(C)` `add_one` subroutine compiled with
   `--target=wasm32-unknown-emscripten`. `llvm-readobj` reported `Arch: wasm32`,
   `AddressSize: 32bit`, one defined function, and no runtime references.
   Emscripten 4.0.9 linked it with a C caller, and Node 26 returned zero after
   verifying `41 -> 42`.
2. The same source compiled as wasm64. `llvm-readobj` reported `Arch: wasm64`
   and `AddressSize: 64bit`. This proves that one host compiler can select both
   target data layouts without a build-time `FLANG_WASM64` mode.
3. A complex(kind=8) `BIND(C)` function was compiled and called from C. Its
   LLVM signature used an eight-byte-aligned `sret` result plus eight-byte-
   aligned `byval` operands. Emscripten linked the mixed C/Fortran program and
   Node returned zero after verifying both components of the complex result.
   This directly exercises the new Wasm target ABI hook.
4. A Fortran `COMMON` block emitted a real `BINDING_COMMON` data symbol with
   the correct relocations. A matching current-main `wasm-ld` linked it with a
   C caller, and direct WebAssembly instantiation under Node returned zero.
   No Flang weak-symbol workaround is needed on current main.
5. A formatted-output program still emits a valid wasm32 object, but its LLVM
   declaration is wrong for the target:

   ```llvm
   declare zeroext i1 @_FortranAioOutputAscii(ptr, ptr, i64)
   ```

   The third parameter is `std::size_t` in `io-api.h`; it must be `i32` for
   wasm32. This is the concrete boundary where the LP64 build host leaks into
   target runtime signatures. It confirms that target-driven runtime ABI
   modeling, not the static downstream specialization, is the next central
   compiler task.
6. The current `complex-reduction.c` and the actual C++ sum/dot-product runtime
   sources were independently compiled with Emscripten and relocatably linked.
   `wasm-ld` reports signature mismatches: the C wrapper sees seven parameters
   for `CppSumComplex*` while the definition has six, and eight parameters for
   `CppDotProductComplex*` while the definition has five. This validates recipe
   patch 6 as a general runtime correctness fix, but not as part of the Flang
   target-enablement patches.
7. A standalone program using the current `flang-rt` lock implementation was
   compiled and linked with Emscripten without `-pthread` and ran successfully
   under Node. Recipe patch 7 is therefore neither required for the initial
   runtime nor appropriate as an unconditional architecture policy.

### LLVM tool issue found during validation

Current-main `llvm-readobj` supports Wasm file headers, sections, symbols, and
relocations, but it crashes on a valid checked-in Wasm object when invoked with
`--unwind` or `--stackmap`. `--all` also crashes because it enables unwind
printing. `WasmDumper.cpp` implements both entry points with
`llvm_unreachable("unimplemented")`. This is a general LLVM tool bug, not a
Flang object-emission failure. Until fixed, validation should request the
supported views explicitly rather than use `--all`.

## Interactive Flang: the correct MLIR boundary

For execution, one cell should produce one top-level `builtin.module`, not one
arbitrary function operation. There are two useful MLIR stages; they should not
be conflated:

```text
Fortran cell
  -> parsing + semantic analysis
  -> one owned MLIR builtin.module (HLFIR/FIR frontend result)
  -> Flang MLIR pass pipeline
  -> one LLVM-dialect builtin.module (executor input)
  -> executor selected by environment
       native: translate to LLVM IR + persistent ORC JIT
       Wasm:   translate to LLVM IR + Wasm object + shared side module
               + dlopen/dlsym
```

The patched MLIR/Wasm `WasmExecutionEngine` accepts an `MlirOperation`, but its
implementation immediately calls MLIR-to-LLVM-IR translation. Therefore an
arbitrary HLFIR/FIR module cannot be passed to it directly: Flang's normal
HLFIR/FIR-to-LLVM-dialect pipeline must run first. The cell's callable entry
can be a uniquely named no-argument `BIND(C)` subroutine such as
`flang_repl_cell_7`.

This makes MLIR the right interface between the frontend and both executors,
but it does not by itself provide cross-cell Fortran semantics.

## What Clang-Repl contributes

Clang-Repl does not put all cells into one partial translation unit. Each input
produces a `PartialTranslationUnit` with its own AST translation unit and LLVM
module. A persistent incremental executor loads each module and tracks its
resources.

Concretely, Clang keeps one `CompilerInstance`, `ASTContext`, `Sema`, and
`IncrementalParser` alive. `Parse()` creates a new source buffer and a new
linked `TranslationUnitDecl`; `RegisterPTU()` gives that PTU a fresh LLVM
module; `Execute()` adds the module to one persistent executor. These are two
separate properties: persistent frontend name lookup and one loadable code
module per cell.

### What `-fincremental-extensions` actually does in Clang

Clang-Repl's builder injects `-Xclang -fincremental-extensions`. This is not a
generic "enable JIT" switch. It enables several C/C++ frontend deviations and
lifecycle changes together:

- EOF becomes `annot_repl_input_end`, so the lexer/preprocessor can accept the
  next memory buffer instead of being torn down;
- the parser accepts statements at file scope and represents them as
  `TopLevelStmtDecl` nodes;
- Sema retains its translation-unit scope between partial translation units
  and clears per-PTU tentative-definition state at the cell boundary;
- CodeGen wraps top-level statements in generated `__stmts__N` functions and
  adjusts linkage/re-emission rules for definitions reused by later PTUs.

There is an important declaration/statement split behind that summary.
Ordinary C++ declarations are already valid at translation-unit scope, so
Clang parses them through its normal declaration path. Only input that cannot
be disambiguated as a declaration is sent to `ParseTopLevelStmtDecl()`. That
routine creates a real `TopLevelStmtDecl` AST node and temporarily pushes
function and compound-statement scopes while Sema checks the statement. In
CodeGen, consecutive `TopLevelStmtDecl` nodes are emitted into an internal
`void __stmts__N()` function. That function is added to the module's global
initializer list, so `Interpreter::Execute()` only has to add the PTU to the
persistent executor and run its constructors; the command-line tool does not
look up and call `__stmts__N` itself.

The relevant Clang flow is deliberately interpreter-only:

```text
IncrementalCompilerBuilder
  -> inject -fincremental-extensions
  -> LangOpts.IncrementalExtensions
  -> lexer emits annot_repl_input_end instead of final EOF
  -> persistent IncrementalParser parses another source buffer
       declaration -> normal C++ Decl
       statement   -> TopLevelStmtDecl in a function-like Sema scope
  -> one linked TranslationUnitDecl + one LLVM module form a PTU
  -> CodeGen emits __stmts__N as a global initializer
  -> persistent executor adds the module and runs initializers
```

This is more than textual wrapping, and less than a general relaxation of
C++. Batch Clang behavior is unchanged unless the explicit option is enabled.

### What the Clang model means for Flang-Repl

Flang needs the same *isolation policy* but cannot copy Clang's AST operation
literally. Flang's top-level grammar is `Program`, a list of `ProgramUnit`s.
Its `MainProgram` contains a `SpecificationPart` followed by an
`ExecutionPart` and requires an end-program statement (the `PROGRAM` statement
itself is optional). Therefore neither of these notebook cells is a complete
top-level entity in today's parser:

```fortran
integer :: i
```

```fortran
i = 5
```

This is also why declarations cannot simply be treated as Flang's equivalent
of C++ translation-unit declarations. A Fortran declaration belongs to the
specification part of some program unit. If it is put in a generated
subroutine, its ordinary local storage and interface are the wrong persistent
state; if it is put in a generated module, later cells can import its public
symbol and the JIT can retain its storage.

The right first implementation is consequently:

```text
cell source
  -> CellSourceTransformer (interpreter-only)
       complete ProgramUnit -> leave unchanged
       declarations         -> versioned state module declarations
       procedure             -> module procedure in that state module
       statements            -> unique BIND(C) cell subroutine
       bare expression       -> unique cell subroutine + result display
  -> ordinary Flang parse + semantics + lowering
  -> one independent MLIR module
  -> load module, then explicitly invoke flang_repl_cell_N
```

The generated `flang_repl_cell_N` is Flang's first-stage analogue of Clang's
`__stmts__N`, but explicit invocation is preferable to registering it as a
global constructor. It gives the library a stable entry symbol, keeps
compilation separate from execution, and works with both native ORC lookup and
the Wasm `dlsym` route. It also avoids emitting or invoking `_QQmain` for loose
cells.

This transformation must happen before the current whole-program parser. It
cannot initially be described as an AST/parse-tree transform because invalid
loose input does not produce a `parser::Program` to transform. A later, more
upstream-quality frontend API could expose parsers for an interactive
`SpecificationPart`, `ExecutionPart`, or expression fragment and then build a
synthetic standard `ProgramUnit` in the parse tree. That would replace the
prototype's lexical classification without changing normal Fortran grammar.

An eventual Flang incremental option should therefore cover two independent
capabilities, which should not be conflated:

1. **Fragment syntax:** allow the interpreter to parse declarations,
   executable constructs, and expressions without requiring users to type a
   complete program unit. The result is still normalized into a standard,
   generated module/cell procedure before ordinary semantics and lowering.
2. **Persistent semantics:** retain or parent semantic scopes across cells so
   types, generics, operators, procedures, and shadowing no longer depend only
   on serialized `.mod` interfaces and generated `USE` statements.

The prototype only needs the first behavior through source transformation and
`.mod` state. A `-fincremental-extensions`-style Flang option becomes justified
when the parser itself gains fragment entry points or the semantic context is
kept alive. Until then, an interpreter-library option is clearer than changing
the public Flang driver or accepting loose syntax in batch compilation.

Flang currently has no corresponding interactive/incremental option. The first
complete-program-unit and generated-module milestones do not require one:
every transformed cell is valid standard Fortran, a normal Flang compilation
produces its `.mod` interface and MLIR module, and the session owns the module
directory and persistent executor.

A real persistent-semantic Flang mode will need an explicit opt-in, and using
the familiar `-fincremental-extensions` spelling may be reasonable. Its
implementation cannot simply share Clang's `LangOpts` bit, however. It must
define Flang-specific behavior for cell source boundaries, parse-tree and
cooked-source retention, parented semantic scopes, prior-symbol
externalization, redefinition/rollback, and lowering only the newest cell. The
flag should be added only with the first behavior that needs it, not as an
empty prerequisite.

The reusable ideas are:

- additive module transactions;
- stable session state outside any one module;
- unique per-cell entry points;
- symbol lookup across previously loaded modules;
- resource tracking, rollback, and well-defined redefinition policy;
- separate frontend and executor layers.

The current Clang Wasm executor already follows the side-module route: object
emission, in-process shared linking, `dlopen(RTLD_GLOBAL)`, then `dlsym`.
Sharing it cleanly will require factoring it away from Clang's
`PartialTranslationUnit` type or implementing the same small executor contract
for MLIR modules.

### Longer-term common interactive-compiler architecture

Flang should first establish its own correct frontend transaction. After that,
Clang and Flang can converge below the language-specific semantic layer:

```text
Clang cell frontend              Flang cell frontend
AST/Sema + PTU                   parse/sema + .mod + MLIR module
          \                       /
           compiled transaction
           - LLVM IR or object
           - entry symbol(s)
           - resource/rollback handle
           - diagnostics
                    |
          language-neutral executor
          native ORC | Wasm side-module loader
```

Good common candidates are cell identifiers, transaction/resource tracking,
LLVM module or object loading, symbol lookup, runtime registration, constructor
execution, unload/rollback, and error transport. Syntax relaxation, semantic
scope persistence, `.mod` management, redefinition rules, and value display
remain frontend/language policies.

The present `clang::IncrementalExecutor` API takes a
`PartialTranslationUnit &`, which exposes the coupling: a reusable executor
would instead consume a language-neutral LLVM module/object transaction, with
thin Clang and Flang adapters. MLIR can be Flang's frontend boundary without
being imposed on future LLVM language frontends that do not use MLIR.

### Redefinition and undo

On current main, `%undo` is handled by the standalone
`clang/tools/clang-repl/ClangRepl.cpp`, not LLDB's
`lldb/source/Plugins/REPL/Clang/ClangREPL.cpp`; LLDB uses its own expression
REPL path and does not contain a `%%undo` handler there. The standalone command
calls `Interpreter::Undo(1)`, which performs two reversals:

1. Native ORC removes the newest module through its per-PTU
   `ResourceTracker`.
2. `IncrementalParser::CleanUpPTU()` removes the newest declarations from AST
   lookup and identifier-resolution structures, then withdraws the newest
   `TranslationUnitDecl` from the redeclaration chain.

Undo is deliberately stack-shaped. Removing an arbitrary old cell could leave
newer compiled cells referring to its symbols. Clang's Wasm executor currently
returns `"Not implemented yet"` from `removeModule`, so `%undo` is not yet a
portable property of its native and browser executors.

Clang-Repl generally cannot keep two ordinary definitions of the same variable
alive and make the later one shadow the earlier one. Its tests redefine after
undoing the previous PTU. LFortran instead parents every cell scope to the
previous scope; a declaration in the new scope shadows the old name for future
cells, while already-compiled code keeps its original binding.

Versioned generated Flang state modules can emulate LFortran's behavior for an
important subset before persistent Flang semantics exists:

- Cell 1's declaration of `x` lives in `flang_repl_state_1`.
- A later declaration of `x` lives in `flang_repl_state_7`; it does not replace
  or overwrite Cell 1's storage.
- The session's visible-name index maps `x` to the newest provider, so future
  wrappers generate `USE flang_repl_state_7, ONLY: x`.
- Previously compiled wrappers still name Cell 1's module symbol.
- An assignment-only cell imports and mutates the currently visible `x`; only
  a declaration creates a new binding.

Generated state variables should use their versioned Fortran module symbols,
not a fixed `BIND(C, NAME="x")`, because the latter would collide at link time.
Only the per-cell callable entry needs a unique explicit C binding name.

This was checked with the wasm32 Flang experiment. Two independent cells that
each declare `x` in different generated modules lower to distinct definitions:

```llvm
@_QMflang_repl_state_10Ex = global i32 40
@_QMflang_repl_state_20Ex = global i32 100
```

An observer compiled against the first `.mod` has an external reference to the
first symbol; an observer compiled after rebinding and importing the second
module refers only to the second symbol. The compiler/linkage part of simple
variable shadowing therefore works without modifying Flang semantics.

A Flang cell record should therefore own the loaded-module resource handle,
generated `.mod` files, exported-name bindings, prior binding stack, entry
symbol, and dependencies. Native undo removes the resource tracker and pops
that semantic/interface transaction. Wasm may initially need logical undo—pop
the visible bindings but leave uniquely named old side modules loaded—until
Emscripten supports reliable unloading and symbol removal. Fixed-name exports
must be rejected or treated more conservatively in that mode.

## LFortran versus Flang

LFortran's ASR is a typed semantic representation with symbol tables. Its
evaluator can retain a pristine semantic scope for every cell, parent a new
scope to the previous cell, mark prior variables external, implement
shadowing, and copy/relink semantic symbols before destructive lowering passes.

The current evaluator implementation makes this separation explicit. Each
cell receives a new translation-unit symbol table whose parent is the previous
cell; the semantic scope is copied before ASR passes mutate the cell; earlier
variables are marked external; duplicate helper bodies are dropped before a
new LLVM module is added to the persistent evaluator. It also assigns every
cell a stable source range so diagnostics can refer back to earlier cells.

Flang's FIR/HLFIR is later in the pipeline. It describes executable lowering
well, but it is not the persistent semantic database required to resolve the
next source cell.

Flang does have an owned parse tree (`parser::Program`) and semantic scopes; it
is not an AST-free frontend. The current action lifecycle is batch-oriented,
however: `runSemanticChecks()` calls `createNewSemanticsContext()` for every
input, and `CodeGenAction::beginSourceFileAction()` replaces both its MLIR
context and module. Merely retaining a `CompilerInstance` therefore does not
produce Clang-like incremental semantics.

Useful LFortran ideas to reproduce at the appropriate Flang layer are:

- stable source ranges per cell for diagnostics;
- a chain of prior semantic scopes;
- additive definitions with an explicit shadowing/redefinition policy;
- a pristine semantic snapshot distinct from IR mutated by passes;
- unique `flang_repl_cell_N` entry functions with explicit `BIND(C)` names;
- suppression of duplicate helper bodies;
- expression-result capture and display;
- one independently loadable code module per cell.

## Incremental REPL delivery plan

### R0a: make buffer-backed Fortran frontend inputs actually work

`FrontendInputFile` already has a `MemoryBuffer` constructor, which is the
right input abstraction for a notebook cell. Current Flang does not complete
that path: `FrontendAction::beginSourceFile()` unconditionally calls
`getFile()`, and prescanning only knows how to reopen a filesystem path or
stdin. `CodeGenAction` also uses `getCurrentFile()` while naming pass-pipeline
and save-temporary outputs.

The first REPL-side experiment therefore adds a parser-prescan path for a
non-owning input buffer and preserves its identifier for diagnostics. A
source-to-LLVM unit test exercises the complete frontend path, rather than
stopping after parsing or initial FIR generation. A separate local smoke
harness has successfully compiled the same buffer first to HLFIR and then all
the way to textual LLVM IR. This is independent of JIT semantics and useful to
any in-process Flang embedding. It should be reviewed as its own patch.

### R0b: embeddable source-to-MLIR action

Add a frontend action that lowers one complete Fortran input to FIR and lets an
embedding client take ownership of the MLIR module and its context, without
printing it or lowering to LLVM. This is a small, generally useful embedding
improvement and a plausible standalone upstream patch.

Most of the machinery already exists: `CodeGenAction::beginSourceFileAction`
creates and owns both `mlirCtx` and `mlirModule`, while `EmitFIRAction` merely
lowers and prints them. The missing API is analogous to Clang
`CodeGenAction::takeModule()`. For MLIR, the module and context should be moved
together in one owner object (with the module destroyed before the context),
plus a no-output FIR action. That lifetime rule is important; returning only
the `OwningOpRef<ModuleOp>` would leave it referring to a context destroyed
with the frontend action.

For the browser executor, the same capture API should either expose a requested
stage or add a second no-output action that stops after the MLIR-to-LLVM
dialect pipeline. Today `CodeGenAction::generateLLVMIR()` runs that pipeline
and immediately translates into an `llvm::Module`; there is no public point to
take the intervening LLVM-dialect module.

### R1: complete program-unit cells

- Accept complete modules/subroutines/functions as cells.
- Keep a session module directory.
- Use ordinary `.mod` files as the first semantic boundary.
- Generate or require one unique `BIND(C)` entry subroutine per executable
  cell.
- Return `{MLIR module, entry symbol, diagnostics, declared module names}`.

This proves separate-cell compilation and cross-module symbol resolution
without changing Flang's parser or semantic scope ownership.

### Concrete two-cell result on the A1 experiment build

The complete-program-unit route has now been exercised with the wasm32 target:

```fortran
! Cell 1
module flang_repl_state_1
  implicit none
  integer, bind(c, name="flang_repl_x") :: x = 40
end module
```

Cell 1 produced both `flang_repl_state_1.mod` and an independent MLIR module
containing a definition of `fir.global @flang_repl_x`. A second source was then
compiled in a fresh Flang invocation:

```fortran
! Cell 2 (the wrapper will eventually be generated)
function flang_repl_cell_2() result(r) bind(c, name="flang_repl_cell_2")
  use flang_repl_state_1, only : x
  integer :: r
  r = x + 2
end function
```

Cell 2 read Cell 1's `.mod` interface and produced a second MLIR module with a
callable `@flang_repl_cell_2` and an external `fir.global @flang_repl_x`. After
the normal Flang pipeline, the two LLVM IR modules respectively contain:

```llvm
@flang_repl_x = global i32 40
```

and:

```llvm
@flang_repl_x = external global i32
define i32 @flang_repl_cell_2() { ... }
```

This proves the frontend half of the design: old source and old FIR do not need
to be concatenated or lowered again. The `.mod` file is only the compile-time
semantic interface; module 1 must remain loaded so the executor can resolve
module 2's live symbol. The next executor test is to load these exact two
dependent modules with the patched Wasm engine and invoke
`flang_repl_cell_2`, expecting `42`.

The session should treat these outputs as one transaction. Compile into a
per-cell staging directory, lower and load the code module, and publish the
new `.mod` files and visible-name index only after all steps succeed. Failed
cells must leave neither semantic interfaces nor loaded symbols behind. Undo
can initially be restricted to the newest cell (or a dependent suffix), which
matches the dependency direction and avoids invalidating already-compiled
callers.

### R2: statement cells through generated wrappers

Wrap executable statements in a generated subroutine. This supports useful
calculator/notebook cells but does not yet make bare declarations persistent.

### R3: generated state modules

Lift cell declarations into generated module storage and automatically `USE`
prior generated modules. A cell contains a module plus a `contains` entry
subroutine. Define how later declarations shadow earlier names.

Every state module should be versioned rather than overwritten. A later `x`
can live in `flang_repl_state_7`, while code compiled earlier keeps referring
to the `x` in its original module and new wrappers import only the newest
visible provider. This gives notebook-like additive rebinding for simple
variables and procedures. Derived types, generics, operator bindings, and
ambiguous `USE` association are the cases most likely to force the later
persistent-semantic design.

This can approximate much of LFortran's behavior using standard Fortran
boundaries, although source mapping and generic/type redefinitions require
care.

### R4: genuine persistent Flang semantic session

If generated modules prove too restrictive, introduce a supported incremental
semantic API:

- preserve and parent scopes across parses;
- retain cooked-source provenance;
- keep pristine symbols separate from lowered/mutated state;
- relink derived types, generics, operators, and procedure bindings;
- support rollback after parse, semantic, lowering, link, or execution errors.

This is the large frontend project.

## Native `flang-repl` prototype status

The experimental implementation is preserved on branch
`flang-repl-experimental` in the durable worktree
`/Users/anutosh491/work/llvm-project-flang-repl`. It currently contains three
checkpoint commits for buffer-backed frontend input, the interpreter/tool
skeleton, and this design summary. No Wasm ABI or target-predefine work is
mixed into its REPL changes; its base is the already committed A1 target-index
change used by the existing build.

The native prototype now has a working vertical slice:

- `flang-repl` preloads the native `flang-rt` and its target intrinsic modules;
- every input compiles from a memory buffer to an independently owned,
  post-Flang-pipeline LLVM-dialect MLIR module;
- `MLIRIncrementalExecutor` creates an execution engine for each successful
  cell and registers earlier definitions for cross-cell resolution;
- executable cells have the explicit entry `flang_repl_cell_N`;
- definition-only cells deliberately have no entry and are loaded without
  being invoked;
- generated state modules and their `.mod` files preserve simple variables
  and procedures across cells;
- `%undo` removes the newest loaded cell and restores the preceding state
  module;
- `%load` preloads an additional native dynamic library.

An explicit `PROGRAM ... END PROGRAM` cell is now normalized into a cell body
and then wrapped in `flang_repl_cell_N`. `_QQmain` is no longer the interpreter
entry contract. This was tested with the unchanged Hello World notebook cell,
which compiled and ran as `flang_repl_cell_1`.

The entry symbol is also returned directly by the cell transformation rather
than guessed by scanning the resulting MLIR. Consequently a user-defined,
zero-argument `BIND(C)` subroutine can be compiled as a definition-only cell
without being executed accidentally.

Simple additive redefinition now works without weakening Flang semantics. If
a new cell redeclares `i`, the generated state module imports the old binding
under a unique internal alias before declaring the new `i`:

```fortran
module flang_repl_state_3
  use flang_repl_state_1, flang_repl_old_3_0 => i
  integer :: i
end module
```

Old compiled code remains bound to the symbol in `flang_repl_state_1`; future
cells import `flang_repl_state_3` and resolve `i` to the new symbol. The same
scheme has been tested for redefining a module procedure. A session where the
first `fn` added its arguments and the second `fn` multiplied them printed `5`
and then `6`.

Current validation covers:

- a complete Hello World program normalized to a numbered cell entry;
- loose expressions (`1+3`, `(2+3)*3`);
- declarations followed by assignment and lookup (`i = 5`);
- declaration plus loop execution;
- procedures defined in one cell and called by later cells;
- a procedure definition and trailing expression in the same cell;
- definition-only `BIND(C)` procedures not being invoked;
- newest-cell undo; and
- simple variable and procedure redefinition.

The first redefinition implementation exposed an important distinction between
"declared by this cell" and "already visible before this cell". It initially
renamed every newly declared entity in the preceding module's `USE` statement;
therefore a first declaration such as `integer :: k` incorrectly tried to
import `k` from a module that did not contain it. `IncrementalCompiler` now
tracks the visible-name set alongside the state-module history and only emits
a rename for a true shadowing declaration. The original multi-cell `k` loop,
new variable declarations after earlier cells, variable redefinition, and undo
were retested. Undo restores compiler/module visibility, but—as with Clang's
code unloading—it cannot reverse side effects that an executed cell already
performed on persistent storage.

Loose Fortran is currently normalized before parsing, not by mutating a Flang
AST. `prepareCellSource()` classifies the cell, emits complete standard
Fortran source containing a state module and/or numbered `BIND(C)` subroutine,
and places that source in an in-memory `MemoryBuffer`. The ordinary Flang
parser, semantic analysis, and lowering pipeline only see the generated valid
program unit. This is a useful prototype boundary, but the textual classifier
does not cover the complete Fortran grammar. An upstream-quality version
should reuse Flang parser productions to build or transform a structured parse
tree before semantics while retaining the same generated-module semantics.

## Native Jupyter kernel proof

An out-of-tree `xeus-flang-repl` prototype now links directly to the exported
`flangInterpreter` CMake target. It does not use CppInterOp. One xeus kernel
process owns one persistent `Fortran::interpreter::Interpreter`, and every
Jupyter `execute_request` passes the complete notebook cell directly to
`compileAndExecute()`.

The initial kernel implements:

- native xeus/ZeroMQ startup and an installable `xflang` kernelspec;
- one persistent Flang session across notebook cells;
- Flang runtime and intrinsic-module discovery from the resource directory;
- runtime loading during xeus `configure_impl()`, before the first request;
- OS-level stdout/stderr capture and Jupyter stream publication; and
- basic success/error execute replies.

CppInterOp and xeus-swift both capture JIT output by redirecting the process
file descriptors to temporary files for the duration of a cell. The Flang
kernel uses the same non-pipe design, but must additionally call the public
Flang runtime `_FortranAFlush(-1)` entry after execution and before restoring
file descriptor 1. Flang's preconnected unit 6 owns buffering above C stdio;
`fflush(nullptr)` alone produced successful execute replies with no notebook
output. Explicitly flushing the Fortran runtime delivers the buffered record
while the per-cell capture is still active.

An executed demonstration notebook validates Hello World, state shared across
cells, a multiline `DO WHILE`, array assignment/output, and variable
redefinition. The observed outputs include `1, 2, 4`, a later value of `8`,
the array `1, 4, 9, 16, 25`, and a redefined value of `100`. The terminal-only
`%end` delimiter is therefore not part of the notebook interface: the Jupyter
protocol already supplies the complete cell as one request.

The current cell classifier is intentionally still a prototype. Declaration
shadowing recognizes simple entity declarations using `::`, and explicit main
program normalization does not yet support an internal `CONTAINS` part. These
rules must move into a separately tested `CellSourceTransformer`, followed by
real Flang fragment-parser entry points, before the loose-cell syntax is an
upstream-quality frontend feature. The successful execution and state model do
not depend on adding a new node to ordinary batch Flang's parse tree.

## MLIR/Wasm executor observations

The MLIR Python binding patches already prove the browser execution pipeline:

```text
MLIR module -> LLVM IR -> Wasm object -> in-process lld shared module
            -> dlopen(RTLD_GLOBAL) -> dlsym/invoke
```

Their repeated-module test loads two independent modules, which is necessary
but not sufficient for a notebook. Add a test where module 2 calls a symbol
defined by module 1. Also specify:

- recoverable diagnostics instead of fatal errors;
- lifetime and unload behavior for `dlopen` handles;
- symbol collision/redefinition behavior;
- rollback after a failed link/load;
- runtime preloading and `RTLD_GLOBAL` behavior under Pyodide.

Native MLIR `ExecutionEngine::create` currently adds one module to a new LLJIT
and does not expose a public incremental `addModule`. Native REPL support will
therefore require either an incremental MLIR executor API or a small adapter
around LLVM ORC. This is executor work; it does not change the proposed Flang
frontend-to-MLIR boundary.

## Proposed atomic REPL patch order

1. Support buffer-backed Fortran input through prescan and code generation,
   with a full source-to-LLVM unit test.
2. Add a no-output frontend action that returns one owned HLFIR/FIR module and
   its context.
3. Expose the post-Flang-pipeline LLVM-dialect module needed by MLIR's Wasm
   executor, without making Flang depend on browser or Python code.
4. Build an out-of-tree session prototype for complete program-unit cells:
   session module directory, dependency tracking, unique entry names, and one
   module loaded per successful cell.
5. Add a cross-module executor test where cell 2 reads storage defined by cell
   1, first natively and then through the Wasm side-module loader.
6. Add generated statement wrappers and automatic `USE` insertion.
7. Only after the generated-module approach is understood, decide whether
   Flang needs a persistent semantic-scope API comparable to LFortran's.

This keeps the compiler patches independently useful. The session layer should
own cell numbering, wrapper generation, `.mod` directories, rollback, and
executor selection; those policies do not belong in ordinary batch Flang.

## Proposed source-tree shape

Once the frontend capture APIs and out-of-tree spike stabilize, a
`flangInterpreter` library can mirror the successful library/tool separation of
`clangInterpreter` without copying its C/C++ data model:

```text
flang/include/flang/Interpreter/
  Interpreter.h
  CellArtifact.h
  IncrementalExecutor.h
  Value.h

flang/lib/Interpreter/
  Interpreter.cpp
  IncrementalCompiler.h
  IncrementalCompiler.cpp
  OrcIncrementalExecutor.h
  OrcIncrementalExecutor.cpp
  WasmIncrementalExecutor.h
  WasmIncrementalExecutor.cpp
  StateModule.cpp
  Value.cpp
  CMakeLists.txt

flang/tools/flang-repl/
  FlangRepl.cpp
  CMakeLists.txt

flang/unittests/Interpreter/
flang/test/Interpreter/
```

`Interpreter` can expose familiar compile/execute, lookup, undo, and runtime
loading operations. `CellArtifact` is a better Flang term than
`PartialTranslationUnit`: it owns or identifies the MLIR module, entry symbol,
generated `.mod` effects, exported-name bindings, dependencies, and executor
resource handle. `IncrementalCompiler` and `StateModule` implement the
Fortran-only wrapper/`USE`/rebinding policy. `IncrementalExecutor` is the
execution contract, with native ORC and Emscripten implementations.

The first implementation should deliberately be named `IncrementalCompiler`,
not `IncrementalParser`. It compiles one complete or generated Fortran program
unit at a time while accumulating session-visible `.mod` interfaces and loaded
code. Flang does not yet keep its parser and semantic context alive across
cells, so naming that component `IncrementalParser` would claim semantics it
does not provide. A real `IncrementalParser` becomes appropriate only in the
later persistent-semantics phase.

## Embedding and process-lifetime boundary

`flang-repl` is a thin standalone client of `flangInterpreter`, not the API
that Jupyter or WasmBolt should embed. The same library must support three
clients without knowing about any of their UI or transport details:

- the `flang-repl` terminal executable;
- a native Jupyter kernel; and
- a long-lived Emscripten host such as WasmBolt or a JupyterLite kernel.

The executable owns `InitLLVM`, command-line parsing, line editing, and process
shutdown. An embedding host owns those facilities for its entire lifetime and
calls `flangInterpreter` directly. The library must not construct `InitLLVM`,
call `llvm_shutdown()`, reset process-global command-line options per cell, or
route results through temporary command-line-driver output.

`IncrementalCompilerBuilder` should parse/configure compiler options once and
produce session configuration. Each cell then borrows or clones that
configuration through ordinary APIs. This avoids the long-lived-driver
command-line-state problem described by the embeddable LLVM tool-driver RFC.

The frontend/library boundary is an owned compilation result: an in-memory
Fortran buffer enters Flang and an MLIR module leaves it without printing or a
temporary source file. The module and its `MLIRContext` must be returned as one
owner object whose destruction order is encoded in the type. A local capture
experiment reached the post-Flang LLVM-dialect module successfully; taking the
module and context as unrelated values also reproduced a destruction-order
crash, confirming that this ownership rule belongs in the public API.

The executor boundary is deliberately replaceable:

- native hosts translate/add the module to a persistent ORC JIT and retain a
  resource tracker for undo;
- Emscripten hosts emit an object, invoke re-entrant Wasm LLD, `dlopen` the
  side module, and resolve its entry symbol; and
- the interpreter sees the same add/remove/lookup/load-runtime contract.

This keeps Jupyter and WasmBolt integration out of Flang while still making
them first-class consumers of the upstream library.

The executor should not be permanently duplicated in `flangInterpreter`.
After both adapters exist, native ORC and Wasm loading can be factored into the
language-neutral transaction interface described above and shared with
`clangInterpreter`.

## Scope and rough effort

These are engineering ranges, not schedules, and assume an experienced LLVM
contributor with responsive review.

| Increment | Rough effort | Main risk |
|---|---:|---|
| wasm32 object for runtime-free Fortran | days to 2 weeks | FIR target ABI coverage and build validation |
| Target-aware runtime ABI lowering suitable for upstream | 1-3 months | removing host-type reflection without regressing supported targets |
| wasm32 `libflang-rt` MVP + `emcc` link tests | 1-2 months | libc/platform assumptions and runtime coverage |
| `emflang` wrapper/integration | 1-3 weeks after contracts stabilize | ownership split between LLVM and Emscripten |
| Embeddable one-cell FIR action | days | API/lifetime design |
| Complete-unit cells using `.mod` state | 2-4 weeks | module/session hygiene and executor linkage |
| Useful statement/declaration cells via generated modules | 1-3 months | source transformation and shadowing semantics |
| Full loose-cell semantic REPL | 4-9+ months | persistent Flang semantics, redefinition, rollback, result display |
| Jupyter kernel packaging after frontend API | 3-6 weeks for an MVP | protocol/output polish rather than compiler code |

OpenMP, coarrays, CUDA/offload, debugger integration, arbitrary redefinition,
and full runtime coverage are explicitly outside the first MVPs.

## RFC detail to correct

The sample described as an “incomplete module in cell 2 that depends on cell
1” is syntactically a complete module. Its `INTEGER :: a` declares new storage
in that module; it does not reference the variable declared in cell 1. A real
cross-cell example should use a module from cell 1 in cell 2, or show the
generated wrapper/state-module transformation explicitly.

## Definition of done for the immediate Wasm milestone

The first milestone is complete when a clean latest-main-derived branch can:

1. build one native Flang binary without statically specializing that binary
   to wasm32;
2. compile the runtime-free `BIND(C)` test to a valid wasm32 object;
3. compile representative runtime calls with ABI-correct declarations;
4. link those objects and a wasm32 Flang runtime using `emcc`;
5. run and validate output under Node;
6. carry focused tests for every LLVM/Flang patch proposed upstream;
7. document unsupported runtime features rather than silently miscompile them.

## References

- [Interactive Flang/Jupyter RFC](https://discourse.llvm.org/t/rfc-proposing-an-interactive-fortran-workflow-with-flang-using-jupyter-notebooks/89116)
- [Interactive MLIR/Wasm RFC](https://discourse.llvm.org/t/rfc-bringing-interactive-mlir-compilation-and-execution-to-webassembly/91566)
- [Embeddable LLVM tool drivers for long-lived hosts RFC](https://discourse.llvm.org/t/rfc-embeddable-llvm-tool-drivers-for-long-lived-hosts/91754)
- [WasmBolt](https://github.com/anutosh491/WasmBolt) and its [live deployment](https://anutosh21.github.io/WasmBolt/)
- [Emscripten Fortran integration discussion and Isabel's toolchain proposal](https://github.com/emscripten-core/emscripten/issues/5553#issuecomment-5730186892)
- [Serge's unpublished Flang/Wasm branch](https://github.com/serge-sans-paille/llvm-project/tree/feature/flang-wasm)
- [Rejected weak-symbol COMMON prototype (PR 151478)](https://github.com/llvm/llvm-project/pull/151478)
- [LLVM Wasm common-linkage work](https://github.com/llvm/llvm-project/pull/199303)
- [r-wasm/flang-wasm](https://github.com/r-wasm/flang-wasm)
- [George Stagg's Flang/Wasm write-up](https://gws.phd/posts/fortran_wasm/)
- [Emscripten's WebAssembly compilation documentation](https://github.com/emscripten-core/emscripten/blob/main/site/source/docs/compiling/WebAssembly.rst)
- [Emscripten-forge recipes](https://github.com/emscripten-forge/recipes)
