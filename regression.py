import re
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from sklearn.linear_model import LinearRegression

files = [
    'device1FirstRun.txt',
    'device1SecondRun.txt',
    'device1ThirdRun.txt',
    'device1FourthRun.txt'
]

def parse_file(file_path):
    voltages = []
    percentages = []
    ts = []

    with open(file_path, 'r') as f:
        lines = f.readlines()

    for line in lines:
        match = re.search(r'T(\d+)V(\d+)', line)
        if match:
            time_ms = int(match.group(1))
            voltage_mv = int(match.group(2))
            ts.append(time_ms)
            voltages.append(voltage_mv / 1000.0)
            
    min_t, max_t = min(ts), max(ts)
    percentages = [100 * (1 - (t - min_t) / (max_t - min_t)) for t in ts]
    return voltages, percentages

all_voltages = []
all_percentages = []

for file in files:
    v, p = parse_file(file)
    all_voltages.extend(v)
    all_percentages.extend(p)

X = np.array(all_voltages).reshape(-1, 1)
y = np.array(all_percentages)

reg = LinearRegression()
reg.fit(X, y)

x_range = np.linspace(min(all_voltages), max(all_voltages), 100).reshape(-1, 1)
y_pred = reg.predict(x_range)

plt.figure(figsize=(8, 6))
plt.scatter(all_voltages, all_percentages, alpha=0.5, label='Data Points')
plt.plot(x_range, y_pred, label='Linear Fit', color='red', linewidth=2)
plt.xlabel('Voltage (V)')
plt.ylabel('Battery Percentage (%)')
plt.legend()
plt.grid(True)
plt.title('Voltage vs Battery Percentage Regression')

plt.savefig('voltage_vs_battery_percentage.png', dpi=300)
plt.close()

print(f'Battery Percentage = {reg.coef_[0]:.2f} * Voltage + {reg.intercept_:.2f}')
print('Plot saved as voltage_vs_battery_percentage.png')
