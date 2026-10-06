#!/usr/bin/env python3
import numpy as np
import matplotlib.pyplot as plt
import time
import os

# Define constants
MAX_VAL = 0x00FFFFFF  # 24-bit counter mask
PROGRESS_STEP = 8 * 1024 * 1024  # 8M samples per progress step (adjust if necessary)

# Path to the capture file
capture_file = "capture.bin"

# Create a figure for plotting
plt.ion()  # Turn on interactive mode
fig, ax = plt.subplots(figsize=(10, 6))
line, = ax.plot([], [], label='Incremental Data', color='b')

# Set plot labels and title
ax.set_xlabel("Number of Words")
ax.set_ylabel("Increment Value")
ax.set_title("Real-Time Incremental Data Plot")
ax.set_ylim(0, MAX_VAL)  # Set y-axis range for 24-bit values

# Function to update the plot
def update_plot(x_data, y_data):
    line.set_xdata(x_data)
    line.set_ydata(y_data)
    ax.relim()
    ax.autoscale_view()
    plt.draw()
    plt.pause(0.1)  # Pause for a short time to update the plot

# Read data from the capture file in chunks (real-time simulation)
def plot_real_time():
    x_data = []
    y_data = []

    # Open the capture file for reading
    with open(capture_file, "rb") as f:
        while True:
            # Read a chunk of data (adjust the chunk size if needed)
            data = np.fromfile(f, dtype='<u4', count=PROGRESS_STEP)  # Read 32-bit words

            # If no data, exit the loop
            if len(data) == 0:
                break

            # Extract 24-bit counter values
            counter = data & MAX_VAL

            # Append the counter values to x and y data
            x_data.extend(range(len(x_data), len(x_data) + len(counter)))
            y_data.extend(counter)

            # Update the plot with the new data
            update_plot(x_data, y_data)

            # Print progress
            print(f"Processed {len(x_data):,} words...", end='\r')

            # Check if the file has been fully written to (exit condition)
            if os.path.getsize(capture_file) == f.tell():
                break

    print("\nReal-time plot finished.")

# Start plotting in real-time
if __name__ == "__main__":
    try:
        plot_real_time()
    finally:
        plt.ioff()  # Turn off interactive mode
        plt.show()  # Ensure the final plot is displayed
