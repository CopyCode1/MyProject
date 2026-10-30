---
name: gt02-zephyr-product
description: Design, implement, or review this GT02 Zephyr firmware as a maintainable product. Use for requirements, architecture, boards and Devicetree, drivers, services, BLE, GNSS, 4G, IMU, UWB, power, storage, MCUboot, OTA, testing, or reimplementing legacy behavior; do not use for unrelated generic C work.
---

# GT02 Zephyr Product Engineering

Treat this repository as a from-zero product implementation. Use the legacy nRF52840 + FreeRTOS firmware as a behavioral reference, not as the target architecture.

## Source of truth

Read the relevant sections of [GT02 Zephyr 产品工程设计指南](../../../docs/GT02产品工程设计指南.md) before making architectural or product-level decisions. Do not load the whole guide for a small, isolated edit; route by topic:

- Requirements, milestones, or acceptance: sections 4, 15, 20, and 21.
- Layering, APIs, directories, or dependency review: sections 2, 3, 5, 6, and 7.
- For architectural reasoning, lifecycle/ownership discussions, or evaluating a proposed design, use [GT02工程思路与设计讨论](../gt02-engineering-dialogue/SKILL.md) when useful; keep simple questions concise and do not expand the authorized task.
- When adding or modifying project-owned C functions or headers, also read and follow [GT02中文函数与接口注释规范](../gt02-code-comments/SKILL.md). Use concise Chinese function comments and detailed header contracts; do not bulk-edit unrelated code.
- Threads, events, state machines, or shared state: sections 8 and 10.
- Board, Devicetree, Kconfig, or drivers: sections 5.4, 5.5, 6, and 9.
- GT02 4G connection ownership, buffering, scheduling, or modem integration: read the relevant sections of [4G多连接分层架构设计](../../../docs/设计文档/4G多连接分层架构设计.md); distinguish its planned implementation from verified hardware capabilities.
- Reliability, storage, power, or observability: sections 11, 12, and 13.
- Bootloader or OTA: section 14.
- Legacy reimplementation: section 16.
- Review or completion checks: sections 15, 18, and 19.

Legacy reference project:

`C:\Users\Administrator\Desktop\Git\GT02_GT03\applications\gt02_new`

Use `gt02_4g` as the provisional behavior baseline unless the user selects another target. Never modify the legacy project unless explicitly requested.

## Required decision model

For a non-trivial feature, establish these facts before implementation:

1. User-visible goal and explicit non-goals.
2. Inputs, outputs, states, events, timing, and acceptance criteria.
3. Owning layer and module dependencies.
4. Failure, timeout, retry, recovery, and power behavior.
5. Verification method and relevant resource impact.

If a missing hardware fact or product choice would materially change the implementation, identify it clearly instead of hiding it behind an arbitrary assumption. Continue with reversible scaffolding when possible.

## Architecture invariants

- Domain and product policy must not depend on physical pins, bus instances, registers, concrete peripheral chips, or Zephyr driver headers.
- Services orchestrate use cases and expose product-level behavior; drivers do not decide product policy.
- Hardware topology belongs in Devicetree and pinctrl. Compile-time capabilities belong in Kconfig. Source selection belongs in CMake.
- Do not edit generated files under `build/`.
- Prefer Zephyr subsystems and device APIs. Confine nrfx, vendor libraries, and chip-specific APIs to adapters or drivers.
- Define interfaces around the capability the caller needs, not around the full API of a particular chip.
- Give each mutable state one clear owner. Communicate across execution contexts with explicit events, queues, callbacks, or commands.
- Do not recreate the legacy global polling loop by default. Choose work queues, delayable work, timers, or dedicated threads according to blocking and timing requirements.
- Keep OTA transports separate from image storage, validation, activation, confirmation, and rollback.
- Avoid abstraction that has no credible variation, test seam, or dependency-boundary benefit.

## Legacy behavior workflow

When reimplementing an old feature:

1. Inspect only the relevant old source and build target.
2. Record observable behavior, data formats, timing, state, and edge cases.
3. Separate portable algorithms/protocol code from RTOS, Nordic SDK, SoftDevice, board macros, and direct hardware access.
4. Define the new boundary and tests before adapting hardware.
5. Compare new and old outputs with common fixtures or captured traces where practical.

Do not bulk-copy the legacy tree into this repository. Reuse pure, validated logic only after checking its dependencies, license, assumptions, integer widths, byte order, and error handling.

## Implementation workflow

- Prefer one small vertical product slice that can be built and observed end to end.
- Keep `main.c` as composition and startup code, not a growing business module.
- Add only directories and abstractions needed by the current slice.
- Define error propagation and execution context at every new asynchronous boundary.
- Add diagnostics that explain state transitions and failures without exposing secrets.
- Verify in proportion to risk: build checks for configuration changes, unit tests for pure logic, board tests for hardware, and failure injection for recovery paths.
- Report assumptions, files changed, tests run, unresolved hardware facts, and the next safe milestone.

## Review gate

Before declaring work complete, check:

- A board or chip replacement would affect only its intended boundary.
- Interrupt and timer callbacks do not perform blocking work.
- Retries are bounded and lead to a defined degraded or failure state.
- Persistent data has defaults, versioning, and corruption behavior.
- Normal behavior and principal failures have reproducible verification steps.
- Documentation is updated when behavior, interfaces, partitions, or architectural decisions change.

