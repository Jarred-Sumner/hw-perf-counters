import { dlopen, ptr, suffix } from "bun:ffi";

var countersBuffer: BigUint64Array;
var countersBufferPtr;
var lib;

var performance_counters_init;
var performance_counters_start;
var performance_counters_stop;

export function init() {
  if (lib) return;
  if (process.platform === "linux") {
    throw new Error("This package is not supported on Linux yet");
  }

  lib = dlopen(import.meta.dir + `/counters.${process.arch}.${suffix}`, {
    performance_counters_init: {
      returns: "cstring",
      args: [],
    },
    performance_counters_start: {
      returns: "cstring",
      args: [],
    },
    performance_counters_stop: {
      args: ["ptr"],
      returns: "cstring",
    },
  });

  performance_counters_init = lib.symbols.performance_counters_init;
  performance_counters_start = lib.symbols.performance_counters_start;
  performance_counters_stop = lib.symbols.performance_counters_stop;

  const out = performance_counters_init();
  if (out && out.length) {
    throw new Error(out);
  }

  count.countersBuffer = countersBuffer = new BigUint64Array(4);
  countersBufferPtr = ptr(countersBuffer);
}

export function start() {
  const str = performance_counters_start();
  if (str?.length) {
    throw new Error(str);
  }
}

export function run(func: CallableFunction) {
  start();
  func();
  stop();

  return count;
}

export function stop() {
  const str = performance_counters_stop(countersBufferPtr);
  if (str?.length) {
    throw new Error(str);
  }
}

export const count = {
  get cycles(): number | BigInt {
    // @ts-ignore
    return BigInt(Number(countersBuffer[0])) === Number(countersBuffer[0])
      ? Number(countersBuffer[0])
      : countersBuffer[0];
  },
  get branches(): number | BigInt {
    // @ts-ignore
    return BigInt(Number(countersBuffer[3])) === Number(countersBuffer[3])
      ? Number(countersBuffer[3])
      : countersBuffer[3];
  },
  get instructions(): number | BigInt {
    // @ts-ignore
    return BigInt(Number(countersBuffer[2])) === Number(countersBuffer[2])
      ? Number(countersBuffer[2])
      : countersBuffer[2];
  },
  get missedBranches(): number | BigInt {
    // @ts-ignore
    return BigInt(Number(countersBuffer[3])) === Number(countersBuffer[3])
      ? Number(countersBuffer[3])
      : countersBuffer[3];
  },

  countersBuffer: null,
  cyclesOffset: 1,
  branchesOffset: 2,
  instructionsOffset: 3,
  missedBranchesOffset: 4,
};

export function close() {
  if (!lib) {
    return;
  }
  lib.close();
  count.countersBuffer = countersBuffer = null;
  lib = null;
  performance_counters_init = null;
  performance_counters_start = null;
  performance_counters_stop = null;
  countersBufferPtr = 0;
}
