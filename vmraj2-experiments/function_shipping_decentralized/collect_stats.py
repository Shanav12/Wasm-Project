import requests
import random
import statistics

hostnames = ["sp26-cs525-1801.cs.illinois.edu", "sp26-cs525-1802.cs.illinois.edu"]
functions = ["factorial", "fib", "prime"]
function_ranges = {
    "factorial": (1, 10),
    "fib": (1, 10),
    "prime": (1, 1000000)
}

NUM_TRIALS = 100

def make_request(hostname, function, arg):
    r = requests.post(f"http://{hostname}:8080/invoke", json={
            "function": function,
            "arg": arg
        })

    return r.json()

def agg_stats(measurements):
    return statistics.mean(measurements), statistics.stdev(measurements)

stats = {}

for hostname in hostnames:
    for function in functions:
        function_range = function_ranges[function]

        fetch_times = []
        compile_times = []
        inst_times = []
        exec_times = []
        total_times = []
        for i in range(NUM_TRIALS):
            arg = random.randrange(function_range[0], function_range[1], 1)
            res = make_request(hostname, function, arg)
            timings = res["timing_ms"]
            fetch_times.append(timings["fetch"])
            compile_times.append(timings["compile"])
            inst_times.append(timings["instantiate"])
            exec_times.append(timings["exec"])
            total_times.append(timings["total"])

        stats[(hostname, function)] = {
            "fetch": agg_stats(fetch_times),
            "compile": agg_stats(compile_times),
            "inst": agg_stats(inst_times),
            "exec": agg_stats(exec_times),
            "total": agg_stats(total_times)
        }

for (host, func), aggs in stats.items():
    print("-" * 20)
    print(f"{host}\t{func}")
    
    for stat_key, (avg, stdev) in aggs.items():
        print(f"\t{stat_key}: {avg}, {stdev}")
