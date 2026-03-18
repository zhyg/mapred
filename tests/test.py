import sys

for line in sys.stdin:
    line = line.rstrip("\n")
    print(f"{line}\t{len(line)}")
