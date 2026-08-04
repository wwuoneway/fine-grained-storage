"""Read-benchmark pivot heatmaps + wall-time bottleneck breakdown.

Importing the package selects matplotlib's headless backend, so submodules can
import pyplot freely (write files, never open a window).
"""
import matplotlib

matplotlib.use("Agg")
