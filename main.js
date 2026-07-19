const fs = require("fs");

const args = process.argv.slice(2);

if (args.length < 1 || args[0] === "--help" || args[0] === "help") {
  console.error("Usage: node main.js <file.bf>");
  process.exit(1);
}

const filePath = args[0];
let code;
try {
  code = fs.readFileSync(filePath, "utf8");
} catch (e) {
  console.error("error: could not open " + filePath);
  process.exit(1);
}

function brainfuck(code) {
  const tape = [];
  let ptr = 0;
  let ip = 0;
  let out = "";

  const loops = {};
  const stack = [];

  for (let i = 0; i < code.length; i++) {
    if (code[i] === "[") stack.push(i);
    else if (code[i] === "]") {
      const start = stack.pop();
      loops[start] = i;
      loops[i] = start;
    }
  }

  const get = () => tape[ptr] ?? 0;
  const set = v => tape[ptr] = v & 255;

  while (ip < code.length) {
    switch (code[ip]) {
      case ">": ptr++; break;
      case "<":
        if (--ptr < 0) throw new Error("Negative pointer");
        break;
      case "+": set(get() + 1); break;
      case "-": set(get() - 1); break;
      case ".": process.stdout.write(String.fromCharCode(get())); break;
      case "[":
        if (get() === 0) ip = loops[ip];
        break;
      case "]":
        if (get() !== 0) ip = loops[ip];
        break;
    }
    ip++;
  }
}

brainfuck(code);
