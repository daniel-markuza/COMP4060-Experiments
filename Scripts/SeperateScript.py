import re


def process_file(input_filename):
    device1_lines = []
    device2_lines = []
    device3_lines = []

    pattern = re.compile(r"RAW:(-\d{2}),(\d)_((?:START:\d{10})|(?:T\d{10}V\d{4}))")

    with open(input_filename, 'r') as infile:
        for line in infile:
            line = line.strip()
            match = pattern.match(line)
            if match:
                rssi, device_id, data = match.groups()
                if device_id == '1':
                    device1_lines.append(line)
                elif device_id == '2':
                    device2_lines.append(line)
                elif device_id == '3':
                    device3_lines.append(line)


    with open("deviceFirstRun.txt", 'w') as dev1:
        dev1.write("\n".join(device1_lines) + "\n" if device1_lines else "")

    with open("deviceFirstRun.txt", 'w') as dev2:
        dev2.write("\n".join(device2_lines) + "\n" if device2_lines else "")

    with open("device3ThirdRun.txt", 'w') as dev3:
        dev3.write("\n".join(device3_lines) + "\n" if device3_lines else "")


if __name__ == "__main__":
    input_filename = "blast 3.txt"
    process_file(input_filename)