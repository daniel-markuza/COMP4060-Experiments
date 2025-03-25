import re


def process_file(input_filename):
    device1_batches = []
    device2_batches = []
    device3_batches = []

    pattern = re.compile(r"RAW:(-\d{2}),(\d)_((?:START:\d{10})|(?:T(\d{10})V(\d{4})))")

    with open(input_filename, 'r') as infile:
        device1_rssi = []
        device1_voltage = []
        device2_rssi = []
        device2_voltage = []
        device3_rssi = []
        device3_voltage = []

        for line in infile:
            line = line.strip()
            match = pattern.match(line)
            if match:
                rssi, device_id, _, runtime, voltage = match.groups()
                rssi_value = int(rssi)
                voltage_value = int(voltage) if voltage else None

                if device_id == '1':
                    device1_rssi.append(rssi_value)
                    if voltage_value is not None:
                        device1_voltage.append(voltage_value)

                    if len(device1_rssi) == 10:
                        avg_rssi = sum(device1_rssi) / 10
                        avg_voltage = sum(device1_voltage) / len(device1_voltage) if device1_voltage else 0
                        device1_batches.append(f"Average RSSI: {avg_rssi:.2f}, Average Voltage: {avg_voltage:.2f}")
                        device1_rssi.clear()
                        device1_voltage.clear()

                elif device_id == '2':
                    device2_rssi.append(rssi_value)
                    if voltage_value is not None:
                        device2_voltage.append(voltage_value)

                    if len(device2_rssi) == 10:
                        avg_rssi = sum(device2_rssi) / 10
                        avg_voltage = sum(device2_voltage) / len(device2_voltage) if device2_voltage else 0
                        device2_batches.append(f"Average RSSI: {avg_rssi:.2f}, Average Voltage: {avg_voltage:.2f}")
                        device2_rssi.clear()
                        device2_voltage.clear()

                elif device_id == '3':
                    device3_rssi.append(rssi_value)
                    if voltage_value is not None:
                        device3_voltage.append(voltage_value)

                    if len(device3_rssi) == 10:
                        avg_rssi = sum(device3_rssi) / 10
                        avg_voltage = sum(device3_voltage) / len(device3_voltage) if device3_voltage else 0
                        device3_batches.append(f"Average RSSI: {avg_rssi:.2f}, Average Voltage: {avg_voltage:.2f}")
                        device3_rssi.clear()
                        device3_voltage.clear()


    with open("deviceFourthRunAverage.txt", 'w') as dev1:
        dev1.write("\n".join(device1_batches) + "\n" if device1_batches else "")

    with open("deviceFourthRunAverage.txt", 'w') as dev2:
        dev2.write("\n".join(device2_batches) + "\n" if device2_batches else "")

    with open("device3ThirdRunAverage.txt", 'w') as dev3:
        dev3.write("\n".join(device3_batches) + "\n" if device3_batches else "")


if __name__ == "__main__":
    input_filename = "blast 3.txt"
    process_file(input_filename)
