# Hardware performance counters for macOS

This package exposes macOS hardware performance counters to JavaScript.

The code is based on [this gist](https://gist.github.com/ibireme/173517c208c7dc333ba962c1f0d67d12).

This package will only run in [Bun](https://bun.sh) v0.0.79 or later due to depending on `bun:ffi`.

## Usage

This package requires root access to run.

```js
import { init, run } from "hw-perf-count";

// Set up the hardware performance counters.
// This loads private macOS APIs
// If sudo is not enabled, this likely will throw an error
init();

const { instructions, cycles, missedBranches, branches } = run(() => {
  for (let i = 0; i < 100000; i++) {
    // Do something
  }
});

console.log({ instructions, cycles, missedBranches, branches });
```

```js
import { init, start, stop, count } from "hw-perf-count";

// Start counting
start();

// Stop counting
stop();

// How many instructions ran between start() and stop()?
console.log(count.instructions);

// How many cycles ran between start() and stop()?
console.log(count.cycles);

// How many branches missed between start() and stop()?
console.log(count.missedBranches);

// How many branches overall between start() and stop()?
console.log(count.branches);
```

`count` updates when you call `stop()`.

```ts
export const count: {
  get cycles(): number | BigInt;
  get branches(): number | BigInt;
  get instructions(): number | BigInt;
  get missedBranches(): number | BigInt;
};
```
