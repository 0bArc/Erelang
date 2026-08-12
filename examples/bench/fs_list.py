import os
import sys
import time

root = sys.argv[1] if len(sys.argv) > 1 else "bench_tree"
t0 = time.perf_counter()
count = 0
for name in os.listdir(root):
    full = os.path.join(root, name)
    if os.path.isdir(full):
        count += 1
        for child in os.listdir(full):
            cfull = os.path.join(full, child)
            if os.path.isfile(cfull):
                count += 1
    elif os.path.isfile(full):
        count += 1
t1 = time.perf_counter()
print("INNER_MS=" + str(int(round((t1 - t0) * 1000))))
print("COUNT=" + str(count))
