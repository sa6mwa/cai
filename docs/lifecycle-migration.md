# Lifecycle cutover

## Ownership

Make exposes public commands and serializes operations. Bash sequences readable
commands. CMake owns configuration, dependencies, compilation and install. CTest
owns registered checks, their selections and native results. Compiler/tool
collections and source inputs stay stable while one operation owns a workspace.
Changing a compiler collection requires explicit invalidation of its build root.
Opt-in live and inspector targets reject absent authorization before producers.
The focused Lua Smith live target installs the local LuaRock before execution.
Hardening preparation runs live checks and fuzz smoke once, then extends fuzzing.

Library and CLI test registration live in `cmake/CaiTests.cmake` and
`cmake/CaiCliTests.cmake`. Small shared host fixture and environment declarations
live beside them. C89, public API, ABI, presets' product separation and published
targets remain product contracts.
Presets use schema 3, matching the declared CMake 3.21 minimum; they require no
newer preset-only feature.

## Phase contract

| Command | Work | Cleaning / release identity |
| --- | --- | --- |
| `make test`, `make test-cli` | Selected native feedback | Incremental; no cross matrix |
| `make test-all` | Native core once, CLI, memory, fuzz, Lua, offline and local e2e | Incremental; no cross matrix |
| `make format`, `make format-check` | Source formatting | No configure/build |
| `make prerelease` | Native checks, selected matrix, artifacts and consumers | Incremental; candidate override allowed |
| `make package` | SDK staging and binary-scope checksum inventory | Warm; preserves other identified artifacts |
| `make package-verify` | Existing complete shipped bytes and extracted consumers | Does not rebuild product producers |
| `make package-source-smoke` | Explicit clean source reconstruction | Owns only source-smoke scratch |
| `make release`, `make release-matrix` | One final local artifact set | Clean committed checkout and exact lightweight tag required first |

Final release cleans local generated state once. Its source reconstruction is
the native Release producer and full non-live test lane. Its GNU SDK is packaged
from that output. Selected cross SDKs retain their target scope and the shipped
musl CLI retains actual runtime proof. No other cross runtime runner is claimed.
Equivalent prerequisites share `.cache/deps` and verified archives; distinct
instrumentation remains separate. Host-only fixtures are labeled `host` and are
not repeated under hardening or Lua selections.

The final explicit checksum inventory requires every selected SDK plus source,
CLI and Lua delivery. Binary-only rehearsals have their own manifest under
`build/`. No release refs are mutated by local lifecycle commands. Failed final
release stops; it does not fix, retag, rebuild or publish in the same operation.
Version and target discovery failures stop packaging before producers or checksum
publication. Empty targets and unknown checksum scopes are rejected. Source archive
staging belongs under `build/`; `dist/` contains only delivery artifacts. Released
Lua rockspecs reference their public source archive, also embedded in the source rock.

## Removed controllers

Python test drivers and the Python service controller have been removed. Python
is limited to small provider, clipboard, archive and Chrome protocol data peers.
These peers do not launch product commands, make assertions or decide lifecycle
outcomes. Bash/compiled tests own commands, expected failures and assertions.
Existing corpus, protocol fixtures, installed-consumer checks and suppression
policies are preserved.

Services use one rootless `podman kube play` helper and the tracked manifest.
Checkout-specific pod names, loopback ports, user-owned state, private credentials,
real readiness probes and teardown from retained manifests remain required.
No Compose/containerd path, secondary dispatcher or process supervisor remains.
Reset covers all services and requires retained manifests for every live pod;
stopping a single service uses `down SERVICE` and retains the other services.
Both full cleanup and dist-only cleanup reject shared caches under their deleted
paths, including caches reached through external symlink aliases.

The legacy dependency-cache alias and its competing cache interpretation are
removed. `CPKT_DEPENDENCY_CACHE` resolves from explicit CMake input, environment,
XDG cache or HOME. Hash-verified archive hits make no acquisition requests. A
small CMake helper verifies bytes before publication and extraction.

## Verification

The cutover and review precede any build/test execution, as instructed. Relevant
regressions cover phase ordering/failures, acquisition miss/hit/corruption/request
counts, source identity, artifact completeness, producer reuse, service ownership,
CLI behavior and terminal framing. Actual passed results are reported separately;
this ledger does not treat implementation or syntax checks as executable proof.
