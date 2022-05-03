import { init, stop, count, run, start } from "./index";

init();

const { cycles, instructions, missedBranches, branches } = run(() => {
  var j = 0;
  for (let i = 0; i < 999999; i++) {
    j = i;
  }
  return j;
});

console.log({
  cycles: Number(cycles),
  instructions: Number(instructions),
  missedBranches: Number(missedBranches),
  branches: Number(branches),
});
