import time

N = 5_000_000
t0 = time.perf_counter()
s = 0
for i in range(N):
    s = s + i
t1 = time.perf_counter()
print("INNER_MS=" + str(int(round((t1 - t0) * 1000))))
print("SUM=" + str(s))
