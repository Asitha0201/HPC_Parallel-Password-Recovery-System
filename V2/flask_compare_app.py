import math
import json
import re
import subprocess
import time
from pathlib import Path

from flask import Flask, request, render_template_string


BASE_DIR = Path(__file__).resolve().parent
DEFAULT_HASH = "7a265bfa1eed87f48aaa30e2c37f6ade"
DEFAULT_DICT = "dictionary.txt"
RESULTS_FILE = BASE_DIR / ".flask_compare_results.json"

METHODS = [
    {
        "key": "serial",
        "name": "Serial",
        "workers": 1,
        "command": ["./serial_dictionary_txt"],
        "time_patterns": [r"Wall time\s*:\s*([0-9.]+)", r"Time\s*:\s*([0-9.]+)"],
    },
    {
        "key": "openmp",
        "name": "OpenMP",
        "workers": None,
        "command": ["./omp_dictionary_txt_full"],
        "time_patterns": [r"Time\s*:\s*([0-9.]+)"],
    },
    {
        "key": "mpi",
        "name": "Open MPI 4 Ranks",
        "workers": 4,
        "command": ["mpirun", "-np", "4", "./openmpi_dictionary_txt_4rank"],
        "time_patterns": [r"Total time\s*:\s*([0-9.]+)", r"Search time\s*:\s*([0-9.]+)"],
    },
    {
        "key": "hybrid",
        "name": "Hybrid CUDA",
        "workers": 1,
        "command": ["./hybrid_dictionary_txt_full"],
        "time_patterns": [r"Time\s*:\s*([0-9.]+)"],
    },
]

COMPILE_COMMANDS = [
    ["gcc", "-O2", "-o", "serial_dictionary_txt", "serial_dictionary_txt.c", "-lssl", "-lcrypto"],
    ["gcc", "-O2", "-fopenmp", "-o", "omp_dictionary_txt_full", "omp_dictionary_txt_full.c", "-lssl", "-lcrypto"],
    ["mpicc", "-O2", "-o", "openmpi_dictionary_txt_4rank", "openmpi_dictionary_txt_4rank.c", "-lssl", "-lcrypto"],
    ["nvcc", "-O2", "-Xcompiler", "-fopenmp", "-o", "hybrid_dictionary_txt_full", "hybrid_dictionary_txt_full.cu"],
]

app = Flask(__name__)


def is_md5(value):
    return bool(re.fullmatch(r"[0-9a-fA-F]{32}", value or ""))


def parse_first(patterns, text):
    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            return float(match.group(1))
    return None


def parse_found(text):
    if re.search(r"Password NOT found|RESULT\s*:\s*Not found", text, re.I):
        return "Not found"
    match = re.search(r'Password found[^"]*"([^"]+)"', text)
    if not match:
        match = re.search(r'FOUND\s*:\s*"([^"]+)"', text)
    return match.group(1) if match else "Unknown"


def parse_index(text):
    match = re.search(r"(?:Index|Global index)\s*:\s*([0-9]+)", text)
    return int(match.group(1)) if match else None


def rmse(values):
    if not values:
        return None
    avg = sum(values) / len(values)
    return math.sqrt(sum((value - avg) ** 2 for value in values) / len(values))


def load_saved_results():
    if not RESULTS_FILE.exists():
        return []
    try:
        return json.loads(RESULTS_FILE.read_text())
    except (json.JSONDecodeError, OSError):
        return []


def save_results(results):
    RESULTS_FILE.write_text(json.dumps(results, indent=2))


def clear_results():
    if RESULTS_FILE.exists():
        RESULTS_FILE.unlink()


def method_by_key(key):
    for method in METHODS:
        if method["key"] == key:
            return method
    return None


def run_command(command, dictionary, target_hash, timeout_seconds, env=None):
    full_command = list(command)
    if dictionary:
        full_command.append(dictionary)
    if target_hash:
        full_command.append(target_hash)
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            full_command,
            cwd=BASE_DIR,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_seconds,
            env=env,
        )
        wall_time = time.perf_counter() - started
        return completed.returncode, completed.stdout, wall_time
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        return 124, output + "\nTimed out.", timeout_seconds
    except FileNotFoundError as exc:
        return 127, str(exc), 0.0


def run_method(method, dictionary, target_hash, repeats, timeout_seconds, openmp_threads):
    run_times = []
    last_output = ""
    last_code = 0
    fallback_wall_time = 0.0
    env = None
    workers = method["workers"]
    name = method["name"]

    if method["key"] == "openmp":
        workers = openmp_threads
        name = f"OpenMP ({openmp_threads} threads)"
        env = os_environ_with_threads(openmp_threads)

    for _ in range(repeats):
        code, output, wall_time = run_command(method["command"], dictionary, target_hash, timeout_seconds, env)
        last_code = code
        last_output = output
        fallback_wall_time = wall_time
        parsed_time = parse_first(method["time_patterns"], output)
        run_times.append(parsed_time if parsed_time is not None else wall_time)
        if code != 0:
            break

    best_time = min(run_times) if run_times else None
    return {
        "key": method["key"],
        "name": name,
        "workers": workers,
        "return_code": last_code,
        "time": best_time,
        "rmse": rmse(run_times),
        "runs": run_times,
        "found": parse_found(last_output),
        "index": parse_index(last_output),
        "output": last_output[-2500:],
        "wall_fallback": fallback_wall_time,
    }


def os_environ_with_threads(openmp_threads):
    import os
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = str(openmp_threads)
    return env


def compile_programs():
    results = []
    for command in COMPILE_COMMANDS:
        code, output, _ = run_command(command, "", "", 120)
        results.append({"command": " ".join(command), "code": code, "output": output[-1200:]})
    return results


HTML = """
<!doctype html>
<html>
<head>
  <title>HPC Password Recovery Compare</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 24px; background: #f7f7f7; color: #222; }
    h1 { font-size: 24px; margin-bottom: 6px; }
    h2 { font-size: 18px; margin-top: 24px; }
    .box { background: white; border: 1px solid #ccc; padding: 16px; margin-bottom: 16px; }
    label { display: block; margin-top: 10px; font-weight: bold; }
    input { padding: 8px; width: 420px; max-width: 95%; border: 1px solid #aaa; }
    select { padding: 8px; border: 1px solid #aaa; }
    button { margin-top: 14px; padding: 8px 14px; border: 1px solid #555; background: #e9e9e9; cursor: pointer; }
    .buttons button { margin-right: 8px; }
    table { border-collapse: collapse; width: 100%; background: white; }
    th, td { border: 1px solid #bbb; padding: 8px; text-align: left; }
    th { background: #eeeeee; }
    .small { color: #555; font-size: 13px; }
    .error { color: #9b0000; font-weight: bold; }
    pre { background: #222; color: #eee; padding: 10px; overflow: auto; max-height: 220px; }
  </style>
</head>
<body>
  <h1>HPC Password Recovery Compare</h1>
  <div class="small">Simple Flask GUI for Serial, OpenMP, Open MPI, and Hybrid CUDA.</div>

  <div class="box">
    <form method="post">
      <label>MD5 Hash</label>
      <input name="hash" value="{{ target_hash }}" maxlength="32">

      <label>Dictionary file</label>
      <input name="dictionary" value="{{ dictionary }}">

      <label>Repeats for RMSE</label>
      <input name="repeats" type="number" min="1" max="10" value="{{ repeats }}">

      <label>Timeout per run, seconds</label>
      <input name="timeout" type="number" min="10" max="20000" value="{{ timeout }}">

      <label>OpenMP threads</label>
      <select name="openmp_threads">
        {% for n in [2, 4, 8, 16] %}
          <option value="{{ n }}" {% if n == openmp_threads %}selected{% endif %}>{{ n }} threads</option>
        {% endfor %}
      </select>

      <br>
      <div class="buttons">
        <button name="action" value="run_serial">Run Serial</button>
        <button name="action" value="run_openmp">Run OpenMP</button>
        <button name="action" value="run_mpi">Run Open MPI</button>
        <button name="action" value="run_hybrid">Run Hybrid CUDA</button>
      </div>
      <button name="action" value="compile">Compile programs</button>
      <button name="action" value="clear">Clear results</button>
    </form>
    <p class="small">Run methods one by one. The table keeps saved results and updates speedup, efficiency, and RMSE after each run.</p>
  </div>

  {% if error %}
    <div class="box error">{{ error }}</div>
  {% endif %}

  {% if compile_results %}
    <h2>Compile Output</h2>
    {% for item in compile_results %}
      <div class="box">
        <b>{{ item.command }}</b> - return code {{ item.code }}
        <pre>{{ item.output }}</pre>
      </div>
    {% endfor %}
  {% endif %}

  {% if results %}
    <h2>Evaluation Metrics</h2>
    {% if not has_serial %}
      <p class="small">Run Serial first to calculate Speedup and Efficiency.</p>
    {% endif %}
    <table>
      <tr>
        <th>Method</th>
        <th>Found</th>
        <th>Index</th>
        <th>Best Time (s)</th>
        <th>Speedup S</th>
        <th>Efficiency E</th>
        <th>RMSE</th>
        <th>Workers</th>
      </tr>
      {% for row in results %}
      <tr>
        <td>{{ row.name }}</td>
        <td>{{ row.found }}</td>
        <td>{{ row.index if row.index is not none else "-" }}</td>
        <td>{{ "%.4f"|format(row.time) if row.time is not none else "-" }}</td>
        <td>{{ "%.3f"|format(row.speedup) if row.speedup is not none else "-" }}</td>
        <td>{{ "%.3f"|format(row.efficiency) if row.efficiency is not none else "-" }}</td>
        <td>{{ "%.4f"|format(row.rmse) if row.rmse is not none else "-" }}</td>
        <td>{{ row.workers }}</td>
      </tr>
      {% endfor %}
    </table>

    <h2>Raw Output</h2>
    {% for row in results %}
      <div class="box">
        <b>{{ row.name }}</b> return code {{ row.return_code }}
        <pre>{{ row.output }}</pre>
      </div>
    {% endfor %}
  {% endif %}
</body>
</html>
"""


@app.route("/", methods=["GET", "POST"])
def index():
    target_hash = DEFAULT_HASH
    dictionary = DEFAULT_DICT
    repeats = 1
    timeout_seconds = 7200
    openmp_threads = 4
    error = None
    results = load_saved_results()
    compile_results = None

    if request.method == "POST":
        target_hash = request.form.get("hash", "").strip()
        dictionary = request.form.get("dictionary", DEFAULT_DICT).strip() or DEFAULT_DICT
        repeats = int(request.form.get("repeats", "1"))
        timeout_seconds = int(request.form.get("timeout", "7200"))
        openmp_threads = int(request.form.get("openmp_threads", "4"))
        action = request.form.get("action")

        if action == "compile":
            compile_results = compile_programs()
        elif action == "clear":
            clear_results()
            results = []
        elif not is_md5(target_hash):
            error = "Please enter a valid 32-character MD5 hash."
        elif not (BASE_DIR / dictionary).exists():
            error = f"Dictionary file not found: {dictionary}"
        else:
            action_to_key = {
                "run_serial": "serial",
                "run_openmp": "openmp",
                "run_mpi": "mpi",
                "run_hybrid": "hybrid",
            }
            method_key = action_to_key.get(action)
            method = method_by_key(method_key)
            if method:
                new_result = run_method(method, dictionary, target_hash, repeats, timeout_seconds, openmp_threads)
                new_result["target_hash"] = target_hash
                new_result["dictionary"] = dictionary
                results = [row for row in results if row.get("key") != method_key]
                results.append(new_result)
                save_results(results)

    serial_time = next((row["time"] for row in results if row["key"] == "serial"), None)
    for row in results:
        if serial_time and row["time"] and row["time"] > 0:
            row["speedup"] = serial_time / row["time"]
            row["efficiency"] = row["speedup"] / row["workers"]
        else:
            row["speedup"] = None
            row["efficiency"] = None

    return render_template_string(
        HTML,
        target_hash=target_hash,
        dictionary=dictionary,
        repeats=repeats,
        timeout=timeout_seconds,
        openmp_threads=openmp_threads,
        error=error,
        results=results,
        has_serial=serial_time is not None,
        compile_results=compile_results,
    )


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000, debug=False, use_reloader=False)
