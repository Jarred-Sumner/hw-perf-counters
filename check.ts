import { init, stop, count, run, start } from "./index";

init();

function func() {
  var j = 0;
  for (let i = 0; i < 99999; i++) {
    j = i + 1;
  }
  return j;
}

for (let i = 0; i < 9999; i++) func();

const { cycles, instructions, missedBranches, branches } = run(func);

console.log({
  cycles: Number(cycles),
  instructions: Number(instructions),
  missedBranches: Number(missedBranches),
  branches: Number(branches),
});
