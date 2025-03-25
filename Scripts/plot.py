import matplotlib.pyplot as plt
import glob
import re
import numpy as np
import os

def parse_file(filename):
    data = []
    with open(filename) as f:
        for line in f:
            match = re.search(r'Average Voltage: ([\d.]+)', line)
            if match:
                data.append(float(match.group(1)))
    return data

files = glob.glob("device*RunAverage.txt")
devices = {}

for file in files:
    match = re.match(r'device(\d+)(First|Second|Third)RunAverage\.txt', file)
    if match:
        device_num = int(match.group(1))
        device = f"Device {device_num}"
        run_data = parse_file(file)
        devices.setdefault(device, []).append(run_data)

output_folder = "plots"
os.makedirs(output_folder, exist_ok=True)

device_colors = {
    "Device 1": "tab:blue",
    "Device 2": "tab:green",
    "Device 3": "tab:red"
}

combined_data = {}

for device, runs in devices.items():
    min_len = min(len(run) for run in runs)
    trimmed_runs = [run[:min_len] for run in runs]

    runs_array = np.array(trimmed_runs)
    mean = np.mean(runs_array, axis=0)
    std = np.std(runs_array, axis=0)

    if device == "Device 3":
        time_step = 5 / 60
    else:
        time_step = 10

    x = np.arange(min_len) * time_step

    base_color = device_colors.get(device, 'gray')

    combined_data[device] = {
        "x": x,
        "mean": mean,
        "std": std,
        "color": base_color
    }

    plt.figure(figsize=(10, 5))

    for i, run in enumerate(trimmed_runs):
        plt.plot(x, run, label=f"Run {i+1}", color=base_color, alpha=0.5)

    plt.errorbar(x, mean, yerr=std, fmt='-o', color=base_color, label='Mean ± StdDev', capsize=3, linewidth=2)

    plt.title(f"Scenario {device.split()[-1]} - Voltage Over Time")
    plt.xlabel("Time (Minutes)")
    plt.ylabel("Average Voltage")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()

    output_path = os.path.join(output_folder, f"{device}.png")
    plt.savefig(output_path)
    plt.close()

plt.figure(figsize=(12, 6))

for device, data in combined_data.items():
    plt.errorbar(data["x"], data["mean"], yerr=data["std"], fmt='-o',
                 label=f"{device}", color=data["color"], capsize=3, linewidth=2)

plt.title("Combined - Voltage Over Time for All Scenarios")
plt.xlabel("Time (Minutes)")
plt.ylabel("Average Voltage")
plt.legend()
plt.grid(True)
plt.tight_layout()

output_path = os.path.join(output_folder, "All_Devices_Combined.png")
plt.savefig(output_path)
plt.close()

print("All individual and combined plots saved in 'plots/' directory.")
