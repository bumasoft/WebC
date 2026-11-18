#!/usr/bin/env python3
"""Turn a text asset into a C string constant so the binary stays self-contained."""
import sys

def main() -> int:
    src, name = sys.argv[1], sys.argv[2]
    data = open(src, "r", encoding="utf-8").read()
    out = [f"// generated from {src} — do not edit\n",
           f"static const char {name}[] ="]
    chunk = []
    for ch in data:
        if ch == "\\":
            chunk.append("\\\\")
        elif ch == '"':
            chunk.append('\\"')
        elif ch == "\n":
            chunk.append("\\n\"\n\"")
        elif ch == "\t":
            chunk.append("\\t")
        else:
            chunk.append(ch)
    out.append('"' + "".join(chunk) + '";\n')
    out.append(f"static const unsigned {name}_len = sizeof {name} - 1;\n")
    sys.stdout.write("".join(out))
    return 0

if __name__ == "__main__":
    sys.exit(main())
